# FontExp PSRAM 内存报告

> **项目**: XR872 / FontExp EPUB 阅读器 + LVGL + tiny_ttf  
> **固件日志时间**: 2026-07-08 16:06（分支 `mem-fix`）  
> **目的**: 供外部模型审阅——解释开书前 PSRAM 为何只剩 ~1.67MB、L1 glyf 为何只装入 2128/3629 字、DMA heap 占用与优化方向。

---

## 1. 硬件与链接布局

| 项目 | 值 |
|------|-----|
| PSRAM 物理总量 | **4,194,304 B (4 MB)** |
| PSRAM 基址 | `0x01400000` |
| 链接脚本 | `gcc/.project.ld` |

PSRAM 被链接器切成 **四段互不相通的内存**：

```
0x01400000  ┌──────────────────────────────────────────┐
            │ .psram_text   代码 + 只读数据    ~199 KB   │
0x01431b40  ├──────────────────────────────────────────┤
            │ .psram_data                      ~1 KB     │
0x01432018  ├──────────────────────────────────────────┤
            │ .psram_bss    静态大缓冲         ~865 KB  │
0x0150a270  ├──────────────────────────────────────────┤
            │ ★ psram_heap（psram_malloc 池）  ~2.04 MB │
            │   psram_GetFreeHeapSize() 统计这里         │
0x01700000  ├──────────────────────────────────────────┤
            │ ★ DMA heap（_dma_malloc 池）     1 MB     │
            │   与 psram_heap 完全隔离                   │
0x01800000  └──────────────────────────────────────────┘
```

**关键符号**（来自 `FontExp.map`）：

| 符号 | 地址/大小 |
|------|-----------|
| `__psram_text` | `0x01400000`, size `0x31B40` (203,584 B) |
| `__psram_bss_start__` | `0x01432018` |
| `__psram_bss_end__` / `__psram_end__` | `0x0150A270` |
| `.psram_bss` 总大小 | `0xD8258` (885,336 B) |
| `__DMAHEAP_PSRAM_BASE` | `0x01700000` |
| `__DMAHEAP_PSRAM_LENGTH` | `0x100000` (1,048,576 B) |
| **psram_heap 池大小** | `0x01700000 - 0x0150A270` = **2,089,360 B (~2.04 MB)** |

> ⚠️ `heap_debug.c` 里的 `psram_heap free` **不包含** `.psram_bss`、`.psram_text`、DMA heap。  
> 用户看到「4MB 里只剩 1.67MB」实际是 **2MB 堆池里剩 1.67MB**，另有 ~2MB 在堆外已永久占用。

---

## 2. 静态占用明细（.psram_bss + .psram_text）

### 2.1 `.psram_bss` 应用侧（来自 map 按对象）

| 对象/模块 | 大小 | 说明 |
|-----------|------|------|
| lwip / wlan / mbedtls 等库 bss | **~87 KB** | 链接脚本强制网络栈 bss 进 PSRAM |
| `settings_storage.o` | 12,288 B | INI 读写静态缓冲 (`SETTINGS_MAX_FILE_SIZE=4096` ×2) |
| `epub_reader.o` | 71,680 B | `s_raw_entries[256]` ZIP 原始索引 |
| `epd.o` | 12,450 B | `framebuffer[240×415/8]` 墨水屏位图 |
| `lv_port_disp.o` | 99,600 B | `lv_draw_buf[240×415]` LVGL 全屏绘制 |
| `epub_viewer.o` | **307,200 B** | `whole_xhtml 200KB` + `decoded_text 100KB` |
| `lv_mem.o` | **262,144 B** | `LV_MEM_SIZE = 256KB`（`lv_conf.h`） |
| `miniz.o` | 32,768 B | inflate 字典 `TINFL_LZ_DICT_SIZE` |
| **合计（应用可见项）** | **~798 KB** | |
| **.psram_bss 段总量** | **885,336 B** | 含库 bss 等未逐条列出项 |

### 2.2 `.psram_text` 要点

| 内容 | 大小 |
|------|------|
| 段总量 | 203,584 B |
| `level1_chars[]`（3500 字 Unicode 表） | 14,000 B (`0x36B0`) |
| 其余 | LVGL/应用 XIP 到 PSRAM 的代码 |

