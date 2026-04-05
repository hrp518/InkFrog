
---

## 2026/3/22 07:00 - LVGL_withSD 项目创建完成

### 任务目标
基于lvgl_intergration项目，创建一个新的LVGL_withSD项目，添加SD卡支持功能。

### 项目路径
home/Administrator/xradio-skylark-sdk-xradio-skylark-sdk-1.0.2/project/demo/LVGL_withSD/

### 创建步骤

1. **复制项目**
   `ash
   cp -r lvgl_intergration LVGL_withSD
   `

2. **修改prj_config.h**
   - 路径: project/demo/LVGL_withSD/prj_config.h
   - 修改: PRJCONF_MMC_EN 从 0 改为 1
   - 作用: 启用SD卡驱动

3. **修改main.c**
   - 路径: project/demo/LVGL_withSD/main.c
   - 添加: #include <stdlib.h>
   - 添加: SD卡测试函数sd_benchmark_test() - 简化版，提示用户使用控制台命令
   - 在platform_init()后调用sd_benchmark_test()

### 编译结果
- 编译成功 ✓
- text: 574344, data: 2692, bss: 121404
- IMG镜像已生成: xr_system.img (640856 bytes)

### SD卡使用方式

**通过控制台命令测试SD卡:**
`
drv sd init     - 初始化SD卡
drv sd scan     - 扫描SD卡
drv sd test     - 运行SD卡测试
drv sd bench    - 运行性能测试
`

**FatFs文件系统:**
- 物理驱动器0 (pdrv=0) 对应SD卡 (DEV_MMC)
- 使用控制台命令 atfs mount 0 挂载文件系统
- 使用 atfs ls 0: 列出文件

### IMG镜像生成问题解决

**问题:** mkimage生成IMG时报告bin 1和bin 2重叠错误

**原因:** app.bin (129672 bytes) 和 app_xip.bin (447364 bytes) 在flash中的位置有重叠

**解决:** 使用自动校准的配置文件image_auto_cal.cfg替代原image.cfg:
`ash
cd project/demo/LVGL_withSD/image/xr872
cp image_auto_cal.cfg image.cfg
mkimage -O -c image.cfg -o xr_system.img
`

### 关键文件
- prj_config.h - PRJCONF_MMC_EN=1 启用SD卡
- main.c - sd_benchmark_test()输出使用说明
- image.cfg - 使用image_auto_cal.cfg覆盖解决重叠问题

---

## 2026/3/22 07:17 - SD卡GPIO配置修改为GPIOB第一组(PB4/PB5/PB7)

### 任务目标
将SD卡GPIO从GPIOB端口第二组(PB16-PB18)改为第一组(PB4/PB5/PB7)，并启用4位数据模式。

### 修改内容

**文件:** project/common/board/xr872_evb_ai/board_config.c (line 183-198)

**修改前:**
`c
#define BOARD_SD0_DATA_BITS   1
...
static const GPIO_PinMuxParam g_pinmux_sd0[BOARD_SD0_DATA_BITS + 2] = {
    { GPIO_PORT_B, GPIO_PIN_16, { GPIOB_P16_F3_SD_CMD, ... } },  /* CMD */
    { GPIO_PORT_B, GPIO_PIN_18, { GPIOB_P18_F3_SD_CLK, ... } },  /* CLK */
    { GPIO_PORT_B, GPIO_PIN_17, { GPIOB_P17_F3_SD_DATA0, ... } }, /* D0 */
    // D1-D3 注释掉
};
`

**修改后:**
`c
#define BOARD_SD0_DATA_BITS   4
...
static const GPIO_PinMuxParam g_pinmux_sd0[BOARD_SD0_DATA_BITS + 2] = {
    { GPIO_PORT_B, GPIO_PIN_4,  { GPIOB_P4_F3_SD_CMD,  ... } },  /* CMD */
    { GPIO_PORT_B, GPIO_PIN_7,  { GPIOB_P7_F3_SD_CLK,  ... } },  /* CLK */
    { GPIO_PORT_B, GPIO_PIN_5,  { GPIOB_P5_F3_SD_DATA0, ... } },  /* D0 */
    { GPIO_PORT_B, GPIO_PIN_19, { GPIOB_P19_F3_SD_DATA1, ... } },  /* D1 */
    { GPIO_PORT_B, GPIO_PIN_20, { GPIOB_P20_F3_SD_DATA2, ... } },  /* D2 */
    { GPIO_PORT_B, GPIO_PIN_21, { GPIOB_P21_F3_SD_DATA3, ... } },  /* D3 */
};
`

### SD卡GPIO配置总结

| 配置组 | CMD | CLK | D0 | D1 | D2 | D3 | 数据位宽 |
|--------|-----|-----|----|----|----|----|----------|
| **GPIOB第一组(当前)** | PB4 | PB7 | PB5 | PB19 | PB20 | PB21 | 4位 |
| GPIOB第二组(原配置) | PB16 | PB18 | PB17 | - | - | - | 1位 |

### 编译结果
- 编译成功 ✓
- text: 576636, data: 2692, bss: 121404
- IMG镜像已生成: xr_system.img (643928 bytes)

### 关键引脚说明
- **GPIOB_P4_F3_SD_CMD**: SD命令信号
- **GPIOB_P7_F3_SD_CLK**: SD时钟信号  
- **GPIOB_P5_F3_SD_DATA0**: SD数据线0
- **GPIOB_P19_P20_P21_F3_SD_DATA1_2_3**: SD数据线1/2/3(4位模式)

