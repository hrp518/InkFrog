# EPUB / FileManager / LVGL 内存问题总调试计划

## 目标

解决当前系统的两类问题，并建立一份后续可持续维护的“内存排列指南”：

1. **线程栈溢出**（`disp_task`、`lvgl_task`）
2. **SRAM heap exhausted**（字体加载、目录打开、EPUB 打开阶段）
3. 建立一份**内存查找表/布局指南**，覆盖：
   - 全局数组 / 缓冲区 / 大对象的位置
   - `.bss/.data/.xip/.psram_*` 段归属
   - RTOS 线程的栈大小 / 所在线程 / 调用职责
   - 哪些模块走 SRAM、哪些模块走 PSRAM

这份计划不做“简单回退功能”，而是解释：
- 我之前改了什么
- 为什么现在会出现新的问题
- 下一步应如何系统性收敛问题

---

# 一、当前事实与时间线

## 1.1 启动时的物理内存条件

日志给出：

```text
sram heap space [0x2507fc, 0x267c00), total size 95236 Bytes
psram heap space [0x14739c0, 0x1800000), total size 3720768 Bytes
```

结论：
- **SRAM heap 只有约 95KB**，非常紧张
- **PSRAM heap 约 3.55MB**，相对充裕

所以系统设计必须遵守一条原则：

> **所有大缓冲、大数组、大文本缓存、大字体数据，都应该优先放到 PSRAM；SRAM 只能留给线程栈、RTOS 控制块、小型状态对象、驱动内部瞬态分配。**

---

## 1.2 当前日志展示出的真实问题时间线

### 阶段 A：系统启动成功
- `lv_init()` 正常
- EPD 初始化正常
- LVGL 显示驱动和触摸驱动正常
- `disp_task` 和 `lvgl_task` 都成功启动

这说明：
- 基础系统没有问题
- EPD 驱动和 LVGL 初始化没有问题

### 阶段 B：文件管理器（FM）初始化
日志显示：

```text
[FM] File Manager initializing...
[FONT] Attempting to load TTF font from SD card...
[FONT] Scanning Font directory for TTF files...
[FONT] Found TTF file: MiSans-Normal.ttf (size: 8129526 bytes)
[FONT] New smallest font: 0:/Font/MiSans-Normal.ttf
heap exhausted, incr 520, 0x267ca4 >= 0x267c00
[FONT] Reading TTF file to PSRAM: 0:/Font/MiSans-Normal.ttf
heap exhausted, incr 520, 0x267ca4 >= 0x267c00
[FONT] Failed to open TTF file, error: 17
[FONT] Using built-in misans font as fallback
```

这说明：
- `find_smallest_ttf_font()` **逻辑是成立的**
- 它已经成功扫描目录，并选中了当前最小 TTF 候选
- 真正失败的是：**读取 / 创建 TTF 字体对象阶段**
- 所以不是“找最小字体逻辑错了”，而是**在字体加载流程中 SRAM heap 已经炸了**

### 阶段 C：文件管理器刷新目录失败
随后又出现：

```text
[FM] Loading file entries for: /
heap exhausted, incr 520, 0x267c9c >= 0x267c00
[FM] Failed to open dir: /, error: 17
```

说明：
- 不是只有字体路径受影响
- 整个 FM 的目录操作也开始失败
- 这说明此时 SRAM heap 已经进入“边缘/耗尽”状态

### 阶段 D：进入 EPUB 时的栈问题
历史日志中先后出现：
- `task ... disp_task stack over flow`
- `task ... lvgl_task stack over flow`

这说明：
- 我之前修掉的 16KB 局部数组问题是真问题，但**只是其中一层**
- 现在更深层的问题是：
  - 线程栈预算偏小
  - 同时系统 SRAM heap 也不足

也就是说当前系统是：

> **栈和堆同时处于高压状态。**

---

# 二、我之前做了哪些修改，以及为什么会出现现在的问题

## 2.1 我对 EPUB viewer 的主要改动

为了修 EPUB 分页与样式错位，我引入了新的处理链：

```text
HTML temp file
  -> decoded temp file (canonical stream)
  -> page index
  -> direct decoded slice render
```

对应新增/强化的对象和逻辑：
- `build_decoded_stream()`
- `build_page_index()`
- `update_display()`
- `page_char_offsets[]`
- `page_start_styles[]`
- 统一 `page_text_buf / decoded_buf / reflowed_buf` 等工作缓冲