### 2.3 静态占用汇总

| 类别 | 大小 | 计入 psram_heap free? |
|------|------|----------------------|
| .psram_text | ~199 KB | ❌ |
| .psram_data | ~1 KB | ❌ |
| .psram_bss | ~865 KB | ❌ |
| DMA heap 保留区 | **1024 KB** | ❌（另一分配器） |
| **堆外合计** | **~2.09 MB** | — |
| psram_heap 池 | **~2.04 MB** | ✅ 仅此池统计 free |

---

## 3. 动态占用（psram_heap / psram_malloc）

### 3.1 开书前快照（日志 `before_prepare_reader_fonts`）

```
PSRAM Size:        4,194,304 B
psram_heap free:   1,667,120 B  (~1.63 MB)
```

推算：

```
psram_heap 池总量     ≈ 2,089,360 B
开书前已用            ≈   422,240 B  (2,089,360 - 1,667,120)
```

### 3.2 开书前 psram_heap 主要消费者

| 消费者 | 大小 | 分配点 | 生命周期 |
|--------|------|--------|----------|
| **`file_entries[500]`** | **~386,000 B** | `file_manager.c` → `init_pagination()` | 进文件管理器即分配，开书前不释放 |
| 其他（LVGL 对象走内置池、零碎 malloc） | ~36 KB | — | — |

**FileEntry 结构**（`file_manager.c`）：

```c
typedef struct {
    char name[256];
    char full_path[512];
    uint8_t is_dir;
} FileEntry;   // ≈772 B/条 × 500 = 386,000 B
```

> **结论**: 开书前 psram_heap 已用 ~90% 是文件列表数组；与 EPUB/字体无关，但挤占 glyf 预算。

### 3.3 字体预加载后快照（`after_prepare_reader_fonts`）

```
psram_heap free:   357,952 B  (~350 KB)
字体栈总占用:     ≈ 1,309,168 B  (1,667,120 - 357,952)
```

---

## 4. 字体 L1 预加载（tiny_ttf + font_priority_loader）

### 4.1 预加载规模

| 项目 | 值 |
|------|-----|
| level1 字表 | 3500 CJK + 95 ASCII + 标点扩展 = **3629** |
| 有效 glyph | 3629（0 not found） |
| **实际装入 L1** | **2128 / 3629 (58.7%)** |
| glyf 紧凑需求（全量） | **1,308,894 B** |
| glyf 实际装入 | **711,000 B** |
| 冷字池（全局） | **1501 字** glyf 未进 PSRAM |

### 4.2 预加载阶段 psram_heap 增量分配

| 对象 | 大小 | 说明 |
|------|------|------|
| `metrics_cache` | 293,888 B | CJK U+4E00–U+9FFF，`20992 × 14 B` |
| `ascii_metrics_cache` | ~1,330 B | 95 个 ASCII |
| **loca 常驻** | 124,112 B | `ttf_persist_table_cache`，budget 前已 alloc |
| **hmtx 常驻** | 124,108 B | Step 3 读表后 pin |
| **cmap 常驻** | 19,365 B | batch lookup 后 pin |
| **glyf compact** | 711,000 B | L1 字形轮廓 |
| `level1_glyph_info[3629]` | ~58,064 B | `3629 × 16 B`，优先 `_dma_malloc` 失败则 psram |
| `g_glyf_lookup` | 34,048 B | `2128 × 16 B` |
| head/hhea/os2 小表 | ~200 B 级 | 可忽略 |

**pinned 小表合计**: 267,585 B

### 4.3 glyf budget 裁剪逻辑

```c
// lv_tiny_ttf.c
budget = (psram_GetFreeHeapSize() - 48KB - pinned_tables - 16KB) * 85%
```

**开书时实测**（glyf malloc 前 `free = 1,169,600`）：

| 项 | 值 |
|----|-----|
| free（已扣 metrics + loca） | 1,169,600 B |
| reserve (48+268+16 KB) | 333,121 B |
| ×85% 安全系数 | → **budget = 711,007 B** |
| need（3629 字全量） | 1,308,894 B |
| trim 结果 | **2128 glyphs, 711,000 B** |

**已知问题**:

