/*
 * charge_mode 实现 - 充电模式
 *
 * 详见 charge_mode.h 设计说明。复用 clock_mode 的冷启动+RTC magic 架构，
 * 以及 screensaver 的外设关闭序列。
 */
#include "charge_mode.h"
#include "epd.h"
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
    int soc = charge_read_soc();
    char buf[8];
    int len;

    if (soc >= 100) {
        len = snprintf(buf, sizeof(buf), "100");
    } else {
        len = snprintf(buf, sizeof(buf), "%d", soc);
    }
    printf("[CHARGE] soc=%d%%\r\n", soc);

    EPD_ClearBuffer();

    /* 1. 顶部大电池图标 (横向, 居中偏上) */
    {
        uint16_t bw = 160;  /* 电池主体宽 */
        uint16_t bh = 70;   /* 电池主体高 */
        uint16_t bx = (240 - bw - 10) / 2;  /* 减去正极柱宽 */
        uint16_t by = 90;
        charge_draw_battery(bx, by, bw, bh, soc);
    }

    /* 2. 居中下方大号百分比数字 (EPD_DrawDigitLarge, 48x96) */
    {
        int digit_w = 48;
        int digit_h = 96;
        int total_w = len * digit_w;
        int start_x = (240 - total_w) / 2;
        int start_y = 200;
        for (int i = 0; i < len; i++) {
            EPD_DrawDigitLarge(start_x + i * digit_w, start_y, buf[i]);
        }
        /* 百分号画在数字右侧 */
        {
            uint16_t px = start_x + total_w + 4;
            uint16_t py = start_y + 30;
            /* 画一个 % 符号: 两个小圆圈 + 斜线 */
            epd_rect(px, py, 14, 14);          /* 上圆 */
            epd_rect(px + 4, py + 30, 14, 14); /* 下圆 */
            /* 斜线 */
            for (int i = 0; i < 36; i++) {
                EPD_SetPixel(px + 2 + i / 3, py + 48 - i, 0);
            }
        }
    }

    /* 充电界面刷新次数少(每分钟一次)，用 GC 全刷更清晰、清残影 */
    EPD_3IN52_Init();
    EPD_3IN52_Display();
    EPD_Sleep();
    printf("[CHARGE] EPD refreshed\r\n");
}

/*============================================================
 * 唤醒源配置
 *============================================================*/

/* 充电模式武装: PA6(退出) + PA21 上升沿(拔出) + WKTIMER(定期刷新) */
static void charge_arm_wakeup(void)
{
    HAL_Wakeup_ClrTimer();
    /* PA6 = WKIO2，下降沿(按键按下)，上拉 */
    HAL_Wakeup_SetIO(2, WKUPIO_WK_MODE_FALLING_EDGE, GPIO_PULL_UP);
    /* PA21 = WKIO7，上升沿(低→高=拔出充电器)，上拉 */
    HAL_Wakeup_SetIO(7, WKUPIO_WK_MODE_RISING_EDGE, GPIO_PULL_UP);
    HAL_Wakeup_SetSrc(PM_WAKEUP_SRC_WKTIMER | PM_WAKEUP_SRC_WKIO2 | PM_WAKEUP_SRC_WKIO7);
    HAL_Wakeup_SetTimer_Sec(CHARGE_REFRESH_INTERVAL_SEC);
    printf("[CHARGE] wakeup armed: WKTIMER %lus + PA6 + PA21(rising)\r\n",
           (unsigned long)CHARGE_REFRESH_INTERVAL_SEC);
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
 * 冷启动周期入口
 *============================================================*/

void charge_mode_cycle(void)
{
    uint8_t h, m, s;
    HAL_RTC_GetDDHHMMSS(NULL, &h, &m, &s);
    printf("[CHARGE] cycle wake: %02u:%02u:%02u\r\n", h, m, s);

    /* 检查是否还在充电 (PA21=0=充电中)。
     * 冷启动后 GPIO 状态: hibernation 期间 VDDIO 保持，PA21 输入电平有效。 */
    if (HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_21) != 0) {
        /* PA21=高 = 已拔出充电器 → 清标志，回普通休眠 */
        printf("[CHARGE] charger removed, back to sleep\r\n");
        charge_mode_set_enabled(false);
        charge_arm_sleep_wakeup();
        pm_enter_mode(PM_MODE_HIBERNATION);
        while (1) {}
    }

    /* 还在充电 → 刷新电量界面 → 再睡 */
    charge_render_and_refresh();
    charge_arm_wakeup();
    printf("[CHARGE] entering HIBERNATION\r\n");
    pm_enter_mode(PM_MODE_HIBERNATION);
    while (1) {}
}

/*============================================================
 * 主动进入充电模式 (从 disp_task 调用)
 *============================================================*/

void charge_mode_enter(void)
{
    printf("[CHARGE] entering charge mode\r\n");
    charge_mode_set_enabled(true);

    /* 关外设 (复用 screensaver_enter 的序列，不关 EXT LDO) */
    wifi_controller_stop();
    pm_unregister_wlan_power_onoff();
    wifi_controller_poweroff();

    CHSC6540_DeInit();
    HAL_ADC_DeInit();

    f_mount(NULL, "0:", 0);
    struct mmc_card *card = mmc_card_open(0);
    if (card != NULL) {
        if (mmc_card_present(card)) {
            mmc_card_deinit(card);
        }
        mmc_card_close(0);
    }
    HAL_SDC_Deinit(0);

    /* 渲染首帧充电界面 */
    charge_render_and_refresh();

    /* 配置唤醒 + 进 HIBERNATION */
    charge_arm_wakeup();
    printf("[CHARGE] entering HIBERNATION\r\n");
    pm_enter_mode(PM_MODE_HIBERNATION);
    while (1) {}
}