这条架构方向本身是对的，因为它解决的是：
- 分页坐标不一致
- 样式 marker 处理不一致
- 流式渲染和页面索引不同步

## 2.2 我引入的常驻大缓冲

当前 viewer 常驻缓冲为：
- `html_buf` = 8KB
- `stripped_buf` = 16KB
- `decoded_buf` = 16KB
- `reflowed_buf` = 16KB
- `page_text_buf` = 16KB

合计：
- **72KB 级别的大缓冲**

如果这些都落到 PSRAM，问题不大。
但如果：
- 分配控制结构
- 中间对象
- 线程栈
- FM 字体对象
- FatFs 临时分配

仍然大量走 SRAM，那么就会把仅有的 95KB SRAM heap 快速耗尽。

## 2.3 我修过一次 stack overflow

我已经做过两类修复：

### 修复 A：删除 `build_page_index()` 中两个 16KB 局部数组
原问题：
- `char work_buf[EPUB_WORK_BUF_SIZE * 2]`
- 直接打爆线程栈

我改成：
- 复用 `viewer->page_text_buf`

这个修复是正确的，必须保留。

### 修复 B：调大线程栈
我已经把：
- `disp_task`: 2048 -> 8192
- `lvgl_task`: 16384 -> 32768

这一步也没有错，但日志表明：
- 栈压力只是系统问题的一部分
- 堆问题仍然存在

## 2.4 为什么现在会变成 heap 问题显性化

因为当我把最明显的栈炸点修掉之后，系统继续往前走，就暴露出了更深层问题：

> **file_manager 字体加载 + LVGL 对象 + viewer 大缓冲 + 线程栈，共同挤压 95KB SRAM heap。**

所以当前现象不是“我改坏了字体逻辑”，而是：
- 我修了分页与栈的一部分问题
- 系统现在走得更远了
- 因而开始暴露原本被崩溃掩盖的 heap 问题

---

# 三、当前问题的本质判断

## 3.1 FileManager 的“选最小 TTF”逻辑本身没有错

从 `file_manager.c:1258-1311` 可以确认：
- `find_smallest_ttf_font()` 遍历 `0:/Font`
- 比较 `fno.fsize`
- 用最小值更新 `ttf_file_path`

这段逻辑没有被我改坏。

而日志也证明：

```text
[FONT] Found TTF file: MiSans-Normal.ttf (size: 8129526 bytes)
[FONT] New smallest font: 0:/Font/MiSans-Normal.ttf
```

说明：
- 它已经跑完了“选择候选字体”的逻辑
- 问题不在选最小逻辑

## 3.2 真正出问题的是“加载 TTF”的时机和内存路径

`file_manager.c:1319-1416` 的逻辑是：
1. 扫描 Font 目录
2. 找最小 TTF
3. `read_ttf_file_to_psram()` 读取整文件
4. `lv_tiny_ttf_create_data_ex()` 创建字体对象

这里的问题不是“逻辑错”，而是：

### 问题 A：加载时机太早
- FM 初始化阶段就开始做 TTF
- 此时系统还要承担 LVGL / EPD / HTTP / 文件列表等其它压力

### 问题 B：虽然字体数据放 PSRAM，但创建字体对象过程仍依赖 SRAM heap
- FatFs 目录遍历
- `f_open / f_read`
- tiny-ttf 内部结构
- LVGL font object
- fallback 结构

因此实际效果是：

> **TTF 文件本身进 PSRAM，不等于“整个加载过程不吃 SRAM”。**

## 3.3 当前系统是典型的“峰值叠加”问题

当前至少有四组内存压力在同一时间段重叠：

1. **LVGL 初始化后的对象树**
2. **FileManager 的字体加载**
3. **FileManager 的目录刷新与条目创建**
4. **EPUB viewer 的大缓冲 / page index / decoded stream**

而可用 SRAM heap 只有 95KB。

所以当前问题本质是：

> **内存分布与初始化时序没有错峰，导致 SRAM heap 峰值叠加。**

---

# 四、整体调试总策略

必须同时做两件事：

## 4.1 建立“可观测性”
不要继续靠猜，要建立：
- heap 时间线
- 栈使用时间线
- 大对象位置表

## 4.2 重排内存与初始化时机
不是砍功能，而是：
- 哪些对象必须常驻
- 哪些对象可以延后创建
- 哪些结构必须放 PSRAM
- 哪些线程栈需要调整

