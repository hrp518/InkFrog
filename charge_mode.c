/*
 * charge_mode 实现 - 充电模式
 *
 * 详见 charge_mode.h 设计说明。复用 clock_mode 的冷启动+RTC magic 架构，
 * 以及 screensaver 的外设关闭序列。
 */
#include "charge_mode.h"
#include "epd.h"
#include "lv_port_disp.h"           /* pm_diag_dump_noirq_list() 休眠诊断 */
#include "driver/chip/hal_rtc.h"
#include "driver/chip/hal_wakeup.h"
#include "driver/chip/hal_prcm.h"
#include "driver/chip/hal_gpio.h"
#include "driver/chip/hal_adc.h"
#include "pm/pm.h"
#include "fs/fatfs/ff.h"
#include "driver/chip/sdmmc/sdmmc.h"
#include "driver/chip/sdmmc/hal_sdhost.h"
#include "wifi_controller.h"
#include "chsc6540.h"
#include "screensaver.h"             /* screensaver_render_to_epd() */
#include "common/framework/fs_ctrl.h" /* fs_ctrl_mount() 挂载 SD 读屏保 */
#include "common/framework/sys_ctrl/sys_ctrl.h" /* sys_ctrl_create() */
#include "sys/xr_debug.h"            /* ROM_WRN_MASK/ROM_ERR_MASK/ROM_ANY_MASK */
#include "kernel/os/os_thread.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

#define CHARGE_REFRESH_INTERVAL_SEC  60   /* 每 60s 刷新一次电量 */

/*============================================================
 * 模式标志 (RTC weekday magic)
 *============================================================*/

bool charge_mode_enabled(void)
{
    RTC_WeekDay wday;
    uint8_t h, m, s;
    HAL_RTC_GetDDHHMMSS(&wday, &h, &m, &s);
    return (uint32_t)wday == CHARGE_MAGIC_WDAY;
}

void charge_mode_set_enabled(bool en)
{
    RTC_WeekDay wday;
    uint8_t h, m, s;
    HAL_RTC_GetDDHHMMSS(&wday, &h, &m, &s);
    /* 清除时写回 0(MONDAY)，保留时分秒 */
    HAL_RTC_SetDDHHMMSS(en ? (RTC_WeekDay)CHARGE_MAGIC_WDAY : RTC_WDAY_MONDAY,
                        h, m, s);
}

/*============================================================
 * 电量读取 (独立最小 ADC 初始化)
 *============================================================*/

/* 复用 main.c 的电压→SOC 换算逻辑 (简化版) */
static int charge_voltage_to_soc(float v)
{
    if (v >= 4.15f) return 100;
    if (v >= 4.00f) return 90 + (int)((v - 4.00f) / 0.15f * 10);
    if (v >= 3.60f) return 10 + (int)((v - 3.60f) / 0.40f * 80);
    return 5;
}

static int charge_read_soc(void)
{
    ADC_InitParam param;
    memset(&param, 0, sizeof(param));
    param.mode = ADC_CONTI_CONV;
    param.freq = 1000;
    param.delay = 0;
    HAL_ADC_Init(&param);

    uint32_t raw = 0;
    int ret = HAL_ADC_Conv_Polling(ADC_CHANNEL_VBAT, &raw, 100);
    HAL_ADC_DeInit();
    if (ret != 0) return 0;

    /* 12-bit ADC, 2.5V 参考, VBAT 通道比例=3 */
    float v = (float)raw * 2.5f * 3.0f / 4096.0f;
    return charge_voltage_to_soc(v);
}

/*============================================================
 * 充电界面渲染
 *============================================================*/

/* 画水平线/垂直线/矩形 (复用 EPD_SetPixel, color=0 黑) */
static void epd_hline(uint16_t x, uint16_t y, uint16_t w)
{
    for (uint16_t i = 0; i < w; i++) EPD_SetPixel(x + i, y, 0);
}
static void epd_vline(uint16_t x, uint16_t y, uint16_t h)
{
    for (uint16_t i = 0; i < h; i++) EPD_SetPixel(x, y + i, 0);
}
static void epd_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    epd_hline(x, y, w);
    epd_hline(x, y + h - 1, w);
    epd_vline(x, y, h);
    epd_vline(x + w - 1, y, h);
}
/* 填充矩形 (color: 0=黑, 1=白) */
static void epd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t color)
{
    for (uint16_t j = 0; j < h; j++)
        for (uint16_t i = 0; i < w; i++)
            EPD_SetPixel(x + i, y + j, color);
}