1. **loca 双重扣减**: loca 已在 `free` 里 alloc 过，budget 的 `pinned_tables` 又含 loca (~124 KB)。
2. **85% 安全系数** 再削 ~105 KB 可用 glyf。
3. **优先级裁剪**: 数字→标点→ASCII→CJK；尾部 CJK 先被裁。

### 4.4 运行时「冷字」定义

- **冷字** = glyf **不在** L1 PSRAM 的字；渲染/排版需读 SD，约 **70–110 ms/字**。
- **热字** = 在 L1 内，render **0–6 ms**。
- 每页约 **21–26 unique 冷字**，bitmap miss 约 **60**（含多字号重复）。

---

## 5. DMA heap 占用（_dma_malloc, 1 MB 池）

与 `psram_heap` **完全隔离**；`heap_debug.c` 仅打印 Base/Size，**无 free 统计 API**。

### 5.1 调用点汇总

| 模块 | 大小 | 常驻/临时 | 触发时机 |
|------|------|-----------|----------|
| **epub_reader** `epub_decomp_buffer` | **128 KB** | **常驻** | `main()` → `epub_buffer_init()` 开机即 alloc |
| **http_server** `g_http_buffer` | 2 KB | HTTP 运行期 | 设置页启动 HTTP |
| **http_server** `g_http_response` | 32 KB | HTTP 运行期 | 同上 |
| **http_server** 屏保上传 | 12.4 KB | 临时 | 单次请求 |
| **http_server** 文件发送 | 2 KB | 临时 | 单次请求 |
| **http_server** multipart 上传 | 16 KB | 临时 | 单次请求 |
| **epub_reader** `comp_buf` / `stage_buf` | 可变 | 临时 | ZIP 解压每条目 |
| **lv_tiny_ttf** `ttf_scratch_alloc` | 可变 | 临时/后备 | psram_malloc 失败时 fallback |
| **lv_tiny_ttf** `glyphs_keep` | ~58 KB | 常驻（若走 DMA） | 当前日志 `dma=0`，实际走 psram_heap |

### 5.2 典型 DMA 占用估算

| 场景 | 常驻 | 峰值临时 |
|------|------|----------|
| 平时阅读（HTTP 关） | **128 KB** | +0 |
| 开 EPUB 解压 | 128 KB | +comp/stage（数十～数百 KB） |
| HTTP 开启 | 162 KB | +上传 16 KB |

> **1 MB DMA 池平时约用 12%**；~900 KB 为 linker 预留、应用未使用。  
> 此 1 MB **不能**给 `psram_malloc` 装 glyf，除非改 linker 或迁移分配策略。

---

## 6. 全链路内存时间线（开 life.epub）

```
T0  系统启动
    ├─ .psram_bss 865KB 已占用（含 epub_viewer 300KB，未开书也在）
    ├─ DMA: epub_buffer_init() → 128KB
    └─ psram_heap free ≈ 2MB（尚未 init 文件管理器）

T1  file_manager_init() → init_pagination()
    └─ psram_malloc(file_entries 500) → -386KB

T2  用户点 EPUB（before_prepare_reader_fonts）
    └─ psram_heap free = 1,667,120 B

T3  字体预加载
    ├─ metrics_cache        -294 KB
    ├─ loca pin             -124 KB
    ├─ glyf budget trim → 2128 字
    ├─ glyf malloc          -711 KB
    ├─ hmtx/cmap pin        -143 KB
    └─ lookup/metadata      -~90 KB

T4  after_prepare_reader_fonts
    └─ psram_heap free = 357,952 B

T5  epub_reader_create (psram_malloc sizeof EpubReader)
    └─ viewer 大缓冲已在 .psram_bss，不再占 heap
```

---

## 7. 为何装不满 3629 字 glyf（根因链）

```
4 MB PSRAM
  └─ 堆外永久占用 ~2.09 MB（bss 865KB + text 199KB + DMA 1MB）
  └─ psram_heap 池 ~2.04 MB
       └─ 开书前 file_entries 占 ~386 KB  → free 1.67 MB
       └─ 预加载再占 metrics+tables+glyf ~1.31 MB
       └─ 剩余 ~358 KB

全量 glyf 需要 1.31 MB + pinned 0.27 MB + metrics 0.29 MB ≈ 1.87 MB
  > 开书前 free 1.67 MB（即使零 reserve 也不够）

再叠加 budget 85% + loca 双重扣减 + CJK 低优先级
  → 实际只装 2128 字 (711 KB glyf)
```