---

# 五、详细 Debug 计划（非常详细）

## Phase 0：建立统一的“内存排列指南”

这是本次必须额外建立的东西。

目标是形成一份人工可读的查找表，避免每次都在巨大的 `.map` 里肉眼翻。

## 0.1 指南结构
建议这份指南最终包含 5 张表：

### 表 A：静态全局大对象表
字段：
- 符号名
- 文件
- 类型
- 大小
- 所在段（`.bss / .data / .xip / .psram_bss / .psram_data`）
- 地址区间
- 作用

重点关注：
- `lv_draw_buf`
- `framebuffer`
- `html_buf / stripped_buf / decoded_buf / reflowed_buf / page_text_buf`
- `chapter_decoded_cache`
- `page_char_offsets / page_start_styles`
- file_manager 的分页缓存 / 列表缓存 / 字体全局对象
- tiny-ttf 全局缓存

### 表 B：线程栈表
字段：
- 线程名
- 创建点文件:行号
- 栈大小
- 线程职责
- 可能的重调用链
- 风险等级

至少包含：
- `disp_task`
- `lvgl_task`
- `http_server`
- 其它显式 `OS_ThreadCreate` 出来的线程

### 表 C：动态大分配表
字段：
- 分配函数
- 调用点
- 分配目标大小
- 走 SRAM / PSRAM / DMAHEAP_PSRAM
- 生命周期
- 释放点

至少包含：
- `psram_malloc`（字体）
- `_dma_malloc(...DMAHEAP_PSRAM)`（viewer）
- `lv_tiny_ttf_create_data_ex`
- `lv_label_create / lv_obj_create` 引发的对象分配

### 表 D：初始化时间线表
字段：
- 时间阶段
- 模块
- 关键动作
- 新增内存压力
- 是否可延迟

### 表 E：风险冲突表
列出：
- 哪些模块同一阶段同时抢 SRAM
- 哪些可以错峰

---

## 0.2 如何生成这份指南

### 输入来源
1. `LVGL_withSDandNetwork.map`
2. `appos.ld`
3. 源码中的全局数组/静态对象定义
4. `OS_ThreadCreate` 调用点
5. 动态分配调用点（`malloc / psram_malloc / _dma_malloc / lv_tiny_ttf_create_data_ex`）

### 输出方式
计划实施时建议生成一份 markdown 文档，例如：
- `MEMORY_LAYOUT_GUIDE.md` 或者项目内 debug 文档

但当前还处于 plan mode，这里只规定方案，不写文件。

---

## Phase 1：建立内存观测点（Heap 时间线）

## 1.1 在关键阶段打印 SRAM heap / PSRAM heap 剩余量

建议增加统一 helper，例如：
- 打印 SRAM free/used
- 打印 PSRAM free/used

插入点：
1. `main()` 刚进 LVGL 前
2. `file_manager_init()` 入口
3. `find_smallest_ttf_font()` 前后
4. `read_ttf_file_to_psram()` 前后
5. `lv_tiny_ttf_create_data_ex()` 前后
6. `refresh_file_list()` 前后
7. `epub_viewer_create()` 前后
8. `build_page_index()` 前后
9. `build_decoded_stream()` 前后
10. `update_display()` 前后

### 目标
确认：
- 哪一步真正把 SRAM 从安全区打到临界区
- 哪一步只是“最后一根稻草”

## 1.2 加线程栈 watermark 观测

如果 XR OS / FreeRTOS 支持：
- `uxTaskGetStackHighWaterMark()` 或等效接口

给以下线程打点：
- `lvgl_task`
- `disp_task`
- `http_server`

插入点：
- 正常空闲时打印一次
- 进入 EPUB 打开前打印一次
- 进入 FM 字体加载前打印一次
- 完成分页索引后打印一次

### 目标
量化：
- `32768` 对 `lvgl_task` 是否真的足够
- `8192` 对 `disp_task` 是否真的足够
- 是否还需要继续加栈

---

## Phase 2：重新梳理线程职责，确认“重活到底跑在哪个线程”

## 2.1 确认当前调用链

从日志和代码看，目前真正触发 EPUB 打开的路径最终落在 `lvgl_task` 调用的 `lv_timer_handler()` 驱动的事件链上。

也就是说：
- 现在不是 `disp_task` 在扛 EPUB 打开主逻辑了
- 而是 `lvgl_task` 在扛 UI 事件 + 文件管理器 + EPUB 打开 + page index