/* 画电池图标: 外框 + 电量填充(按 soc 比例) + 闪电符号(充电中)
 * bx,by 电池左上角, bw 宽, bh 高 */
static void charge_draw_battery(uint16_t bx, uint16_t by, uint16_t bw, uint16_t bh, int soc)
{
    uint16_t tip_w = 10;   /* 电池正极小柱 */
    uint16_t pad = 6;      /* 内边距 */

    /* 电池正极小柱 (右侧凸起) */
    epd_fill_rect(bx + bw, by + bh / 2 - tip_w, tip_w, tip_w * 2, 0);

    /* 电池外框 */
    epd_rect(bx, by, bw, bh);

    /* 电量填充: 根据 soc 填充内部 */
    if (soc < 0) soc = 0;
    if (soc > 100) soc = 100;
    uint16_t fill_w = (bw - 2 * pad) * soc / 100;
    if (fill_w > 0) {
        epd_fill_rect(bx + pad, by + pad, fill_w, bh - 2 * pad, 0);
    }

    /* 闪电符号(充电中): 画在电池中央, 用白色挖出闪电形状表示正在充电 */
    if (soc < 100) {
        uint16_t lx = bx + bw / 2;
        uint16_t ly = by + bh / 2;
        /* 简化: 画一个白色闪电轮廓 (在黑色填充区挖出视觉效果) */
        for (int dy = -25; dy < 25; dy++) {
            int half = (dy < 0) ? (8 + dy / 4) : (8 - dy / 4);
            for (int dx = -half; dx <= half; dx++) {
                if (lx + dx >= bx + pad && lx + dx < bx + bw - pad &&
                    ly + dy >= by + pad && ly + dy < by + bh - pad) {
                    EPD_SetPixel(lx + dx, ly + dy, 1);  /* 白色闪电 */
                }
            }
        }
    }
}

static void charge_render_and_refresh(void)
{
    /* 充电画面: 只显示 "Charge" 文字, 全程只刷一次 EPD。
     * 不读 SOC (冷启动 ADC 不准), 不画百分比/电池图标。 */
    printf("[CHARGE] render charge screen\r\n");

    /* EPD_DrawStringCentered 现支持 C/h/g 字母, 可显示 "Charge!" */
    EPD_DrawStringCentered("Charge!");

    /* 统一 DU 快刷(正常极性白底黑字), 之后 EPD deep sleep 保持画面, 不再刷新 */
    EPD_3IN52_Init();
    EPD_3IN52_Display();
    EPD_Sleep();
    printf("[CHARGE] EPD refreshed (once)\r\n");
}

/*============================================================
 * SD 子系统就绪 (拔充电器渲染屏保前)
 *============================================================*/

/* 充电路径进入时 SD 子系统已被拆除或从未初始化:
 *   - minimal 路径 (charge_mode_run_minimal) 只跑了 platform_init_level0,
 *     sys_ctrl 队列 / fs_ctrl mutex / SDC host / EXT LDO(SD供电) 全都没有,
 *     直接 fs_ctrl_mount 会 mutex handle 0 + mmc_rescan "init sdc host first!!",
 *     f_open 返回 12 (FR_NOT_ENABLED)。
 *   - 完整路径 (charge_mode_enter) 已 HAL_SDC_Deinit, SDC host 需重建。
 * 本函数幂等, 一次冷启动内最多执行一次。
 *
 * 【2026-08 修复】不再重建 SD:
 *   HAL_SDC_Deinit 会把 _mci_host[0] 指向的 struct mmc_host 清零并注销
 *   SDC 的 PM ops; 而 HAL_SDC_Create 对"已存在"的 host 只打印
 *   "has already created!" 就原样返回, 不会恢复 param/debug_mask/dma_use。
 *   用这种半初始化状态 fs_ctrl_mount → mmc_rescan 必然失败, 且带着该状态
 *   进 HIBERNATION 会在 ROM 的 dpm_suspend_noirq 链上踩空指针 → hard fault
 *   (PC=0, UFSR=0x2), 表现为"拔插头后整机卡死"。所以拔插头路径不再碰 SD,
 *   直接退化文字屏保 (与 PA6 进休眠的已验证路径一致)。 */