---

## 8. 优化建议（按 ROI 排序）

| # | 改动 | 预期释放 | 风险 |
|---|------|----------|------|
| 1 | 开书前 `deinit_pagination()` 或缩小 `MAX_FILE_ENTRIES` | **~300–386 KB** psram_heap | 低；开书不需要文件列表 |
| 2 | `epub_buffer_init()` 延迟到首次开书 | **128 KB** DMA（若合并进 heap 则 +128KB psram） | 中；需测解压峰值 |
| 3 | linker DMA heap **1MB → 512KB**，归还 psram_heap | **~512 KB** | 中；需测 HTTP 上传 + EPUB 解压峰值 |
| 4 | 修 glyf budget：loca 不双重扣减；85%→92% | **~100–200 KB** 可多装字 | 低–中；测 malloc 碎片 |
| 5 | `WHOLE_XHTML`/`DECODED_TEXT` 改按需 alloc | **300 KB** .psram_bss | 中；改 epub_viewer 生命周期 |
| 6 | `LV_MEM_SIZE` 256KB → 128KB | **128 KB** .psram_bss | 中；测复杂 UI |
| 7 | level1 字表按全书词频重排 | 同容量多覆盖高频字 | 高；需离线统计 |

---

## 9. 关键源文件索引

| 文件 | 内容 |
|------|------|
| `gcc/.project.ld` | PSRAM / DMA heap 分区 |
| `heap_debug.c` | `psram_GetFreeHeapSize()` 打印 |
| `lv_conf.h` | `LV_MEM_SIZE = 256KB` |
| `epub_viewer.h` | `WHOLE_XHTML 200KB`, `DECODED 100KB` |
| `file_manager.c` | `MAX_FILE_ENTRIES=500`, `FileEntry`, MEM 探针 |
| `epub_reader.c` | `EPUB_DECOMP_BUF_SIZE 128KB`, DMA bump |
| `http_server.c` | HTTP 2KB + response 32KB |
| `lv_tiny_ttf.c` | glyf budget / trim / pinned tables / L1 cache |
| `font_priority_loader.c` | 3629 字预加载列表 |
| `level1_data.c` | 3500 CJK unicode 表 |

---

## 10. 实测日志摘录（2026-07-08 16:06）

```
[MEM] ==== before_prepare_reader_fonts ====
  PSRAM Size: 4194304 bytes
  psram_heap free: 1667120 bytes
  DMA Heap Size: 1024 KB

[TTF] pinned tables reserve: loca=124112 hmtx=124108 cmap=19365 total=267585
[TTF] glyf need 1308894 bytes > budget 711007 (pinned_tables=267585), trimming...
[TTF] glyf budget trim: loaded 2128 glyphs, 711000 bytes (budget 711007)
[TTF_DBG] batch: psram_heap free before glyf=1169600 need=711000
[TTF] metrics_cache 293888 bytes on psram
[TTF] L1 preload result: 2128/3629 glyphs cached in psram (711000 bytes glyf)

[MEM] ==== after_prepare_reader_fonts ====
  psram_heap free: 357952 bytes

[VIEWER] Buffers static (.psram_bss): whole_xhtml=204800, decoded_text=102400
```

---

## 11. 待 GPT 审阅的问题

1. **budget 公式**是否应改为：`budget = free - hmtx_pending - cmap_pending - slack`（排除已 alloc 的 loca）？
2. **DMA 1MB 保留**是否过大？EPUB+HTTP 峰值需要多少？
3. **file_entries 386KB** 是否应迁 SRAM / 按需加载 / 开书前释放？
4. **epub_viewer 300KB 静态 bss** 与 **epub DMA 128KB** 是否存在功能重叠可合并？
5. 在 **不增 PSRAM 硬件** 前提下，2128→3000+ 字 L1 的最短路径是什么？

---

*报告生成依据：FontExp.map、.project.ld、TTF龟速显示.txt 16:06 日志、lv_tiny_ttf.c / file_manager.c / epub_reader.c 源码静态分析。*