这就是为什么你最新日志变成：

```text
task ... lvgl_task stack over flow
```

## 2.2 调试目标
必须画出一张“线程责任图”：

- `lvgl_task`
  - `lv_tick_inc`
  - `lv_timer_handler`
  - 触摸事件
  - 文件管理器点击回调
  - EPUB 打开
  - page index
- `disp_task`
  - `lv_port_disp_task`
  - `epd_do_refresh`

### 结论方向
如果 EPUB 打开确实走在 `lvgl_task` 上，那就不能只调 `disp_task` 栈。

---

## Phase 3：保留“最小 TTF”设计，但重排字体加载时机

这是非常关键的一步。

## 3.1 不删除 `find_smallest_ttf_font()`

这部分逻辑保留，因为它是你明确要求保留的功能：
- 自动扫描 Font 目录
- 选最小 TTF

## 3.2 先拆分“发现字体”和“创建字体对象”

把当前流程拆成两段：

### 阶段 1：仅扫描并记录路径
保留：
- `find_smallest_ttf_font()`
- 只更新 `ttf_file_path`

但不做：
- `read_ttf_file_to_psram()`
- `lv_tiny_ttf_create_data_ex()`

### 阶段 2：真正需要阅读器字体时再创建
也就是：
- FM 只拿到“最小字体路径”这个结果
- 真进入 Reader 页面时，再加载 TTF 数据并创建 font object

### 为什么这样做
因为这能把两个峰值错开：
- FM 初始化峰值
- EPUB 打开峰值

而不破坏“选最小 TTF”的设计目标。

## 3.3 给字体系统建立状态机

建议最终字体系统分三层状态：
- `FONT_DISCOVERED`：只知道路径
- `FONT_DATA_LOADED`：TTF 原始数据已在 PSRAM
- `FONT_OBJECT_CREATED`：LVGL 字体对象已建立

这样后续才能决定：
- FM 需要哪一层
- Reader 需要哪一层

---

## Phase 4：重新规划 EPUB Viewer 的常驻内存

## 4.1 明确 viewer 当前常驻缓冲

当前 viewer 常驻：
- 8KB `html_buf`
- 16KB `stripped_buf`
- 16KB `decoded_buf`
- 16KB `reflowed_buf`
- 16KB `page_text_buf`
- page index arrays
- decoded cache（小章节）

## 4.2 调试目标
逐项确认这些缓冲：
- 是否全部由 `_dma_malloc(... DMAHEAP_PSRAM)` 获得
- 是否真的位于 PSRAM 地址区（`0x014xxxxx`）
- 生命周期是否合理

## 4.3 要建立 viewer 内存表
表中至少要写：
- 名称
- 大小
- 分配位置
- 释放位置
- 走 PSRAM 还是 SRAM

## 4.4 后续可能的优化方向
如果 heap 压力仍大：
- 考虑把 `stripped_buf / decoded_buf / reflowed_buf / page_text_buf` 中某些缓冲复用成同一块“多用途工作区”
- 但这是第二阶段优化，不作为第一轮修复

---

## Phase 5：线程栈的一步到位策略

你要求“一步到位”，所以要先算范围。

## 5.1 当前线程栈
- `disp_task = 8192`
- `lvgl_task = 32768`

## 5.2 现在为什么我不敢直接拍脑袋说 64KB/128KB

因为必须先确认：
- `OS_ThreadCreate` 的线程栈来自哪块内存
  - SRAM heap？
  - 独立线程栈区？
  - 静态任务池？

如果它来自 **SRAM heap**，那：
- `lvgl_task 32768 -> 65536`
- 就会再吃掉 32KB SRAM
- 在只有 95KB SRAM heap 的前提下，可能直接把系统顶死

### 所以第一步不是盲加栈，而是先确认“栈从哪里来”

这是本阶段必须查清的一项。

## 5.3 如果确认线程栈走 SRAM heap
那么策略应是：
- `lvgl_task` 只小幅提高，例如 40KB / 48KB
- 同时必须配合“字体延迟加载”一起做
- 绝不能只靠无限加栈硬顶

## 5.4 如果确认线程栈不走 SRAM heap 或影响可接受
则可以直接把：
- `lvgl_task -> 64KB`
- `disp_task -> 12KB`

作为一轮快速验证值。

---

## Phase 6：定位 heap exhausted 的真正归属