/* 拔充电器后显示屏保：充电模式进入时 SD 已被拆除, 这里【不重建 SD】,
 * 复用 screensaver.c 的公开 API 渲染到 EPD 并 deep sleep,
 * 避免充电画面长时间残留导致 EPD 烧屏。SD 未挂载时 f_open 失败,
 * screensaver_render_to_epd 自动退化为居中 "Tuwa Reader" 文字屏保。 */
static void charge_render_screensaver(void)
{
    printf("[CHARGE] render screensaver\n");
    screensaver_render_to_epd();
    printf("[CHARGE] screensaver shown, EPD sleeping\n");
}

/*============================================================
 * 唤醒源配置
 *============================================================*/

/* 充电模式武装: PA6(退出) + PA21 上升沿(拔出充电器)。
 * 不设 WKTIMER — 充电模式周期性刷新会频繁唤醒 MCU 耗电,
 * 且刷新后 EPD 白屏闪烁几秒,用户体验差。充电时电量上升
 * 不需要实时更新,显示一次即足够。 */
static void charge_arm_wakeup(void)
{
    /* PA6 = WKIO2，下降沿(按键按下)，上拉 */
    HAL_Wakeup_SetIO(2, WKUPIO_WK_MODE_FALLING_EDGE, GPIO_PULL_UP);
    /* PA21 = WKIO7，上升沿(低→高=拔出充电器)，上拉 */
    HAL_Wakeup_SetIO(7, WKUPIO_WK_MODE_RISING_EDGE, GPIO_PULL_UP);
    HAL_Wakeup_SetSrc(PM_WAKEUP_SRC_WKIO2 | PM_WAKEUP_SRC_WKIO7);
    printf("[CHARGE] wakeup armed: PA6 + PA21(rising)\r\n");
}

/* 普通休眠武装(已拔出，回休眠等下次插入):
 * PA6 + PA21 下降沿(高→低=插入充电器) */
static void charge_arm_sleep_wakeup(void)
{
    HAL_Wakeup_SetIO(2, WKUPIO_WK_MODE_FALLING_EDGE, GPIO_PULL_UP);  /* PA6 */
    HAL_Wakeup_SetIO(7, WKUPIO_WK_MODE_FALLING_EDGE, GPIO_PULL_UP);  /* PA21 插入 */
    HAL_Wakeup_SetSrc(PM_WAKEUP_SRC_WKIO2 | PM_WAKEUP_SRC_WKIO7);
}

/*============================================================
 * 充电模式主循环 (MCU 持续活跃, 轮询 PA21/PA6)
 *============================================================*/

/* 充电模式轮询循环: 不休眠, MCU 保持活跃, 只轮询两个 GPIO。
 * EPD 已显示充电画面并进入 deep sleep(画面保持), 不需要刷新。
 * 充电电流足够大, 不在乎 MCU 活跃功耗。 */
static void charge_poll_loop(void)
{
    printf("[CHARGE] entering poll loop (awake)\r\n");
    while (1) {
        /* PA21=高(非充电) → 拔出充电器, 回休眠 */
        if (HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_21) != 0) {
            printf("[CHARGE] charger removed, showing screensaver then sleep\r\n");
            /* 拔充电器后先刷新屏保画面，避免长时间停留在充电画面造成
             * EPD 残影/烧屏。EPD 已 deep sleep，需重新初始化再显示。 */
            charge_render_screensaver();
            charge_mode_set_enabled(false);
            charge_arm_sleep_wakeup();
            pm_diag_dump_noirq_list();   /* 诊断: 挂起链崩溃排查 */
            pm_enter_mode(PM_MODE_HIBERNATION);
            while (1) {}
        }
        /* PA6=低(按下) → 进普通模式 */
        if (HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_6) == 0) {
            printf("[CHARGE] PA6 pressed, entering normal mode\r\n");
            charge_mode_set_enabled(false);
            /* 等按键释放 */
            while (HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_6) == 0) {
                OS_MSleep(20);
            }
            return;   /* 返回调用方, 后续创建首页 */
        }
        OS_MSleep(200);
    }
}