### 注意事项
- 启用4位数据模式需要硬件上PB19/PB20/PB21正确连接到SD卡槽
- 如果硬件只连接了1位模式(D0)，需要将BOARD_SD0_DATA_BITS改回1

---

## 2026/3/22 07:21 - GPIOB第一组(PB4/PB5/PB7)测试失败 - 已回退

### 测试结果
**PB4/PB5/PB7这组GPIO不可用！** SD卡初始化时发生硬fault崩溃。

### 崩溃信息
`
exception:6 happen!!
usage fault happen, UFSR:0x1
PC:0x4651d0 (指向无效内存)
`

### 结论
**只有GPIOB第二组(PB16-PB18)可用**，当前配置：

`c
#define BOARD_SD0_DATA_BITS   1
static const GPIO_PinMuxParam g_pinmux_sd0[BOARD_SD0_DATA_BITS + 2] = {
    { GPIO_PORT_B, GPIO_PIN_16, { GPIOB_P16_F3_SD_CMD, ... } },  /* CMD */
    { GPIO_PORT_B, GPIO_PIN_18, { GPIOB_P18_F3_SD_CLK, ... } },  /* CLK */
    { GPIO_PORT_B, GPIO_PIN_17, { GPIOB_P17_F3_SD_DATA0, ... } }, /* D0 */
};
`

### 可用的SD卡GPIO配置
| 配置组 | CMD | CLK | D0 | D1 | D2 | D3 | 数据位宽 |
|--------|-----|-----|----|----|----|----|----------|
| **GPIOB第二组(可用)** | PB16 | PB18 | PB17 | - | - | - | 1位 |

### 编译结果
- 编译成功 ✓
- text: 576620, data: 2692, bss: 121404
- IMG镜像: xr_system.img (643928 bytes)

---

## 2026/3/23 10:28 - 文件管理器功能添加完成

### 任务目标
为LVGL_withSD项目添加文件管理器功能，支持：
- 目录浏览（基于lv_list）
- 文件操作（打开文本、重命名、删除）
- 墨水屏优化（禁用动画和过渡效果）

### 新增文件
- `project/demo/LVGL_withSD/file_manager.h` - 文件管理器头文件
- `project/demo/LVGL_withSD/file_manager.c` - 文件管理器实现

### 修改文件
- `lv_conf.h` - 开启LV_USE_TEXTAREA、LV_USE_KEYBOARD、LV_USE_MSGBOX
- `lv_port_disp.c` - 添加epd_mark_refresh_pending()函数实现
- `main.c` - 添加文件管理器按钮和回调

### 关键函数
- `file_manager_init()` - 初始化文件管理器
- `file_manager_close()` - 关闭文件管理器
- `refresh_file_list()` - 刷新文件列表
- `open_text_viewer()` - 打开文本查看器
- `show_delete_confirm()` - 显示删除确认

### 编译结果
- text: 594904, data: 3036, bss: 124288
- IMG镜像已生成: xr_system.img (662360 bytes)

### 遇到的问题和解决
1. `lvgl.h`路径错误 → 改为`lvgl/lvgl.h`
2. `ff.h`路径错误 → 改为`fs/fatfs/ff.h`
3. `lv_msgbox_get_active_btn_index`不存在 → 改用自定义对话框方案
4. `epd_mark_refresh_pending`函数未实现 → 在lv_port_disp.c中添加实现
5. bin文件重叠错误 → 使用image_auto_cal.cfg解决

### 注意事项
- 中文显示需要额外添加中文字体文件
- 文本查看器最多读取前2KB内容

---

## 2026/3/23 14:46 - 物理返回按键支持添加

### 功能描述
添加物理返回按键（CHSC6540触摸IC的特殊信号`FF 0X 64 28`）支持：
- 按下返回键时，设置标志位
- 抬手时触发回调，避免抖动问题
- 若已在根目录，退出文件管理器返回首页
- 若在子目录，返回上级目录

### 修改文件
- `lv_port_indev.c` - 添加返回按键检测和回调机制
- `lv_port_indev.h` - 添加函数声明
- `file_manager.c` - 添加physical_back_btn_handler()回调函数

### 关键代码
```c
// chsc6540.c中检测返回按键信号:
if (g_rx_data[0] == 0xFF && g_rx_data[2] == 0x64 && g_rx_data[3] == 0x28) {
    if (g_rx_data[1] == 0x01) {
        return -2;  // Back Button Pressed
    } else if (g_rx_data[1] == 0x00) {
        return -3;  // Back Button Released
    }
}

// lv_port_indev.c中处理:
if (touch_cnt == -2) {
    back_btn_pressed = 1;  // 设置标志等待released信号
} else if (touch_cnt == -3) {
    // Released信号到达，直接触发回调
    if (back_btn_pressed && back_btn_callback != NULL) {
        back_btn_callback();
    }
    back_btn_pressed = 0;
}
```

### 信号说明
- `FF 01 64 28` = Back Button Pressed
- `FF 00 64 28` = Back Button Released

### 编译结果
- text: 595436, data: 3036, bss: 124296
- IMG镜像: xr_system.img (663384 bytes)

### 注意事项
- 使用released信号触发回调，无需额外抬手检测
- 避免高频抖动问题

---

## 2026/3/22 09:44 - SD卡EXT_LDO电压稳定化修改

### 问题描述
SD卡初始化时报