## 6.1 错误 17 的含义要确认
日志里多次出现：
- `Failed to open TTF file, error: 17`
- `Failed to open dir: /, error: 17`

需要确认 FatFs 在你们 SDK 下 `17` 的实际含义：
- `FR_NOT_ENOUGH_CORE`？
- `FR_INVALID_OBJECT`？
- 还是包装层自己的 errno？

因为这个会直接决定：
- 是“内存不足导致 FatFs 失败”
- 还是“对象状态已损坏”

## 6.2 把“目录扫描”和“TTF 对象创建”分开验证

后续实现时建议分别验证：
1. 只扫描目录，不创建 TTF 对象
2. 不扫描目录，直接用已知路径创建 TTF 对象
3. 不创建 TTF 对象，只保持 file_manager 页面
4. 再叠加 EPUB 打开

### 目标
确认究竟是：
- 目录扫描本身吃内存
- 还是 TTF 对象创建吃内存
- 还是二者叠加才爆

---

## Phase 7：建立“内存排列指南”具体内容

这是你额外要求的重点，我单独列出最终交付结构。

## 7.1 需要收集的内容

### A. 静态段分布（来自 map + 链接脚本）
按段整理：
- `.text`
- `.rodata`
- `.data`
- `.bss`
- `.xip`
- `.psram_text`
- `.psram_data`
- `.psram_bss`

### B. 全局数组与表
重点抽取：
- 文件路径缓存
- framebuffer
- lv_draw_buf
- EPUB viewer 五块工作缓冲
- FM 的分页数组、文件项数组、字体缓存
- tiny-ttf 的全局指针与缓存

### C. 动态分配点
列出所有：
- `malloc/calloc/free`
- `psram_malloc/free`
- `_dma_malloc/_dma_free`
- `lv_mem_alloc`
- `lv_tiny_ttf_create_data_ex`

### D. 线程栈表
列出所有 `OS_ThreadCreate`：
- 名称
- 栈大小
- 优先级
- 线程职责
- 风险调用链

### E. 内存压力时间线
按模块初始化顺序：
1. platform
2. network/http
3. lvgl/epd/touch
4. file_manager
5. font scan / font load
6. epub viewer

记录每个阶段：
- 进入前 free SRAM
- 退出后 free SRAM
- 进入前 free PSRAM
- 退出后 free PSRAM

---

# 六、最终建议的执行顺序

## 第一轮：只做观测，不做功能回退
1. 确认线程栈来源
2. 增加 heap / stack watermark 日志
3. 建立线程责任图
4. 建立内存排列指南初稿

## 第二轮：做最小内存时序重排
5. 保留“选最小 TTF”逻辑
6. 将 TTF 真正加载/建对象推迟到 Reader 需要时
7. 保持 FM 只发现字体，不立即建字体对象

## 第三轮：必要时再做容量调整
8. 根据观测结果决定 `lvgl_task` 是否增到 40KB / 48KB / 64KB
9. 只有在确认收益明确时才继续加线程栈

## 第四轮：整理查找表
10. 形成最终 Memory Layout Guide，供后续快速排查

---

# 七、这次计划的底线原则

1. **不砍“最小 TTF 自动选择”功能**
2. **不以“退回内置字体”作为正式解决方案**
3. **先做观测，再做时序重排，再做容量调整**
4. **把 map 文件的大海捞针，沉淀成可读的内存查找表**

---

# 八、计划完成后的验收标准

## 验收 A：功能不回退
- 仍能保留“自动选择 Font 目录中最小 TTF”的能力
- Reader 仍支持 H1/H2/H3 样式路线

## 验收 B：内存稳定
- FM 初始化阶段不再 `heap exhausted`
- EPUB 打开阶段不再 stack overflow
- 目录打开、TTF 打开、分页索引都能继续执行

## 验收 C：可维护性提升
- 有一份独立可读的 Memory Layout Guide
- 能快速回答：某个数组/缓冲/线程栈在哪里、多大、归属哪块内存

## 验收 D：日志可定位
- heap / stack watermark 能显示谁在什么阶段吃掉了多少内存

---

# 九、当前一句话结论

当前不是“字体逻辑错了”，也不是“EPUB 逻辑单独错了”，而是：

> **FileManager 字体加载、EPUB viewer 大缓冲、LVGL 线程栈三者同时挤压 SRAM，形成了系统级内存分布问题。**

后续要按这个系统级方向修，而不是再做单点拍脑袋回退。