/*============================================================
 * 主动进入充电模式 (从 disp_task 调用, 外设还在运行)
 *============================================================*/

void charge_mode_enter(void)
{
    extern OS_Thread_t lvgl_thread;
    extern void lv_timer_handler_unblock_after_suspend(void);

    printf("[CHARGE] entering charge mode\r\n");
    charge_mode_set_enabled(true);

    /* 关外设 (复用 screensaver_enter 的序列, 避免后台 EPD 刷新) */
    wifi_controller_stop();
    pm_unregister_wlan_power_onoff();
    wifi_controller_poweroff();
    CHSC6540_DeInit();
    HAL_ADC_DeInit();
    /* SD 卡【保持挂载、不拆除】:
     *  - 拔充电器要读 /ScreenSaver/screensaver.bin 显示自定义壁纸, 卡必须
     *    保持挂载 (旧代码 fs_ctrl_unmount + HAL_SDC_Deinit 拆掉后, 重建
     *    SDC host 会踩 "already created" 半初始化 host, mount 必失败);
     *  - HAL_SDC_Deinit 会注销 SDC 的 PM ops, 带被改动的 DPM 列表进
     *    HIBERNATION, ROM 的 dpm_suspend_noirq 挂起链踩损坏项 → hard fault。
     *    保持 SDC 注册, 由 __mci_suspend 在挂起链里给 SD 下电。 */

    /* 渲染充电画面 (直接操作 framebuffer + EPD) */
    charge_render_and_refresh();

    /* 挂起 lvgl_task: 充电模式 MCU 必须保持活跃(给充电芯片供电),
     * 但 lvgl_task 不能继续跑(会触发 EPD 刷新/font_warm, 和充电画面冲突)。
     * 用 vTaskSuspend 和 epd_take_ustainability 一样的机制。 */
    vTaskSuspend(lvgl_thread.handle);
    printf("[CHARGE] lvgl_task suspended\n");

    /* 进入轮询循环 (MCU 活跃, 不休眠) */
    charge_poll_loop();

    /* PA6 按下 → 恢复 lvgl_task, return 到调用方。
     * 注意: charge_poll_loop return 意味着要继续正常启动,
     * 但外设已关, 需要冷启动重初始化。所以这里直接 reboot 更安全。 */
    printf("[CHARGE] PA6 pressed, rebooting to normal mode\r\n");
    charge_mode_set_enabled(false);
    /* 武装 PA6 唤醒(下降沿已触发, 这里配 PA21 下降沿等下次插入) */
    charge_arm_sleep_wakeup();
    pm_enter_mode(PM_MODE_HIBERNATION);
    while (1) {}
}

/*============================================================
 * 冷启动充电入口 (最小初始化版本, 从 main() 早判调用)
 * 前置: platform_init_level0 + EPD_GPIO_Init_Public 已完成。
 * PA6 按下时 return (调用方继续正常启动)。
 *============================================================*/

void charge_mode_run_minimal(void)
{
    printf("[CHARGE] minimal charge mode\r\n");
    /* PA6/PA21 GPIO 必须在轮询前配置为输入+上拉！
     * 之前依赖 platform_init_level0 的默认状态, 但 PA21 未配置成
     * GPIO 输入导致 HAL_GPIO_ReadPin 读不到充电状态, 拔出充电器
     * 永远检测不到 → 无法回休眠。 */
    {
        GPIO_InitParam gparam;
        memset(&gparam, 0, sizeof(gparam));
        gparam.mode = GPIOx_Pn_F0_INPUT;
        gparam.driving = GPIO_DRIVING_LEVEL_1;
        gparam.pull = GPIO_PULL_UP;
        HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_6, &gparam);
        HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_21, &gparam);
        printf("[CHARGE] PA6/PA21 GPIO configured as input pull-up\n");
    }

    /* 渲染充电画面 */
    charge_render_and_refresh();

    /* 进入轮询循环 */
    charge_poll_loop();
    /* PA6 按下 → return, main() 继续 platform_init_level1/2 等正常启动 */
}

void charge_mode_run(void)
{
    /* 完整启动版本的充电模式 (从 main_ui_create 前调用, 全系统已初始化)。
     * 目前不使用 — PA21 早判已走 charge_mode_run_minimal。保留接口以防需要。 */
    charge_mode_run_minimal();
}
