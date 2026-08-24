/*
 * clock_mode - 休眠时钟模式实现 (基于 GPT clock.txt 方案)
 *
 * 状态流程(GPT 方案 §一)：
 *   正常 InkFrog --用户选择--> 进入时钟模式
 *     保存 weekday=7 标志
 *     停止 WiFi/SD/LVGL
 *     刷新一次时钟
 *     EPD 深度睡眠
 *     设置 WKTIMER + PA6 唤醒
 *     进入 HIBERNATION
 *        |
 *        +-- WKTIMER 唤醒 --> 最小化启动 --> 更新时间 --> 再次休眠
 *        +-- PA6 唤醒    --> 清除时钟模式 --> 正常启动 InkFrog
 *
 * HIBERNATION 唤醒后系统重启，不会回到 pm_enter_mode() 之后继续执行。
 * 因此分钟周期是"冷启动 -> 渲染 -> 刷 -> 睡"的循环。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>

#include "clock_mode.h"
#include "epd.h"
#include "lv_port_disp.h"
#include "time_sync.h"
#include "chsc6540.h"
#include "wlan_manager.h"
#include "wifi_controller.h"
#include "http_server.h"
#include "settings_storage.h"

#include "kernel/os/os.h"
#include "driver/chip/hal_gpio.h"
#include "driver/chip/hal_nvic.h"
#include "driver/chip/hal_rtc.h"
#include "driver/chip/hal_wakeup.h"
#include "driver/chip/hal_prcm.h"
#include "driver/chip/hal_adc.h"
#include "pm/pm.h"
#include "fs/fatfs/ff.h"
#include "driver/chip/sdmmc/sdmmc.h"
#include "driver/chip/sdmmc/hal_sdhost.h"

/* PA6 按键(唤醒/退出) - 与 main.c 一致 */
#define CLOCK_BUTTON_PIN    GPIO_PIN_6
/* WUPIO 索引 2 -> PA6 (ARCH_VER==2 映射) */
#define CLOCK_WAKE_IO_IDX   2

/* CST +8h */
#define CST_OFFSET_SEC      (8 * 3600)

/* RTC weekday 子字段正常取值 0-6；用 7 作为"时钟模式"magic(3bit 字段可表达)。 */
#define CLOCK_MAGIC_WDAY    ((RTC_WeekDay)7)

/* 时钟渲染布局(240 宽 x 415 高)：
 *   4 个 48px 数字 + 1 个 24px 冒号，数字间距 4px。
 *   总宽 = 48*4 + 24 + 4*4 = 232，居中左边距 = (240-232)/2 = 4。 */
#define CLOCK_DIGIT_W       EPD_DIGIT_W      /* 48 */
#define CLOCK_DIGIT_H       EPD_DIGIT_H      /* 96 */
#define CLOCK_COLON_W       EPD_COLON_W      /* 24 */
#define CLOCK_SPACING       4
#define CLOCK_TOTAL_W       (CLOCK_DIGIT_W*4 + CLOCK_COLON_W + CLOCK_SPACING*4)  /* 232 */
#define CLOCK_X0            ((240 - CLOCK_TOTAL_W) / 2)                          /* 4 */
#define CLOCK_Y0            ((415 - CLOCK_DIGIT_H) / 2)                          /* ~159 */

/* boot guard：PA6 唤醒退出时钟模式后，disp_task 应忽略首次原始按下 */
static volatile bool s_boot_key_guard = false;

/* "请求进入时钟模式"标志：由 lvgl 按钮回调 set，由 disp_task 消费执行。
 * 不能在 lvgl 上下文直接进 HIBERNATION，必须 deferred 到 disp_task。 */
static volatile bool s_clock_enter_pending = false;

/*============================================================
 * RTC 时间桥
 *============================================================*/

/* 读 RTC 时分秒 */
static void clock_rtc_read(uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    RTC_WeekDay wday;
    uint8_t h = 0, m = 0, s = 0;
    HAL_RTC_GetDDHHMMSS(&wday, &h, &m, &s);
    if (hour)   *hour = h;
    if (minute) *minute = m;
    if (second) *second = s;
}

/*
 * NTP 校时后写入 RTC。此时 WiFi 已连接。
 *  - gettimeofday 取系统 UTC 秒(由 time_sync_from_ntsc 写入)
 *  - 加 CST(+8h)得到本地时间
 *  - gmtime 拆分(避免再叠 TZ)
 *  - 同时写 weekday=7 作为时钟模式 magic
 */
static void clock_rtc_set_from_ntp(void)
{
    struct timeval tv;
    struct tm *tm;
    uint8_t is_leap, year, month, mday;

    printf("[CLOCK] NTP sync before entering clock mode\r\n");
    if (time_sync_from_ntsc() != 0) {
        printf("[CLOCK] NTP failed, RTC may be stale\r\n");
        /* 仍继续：即使时间不准则画面仍能跑，用户可手动退出 */
    }

    if (gettimeofday(&tv, NULL) != 0) {
        printf("[CLOCK] gettimeofday failed\r\n");
        return;
    }
    time_t local = tv.tv_sec + CST_OFFSET_SEC;
    tm = gmtime(&local);
    if (!tm) {
        printf("[CLOCK] gmtime failed\r\n");
        return;
    }

    /* 写日期(用 RTC 现有 isLeap/year 作为占位，主要是让 RTC 日历不漂) */
    HAL_RTC_GetYYMMDD(&is_leap, &year, &month, &mday);
    HAL_RTC_SetYYMMDD(is_leap,
                      (uint8_t)(tm->tm_year % 100),
                      (uint8_t)(tm->tm_mon + 1),
                      (uint8_t)tm->tm_mday);

    /* 写时分秒 + magic weekday(7) */
    HAL_RTC_SetDDHHMMSS(CLOCK_MAGIC_WDAY,
                        (uint8_t)tm->tm_hour,
                        (uint8_t)tm->tm_min,
                        (uint8_t)tm->tm_sec);
    printf("[CLOCK] RTC set %02d:%02d:%02d (magic wday=7)\r\n",
           tm->tm_hour, tm->tm_min, tm->tm_sec);
}

/*============================================================
 * 模式标志 (RTC weekday magic)
 *============================================================*/

bool clock_mode_enabled(void)
{
    RTC_WeekDay wday;
    uint8_t h, m, s;
    HAL_RTC_GetDDHHMMSS(&wday, &h, &m, &s);
    return ((uint32_t)wday == (uint32_t)CLOCK_MAGIC_WDAY);
}

void clock_mode_set_enabled(bool en)
{
    RTC_WeekDay wday;
    uint8_t h, m, s;
    HAL_RTC_GetDDHHMMSS(&wday, &h, &m, &s);
    if (en) {
        wday = CLOCK_MAGIC_WDAY;
    } else {
        /* 退出：写回合法 weekday(0-6)。用真实 epoch 算，避免再用 magic。
         * 若无有效系统时间，用 0(Monday) 作安全回退。 */
        struct timeval tv;
        if (gettimeofday(&tv, NULL) == 0) {
            struct tm *tm = gmtime((const time_t *)&(tv.tv_sec));
            if (tm) {
                /* tm_wday: 0=Sunday..6=Saturday; RTC_WDAY: 0=Monday..6=Sunday */
                int rtc_wday = (tm->tm_wday == 0) ? RTC_WDAY_SUNDAY : (tm->tm_wday - 1);
                wday = (RTC_WeekDay)rtc_wday;
            } else {
                wday = RTC_WDAY_MONDAY;
            }
        } else {
            wday = RTC_WDAY_MONDAY;
        }
    }
    HAL_RTC_SetDDHHMMSS(wday, h, m, s);

    /* 退出时钟模式时, 重建真实日期, 避免停在配置标记(88年)上。
     * 有系统时间用真实日期标准化, 否则写一个合理占位。 */
    {
        uint8_t is_leap = 0, year = 20, month = 1, mday = 1;
        struct timeval tv;
        if (gettimeofday(&tv, NULL) == 0) {
            struct tm *tm = gmtime((const time_t *)&(tv.tv_sec));
            if (tm) {
                year  = (uint8_t)(tm->tm_year % 100);
                month = (uint8_t)(tm->tm_mon + 1);
                mday  = (uint8_t)tm->tm_mday;
            }
        }
        if (en) {
            /* 进入时日期由 clock_rtc_set_from_ntp 写, 此处不再动 */
            return;
        }
        HAL_RTC_SetYYMMDD(is_leap, year, month, mday);
    }
}

/*============================================================
 * 唤醒源配置 (GPT 方案 §三/§七)
 *============================================================*/

static int clock_configure_wakeup(uint32_t delay_sec)
{
    int32_t ret;

    HAL_Wakeup_ClrTimer();

    /* PA6 = WKIO2，下降沿唤醒，上拉。退出时钟模式用。 */
    HAL_Wakeup_SetIO(CLOCK_WAKE_IO_IDX,
                     WKUPIO_WK_MODE_FALLING_EDGE,
                     GPIO_PULL_UP);
    /* PA21 = WKIO7，下降沿唤醒(插入充电器)。充电时保持唤醒, 休眠中插入要能唤回。 */
    HAL_Wakeup_SetIO(7, WKUPIO_WK_MODE_FALLING_EDGE, GPIO_PULL_UP);

    /* 同时启用 WKTIMER(分钟周期) + WKIO2(PA6 退出) + WKIO7(PA21 插入充电) */
    HAL_Wakeup_SetSrc(PM_WAKEUP_SRC_WKTIMER | PM_WAKEUP_SRC_WKIO2 | PM_WAKEUP_SRC_WKIO7);

    /* WKTIMER 最大 ~18.6h，60s 范围无问题。最小 10ms。 */
    ret = HAL_Wakeup_SetTimer_Sec(delay_sec);
    if (ret != 0) {
        printf("[CLOCK] SetTimer(%lus) failed: %ld\r\n",
               (unsigned long)delay_sec, (long)ret);
        return -1;
    }
    printf("[CLOCK] wakeup armed: WKTIMER %lus + PA6 + PA21\r\n", (unsigned long)delay_sec);
    return 0;
}

/*============================================================
 * 唤醒精度(分钟)
 *============================================================*/

int clock_get_interval_min(void)
{
    char buf[16];
    if (settings_get_string("clock", "interval_min", buf, sizeof(buf)) == 0) {
        int v = atoi(buf);
        if (v == 1 || v == 3 || v == 5 || v == 10 || v == 15 || v == 30 || v == 60) {
            return v;
        }
    }
    return 1;
}

int clock_set_interval_min(int minutes)
{
    char buf[16];
    int v = minutes;
    if (v != 1 && v != 3 && v != 5 && v != 10 && v != 15 && v != 30 && v != 60) {
        v = 1;
    }
    snprintf(buf, sizeof(buf), "%d", v);
    return settings_set_string("clock", "interval_min", buf);
}

/* 休眠时钟横屏模式 */
int clock_get_landscape(void)
{
    char buf[8];
    if (settings_get_string("clock", "landscape", buf, sizeof(buf)) == 0) {
        return (buf[0] == '1') ? 1 : 0;
    }
    return 0;
}

int clock_set_landscape(int on)
{
    return settings_set_string("clock", "landscape", on ? "1" : "0");
}

int clock_format_rtc_time(char *out, size_t sz)
{
    uint8_t h = 0, m = 0, s = 0;
    clock_rtc_read(&h, &m, &s);
    if (!out || sz == 0) return -1;
    snprintf(out, sz, "%02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
    return 0;
}

/* RTC 配置存储(精度/横屏) —— 记入日期字段, 供无 SD 的分钟唤醒路径读取。
 * 仅在时钟模式期间使用; 退出时重建真实日期(见 clock_mode_set_enabled)。 */
#define CLOCK_CFG_YEAR_MARK 88

static void clock_rtc_store_config(void)
{
    static const int span[] = {1, 3, 5, 10, 15, 30, 60};
    int iv = clock_get_interval_min();
    int ls = clock_get_landscape();
    int idx = 0;
    for (int i = 0; i < 7; i++) { if (span[i] == iv) { idx = i; break; } }
    HAL_RTC_SetYYMMDD(0,
                      CLOCK_CFG_YEAR_MARK,
                      (uint8_t)(ls ? 2 : 1),   /* month: 1=竖屏 2=横屏 */
                      (uint8_t)(idx + 1));     /* mday: 精度下标 1..7 */

    /* 写日期字段可能触发 RTC 硬件重算 weekday, 覆盖掉时钟模式 magic(wday=7),
     * 导致休眠后插入充电器被误判为普通充电唤醒而非回时钟。这里最后再断言一次
     * magic, 保证进入后 wday==7 是 RTC 最终状态, 并随休眠持久化。 */
    {
        RTC_WeekDay wd;
        uint8_t hh = 0, mm = 0, ss = 0;
        HAL_RTC_GetDDHHMMSS(&wd, &hh, &mm, &ss);
        HAL_RTC_SetDDHHMMSS(CLOCK_MAGIC_WDAY, hh, mm, ss);
    }
}

static int clock_rtc_load_config(int *interval_min, int *landscape)
{
    static const int span[] = {1, 3, 5, 10, 15, 30, 60};
    uint8_t is_leap, year, month, mday;
    HAL_RTC_GetYYMMDD(&is_leap, &year, &month, &mday);
    if (year != CLOCK_CFG_YEAR_MARK) return -1;
    if (mday < 1 || mday > 7) return -1;
    if (interval_min) *interval_min = span[mday - 1];
    if (landscape)    *landscape    = (month == 2) ? 1 : 0;
    return 0;
}

/* 分钟唤醒路径优先取 RTC 内配置(无 SD 也能读), 否则回退 SD/默认 */
static void clock_get_active_config(int *interval_min, int *landscape)
{
    if (clock_rtc_load_config(interval_min, landscape) == 0) return;
    if (interval_min) *interval_min = clock_get_interval_min();
    if (landscape)    *landscape    = clock_get_landscape();
}

/* 按当前精度算到下一个唤醒时间点(对齐到 interval 边界)，取代固定每分钟。
 * 优先读 RTC 配置(分钟唤醒无 SD 也能取回精度)。 */
static void clock_schedule_next_wake(void)
{
    uint8_t h, m, s;
    int iv = 1;
    clock_get_active_config(&iv, NULL);
    uint32_t interval = (uint32_t)iv * 60U;
    if (interval < 60U) interval = 60U;

    clock_rtc_read(&h, &m, &s);
    uint32_t total = (uint32_t)h * 3600U + (uint32_t)m * 60U + (uint32_t)s;
    uint32_t delay_sec = interval - (total % interval);
    /* 避免刚配置完就立即唤醒 */
    if (delay_sec < 2U) {
        delay_sec += interval;
    }
    clock_configure_wakeup(delay_sec);
}

/*============================================================
 * 大号时钟渲染 (GPT 方案 §四)
 *============================================================*/

/* 在 framebuffer 画 "HH:MM"，居中。白底黑字。 */
static void clock_render_framebuffer_portrait(uint8_t hour, uint8_t minute)
{
    char str[6];
    uint16_t x = CLOCK_X0;
    uint16_t y = CLOCK_Y0;

    /* 清白底(bit=1=白) */
    EPD_ClearBuffer();

    snprintf(str, sizeof(str), "%02u%02u", hour, minute);
    /* str 现为 "HHMM"，在第 2 个数字后插冒号显示 */

    /* H1 */
    EPD_DrawDigitLarge(x, y, str[0]); x += CLOCK_DIGIT_W + CLOCK_SPACING;
    /* H2 */
    EPD_DrawDigitLarge(x, y, str[1]); x += CLOCK_DIGIT_W + CLOCK_SPACING;
    /* : */
    EPD_DrawDigitLarge(x, y, ':');    x += CLOCK_COLON_W + CLOCK_SPACING;
    /* M1 */
    EPD_DrawDigitLarge(x, y, str[2]); x += CLOCK_DIGIT_W + CLOCK_SPACING;
    /* M2 */
    EPD_DrawDigitLarge(x, y, str[3]);
}

/* 横屏: 数字按 48x96 摆在逻辑 415x240 画布上, 写回竖屏 framebuffer 前旋转 90°。
 * 逻辑坐标 (lx, ly) -> 竖屏像素 EPD_SetPixel(col, row)。
 * 修正: 转置映射会带镜像导致上下颠倒, 故竖直轴(ly->col)取反一次。
 *   col = (240-1) - ly, row = lx */
static void clock_draw_digit_rot(uint8_t ch, uint16_t x0, uint16_t y0)
{
    const uint8_t *glyph = EPD_GetLargeGlyph(ch);
    if (!glyph) return;

    uint16_t gw = (ch == ':') ? EPD_COLON_W : EPD_DIGIT_W;
    uint16_t gh = EPD_DIGIT_H;
    uint16_t bpr = gw / 8;

    for (uint16_t gy = 0; gy < gh; gy++) {
        for (uint16_t gx = 0; gx < gw; gx++) {
            uint16_t byte_idx = (uint16_t)(gy * bpr) + (uint16_t)(gx / 8);
            uint8_t  bit_idx  = (uint8_t)(7 - (gx % 8));
            if (glyph[byte_idx] & (1 << bit_idx)) {
                EPD_SetPixel((uint16_t)(239U - (y0 + gy)), (uint16_t)(x0 + gx), 0);
            }
        }
    }
}

static void clock_render_framebuffer_landscape(uint8_t hour, uint8_t minute)
{
    char str[6];
    /* 逻辑横屏画布 415 宽(CLOCK_TOTAL_W=232) x 240 高, 居中摆放 */
    uint16_t x = (415 - CLOCK_TOTAL_W) / 2;
    uint16_t y = (240 - CLOCK_DIGIT_H) / 2;

    EPD_ClearBuffer();
    snprintf(str, sizeof(str), "%02u%02u", hour, minute);

    /* H1 */
    clock_draw_digit_rot(str[0], x, y); x += CLOCK_DIGIT_W + CLOCK_SPACING;
    /* H2 */
    clock_draw_digit_rot(str[1], x, y); x += CLOCK_DIGIT_W + CLOCK_SPACING;
    /* : */
    clock_draw_digit_rot(':', x, y);    x += CLOCK_COLON_W + CLOCK_SPACING;
    /* M1 */
    clock_draw_digit_rot(str[2], x, y); x += CLOCK_DIGIT_W + CLOCK_SPACING;
    /* M2 */
    clock_draw_digit_rot(str[3], x, y);
}

static void clock_render_framebuffer(uint8_t hour, uint8_t minute)
{
    int ls = 0;
    clock_get_active_config(NULL, &ls);
    if (ls) {
        clock_render_framebuffer_landscape(hour, minute);
    } else {
        clock_render_framebuffer_portrait(hour, minute);
    }
}

/*============================================================
 * 同步刷新 + 错误恢复 (GPT 方案 §五)
 *============================================================*/

/* force_full_refresh 标志也借 RTC weekday 之外无法存(SRAM 会丢)。
 * 简化：不跨复位保留，每轮按 minute==0 决定 GC。这里仅保留接口供未来扩展。 */
static int clock_epd_refresh_sync(bool full)
{
    int ret;

    if (EPD_WaitIdle(5000) != 0) {
        printf("[CLOCK] BUSY not idle, hardware reset\r\n");
        EPD_HardwareReset();
    }

    if (full) {
        ret = EPD_3IN52_Init_GC_Sync();
    } else {
        ret = EPD_3IN52_Init_DU_Sync();
    }
    if (ret != 0) {
        goto retry_full;
    }

    if (full) {
        /* 整点 GC(底层已统一为 DU 刷新, 正常极性) */
        ret = EPD_3IN52_Display_GC_Sync(framebuffer);
    } else {
        /* DU LUT 正常极性, 直接推送 */
        ret = EPD_3IN52_Display_DU_Sync(framebuffer);
    }
    if (ret != 0) {
        goto retry_full;
    }

    return EPD_Sleep_Sync();

retry_full:
    /* 最多一次硬复位 + GC 全刷重试。不能无限等 BUSY，否则无法再进低功耗。 */
    printf("[CLOCK] refresh failed (%d), retry once with GC\r\n", ret);
    EPD_HardwareReset();
    if (EPD_3IN52_Init_GC_Sync() != 0) {
        return -1;
    }
    if (EPD_3IN52_Display_GC_Sync(framebuffer) != 0) {
        return -1;
    }
    return EPD_Sleep_Sync();
}

/*============================================================
 * 充电感知: 充电时保持唤醒, 不反复休眠-唤醒
 *============================================================*/

/* PA21 低=充电中 */
static int clock_charging(void)
{
    return (HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_21) == 0) ? 1 : 0;
}

/* 轮询前 PA6/PA21 必须配置为输入+上拉(休眠唤醒后 GPIO 配置会丢) */
static void clock_charge_gpio_init(void)
{
    GPIO_InitParam gparam;
    memset(&gparam, 0, sizeof(gparam));
    gparam.mode = GPIOx_Pn_F0_INPUT;
    gparam.driving = GPIO_DRIVING_LEVEL_1;
    gparam.pull = GPIO_PULL_UP;
    HAL_GPIO_Init(GPIO_PORT_A, CLOCK_BUTTON_PIN, &gparam);
    HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_21, &gparam);
}

/* 渲染一帧当前时间(首帧/整点用 GC 全刷清残影, 其余 DU 快刷) */
static void clock_render_cycle(bool full)
{
    uint8_t h, m, s;
    clock_rtc_read(&h, &m, &s);
    clock_render_framebuffer(h, m);
    if (clock_epd_refresh_sync(full) != 0) {
        printf("[CLOCK] refresh failed\n");
    }
}

/* 拔出充电器 → 转入休眠(仍处于时钟模式, 下次 WKTIMER/PA21/PA6 唤醒) */
static void clock_charge_sleep(void)
{
    printf("[CLOCK] unplugged -> sleep until next wake\n");
    clock_charge_gpio_init();
    clock_schedule_next_wake();
    pm_diag_dump_noirq_list();
    pm_enter_mode(PM_MODE_HIBERNATION);
    while (1) {}
}

/* PA6 退出 → 清标志后重启进正常模式 */
static void clock_charge_exit(void)
{
    printf("[CLOCK] PA6 exit clock -> reboot normal\n");
    clock_mode_set_enabled(false);
    while (HAL_GPIO_ReadPin(GPIO_PORT_A, CLOCK_BUTTON_PIN) == 0) OS_MSleep(20);
    HAL_NVIC_CPUReset();
}

/* 充电保持唤醒主循环: 每次到间隔边界渲染刷新, 期间轮询 PA21(拔出→休眠)/PA6(退出) */
static void clock_awake_loop(void)
{
    printf("[CLOCK] charging: stay awake (no hibernate cycling)\n");
    clock_charge_gpio_init();
    clock_render_cycle(true);   /* 首帧 GC 全刷清残影 */

    for (;;) {
        int iv = 1;
        clock_get_active_config(&iv, NULL);
        uint32_t interval = (uint32_t)iv * 60U;
        if (interval < 60U) interval = 60U;

        /* 等待下一个边界, 期间轮询 PA21(拔出→休眠) 与 PA6(退出→重启)。
         * PA21 在充满/状态机翻转时会短暂抖动, 必须连续 3s 都读到"未充电"
         * 才视为真正拔出, 避免瞬时误判导致反反复复休眠。 */
        uint32_t unplug = 0;
        for (uint32_t waited = 0; waited < interval; waited++) {
            if (HAL_GPIO_ReadPin(GPIO_PORT_A, CLOCK_BUTTON_PIN) == 0) {
                clock_charge_exit();       /* 不返回 */
            }
            if (clock_charging()) {
                unplug = 0;
            } else if (++unplug >= 3) {
                printf("[CLOCK] unplugged (stable 3s) -> sleep\n");
                clock_charge_sleep();      /* 不返回 */
            }
            OS_MSleep(1000);
        }
        clock_render_cycle(false);         /* 边界 DU 快刷 */
    }
}

/*============================================================
 * 分钟周期 (GPT 方案 §四，最小化启动)
 *============================================================*/

/*
 * 分钟唤醒时只初始化：EPD GPIO、基础延时、日志串口(已在)。
 * **不启动** LVGL/WiFi/SD/触摸/文件管理/电池 UI。
 *
 * 完整执行次序(GPT 方案 §五)：
 *   Init GPIO -> 查 BUSY -> RESET -> 等 BUSY -> 面板参数/LUT
 *   -> 发 12450 字节 framebuffer -> 刷新触发 -> 等 BUSY -> 保护延时
 *   -> EPD deep-sleep -> 设唤醒源 -> HIBERNATION
 * 绝不在触发刷新后立即进 EPD/MCU 休眠。
 */
void clock_minute_cycle(void)
{
    uint8_t h, m, s;
    bool full;
    int ret;

    clock_charge_gpio_init();

    /* 若此刻充电(或休眠期间插入了充电器): 保持唤醒, 不反复休眠-唤醒 */
    if (clock_charging()) {
        clock_awake_loop();   /* 不返回 */
    }

    clock_rtc_read(&h, &m, &s);
    printf("[CLOCK] minute wake: %02u:%02u:%02u\r\n", h, m, s);

    /* 每小时整点做一次 GC 全刷清残影，其余分钟 DU */
    full = (m == 0);

    clock_render_framebuffer(h, m);

    ret = clock_epd_refresh_sync(full);
    if (ret != 0) {
        printf("[CLOCK] refresh failed, will retry GC next chance\r\n");
    }

    clock_schedule_next_wake();
    pm_diag_dump_noirq_list();   /* 诊断: 挂起链崩溃排查 */
    pm_enter_mode(PM_MODE_HIBERNATION);

    /* 不应返回 */
    while (1) {
    }
}

/*============================================================
 * 退出保护：等 PA6 释放 + boot guard
 *============================================================*/

/* 等 PA6 完全释放(高电平稳态 >=50ms)，防止唤醒后未释放又触发休眠。 */
static void clock_wait_key_release(void)
{
    uint32_t stable_ms = 0;
    while (stable_ms < 50) {
        if (HAL_GPIO_ReadPin(GPIO_PORT_A, CLOCK_BUTTON_PIN) == GPIO_PIN_HIGH) {
            stable_ms++;
        } else {
            stable_ms = 0;
        }
        OS_MSleep(1);
    }
}

bool clock_consume_boot_key_guard(void)
{
    if (s_boot_key_guard) {
        s_boot_key_guard = false;
        return true;
    }
    return false;
}

/*============================================================
 * 启动分流 (GPT 方案 §二)
 *============================================================*/

void clock_boot_dispatch(void)
{
    uint32_t event = HAL_Wakeup_GetEvent();
    printf("[CLOCK] boot dispatch, wake_event=0x%08x\r\n", event);

    if (event & PM_WAKEUP_SRC_WKTIMER) {
        /* 分钟唤醒：唯一启用 WKTIMER 的路径就是时钟模式 */
        printf("[CLOCK] WKTIMER wake -> minute cycle\r\n");
        EPD_GPIO_Init_Public();   /* 最小化：只初始化 EPD GPIO */
        clock_minute_cycle();     /* 不返回 */
        return;                   /* 不会到达 */
    }

    if ((event & PM_WAKEUP_SRC_WKIO7) && clock_mode_enabled()) {
        /* 休眠时钟中插入充电器: 切到"保持唤醒"充电时钟, 不退出时钟模式 */
        printf("[CLOCK] PA21(replug) wake -> stay awake clock\r\n");
        EPD_GPIO_Init_Public();
        clock_awake_loop();       /* 不返回 */
        return;                   /* 不会到达 */
    }

    if ((event & PM_WAKEUP_SRC_WKIO2) && clock_mode_enabled()) {
        /* PA6 按下退出时钟模式 */
        printf("[CLOCK] PA6 wake -> exit clock mode\r\n");
        clock_mode_set_enabled(false);
        clock_wait_key_release();
        s_boot_key_guard = true;
        /* 继续正常 InkFrog 启动 */
        return;
    }

    /* 其它(包括上电、异常复位)：正常启动。
     * 若 weekday 仍是 magic(异常复位后)，清掉它以免误判。 */
    if (clock_mode_enabled()) {
        printf("[CLOCK] stale magic on non-WKTIMER wake, clearing\r\n");
        clock_mode_set_enabled(false);
    }
}

/*============================================================
 * 关停外设 (复用 screensaver_enter 序列，GPT 方案 §九约束)
 *============================================================*/

static void clock_shutdown_peripherals(void)
{
    printf("[CLOCK] shutting down peripherals\r\n");

    /* fix-power-saving v2: WiFi 手动下电 + 屏蔽 SDK 重复 teardown (同 screensaver) */
    wifi_controller_stop();
    pm_unregister_wlan_power_onoff();   /* 屏蔽 SDK net_sys_onoff(0) 崩溃源 */
    wifi_controller_poweroff();         /* 手动 wlan_sta_disable + 等 Sys3 掉电 */

    CHSC6540_DeInit();                   /* 触摸 I2C 下电 */
    HAL_ADC_DeInit();                    /* ADC 下电 */

    /* SD 卡: 只卸载文件系统, 【不拆 SDC 控制器/不注销 SDC PM ops】。
     * 原因同 screensaver_enter: HAL_SDC_Deinit 注销 SDC 的 PM 注册会让
     * ROM 的 dpm_suspend_noirq 挂起链踩到损坏项 → hard fault (充电拔插头、
     * PA6 休眠、时钟模式三份崩溃日志的共同点)。保持 SDC 注册由 __mci_suspend
     * 下电, 文件系统唤醒(冷启动)后重新挂载。 */
    f_mount(NULL, "0:", 0);

    /* 注意: EXT LDO 关闭不在这里做! 必须在 HAL_Wakeup_SetIO 之后,
     * 否则写 SYS_LDO_SW_CTRL 会破坏 PA6 唤醒域。由 clock_mode_enter_run
     * 在 clock_schedule_next_minute() 之后统一关闭。 */

    /* GPT 方案 §九: 仍**不要**碰 PA7/PA23/PA3 等 GPIO,
     * 曾导致唤醒后 Flash 访问失败(rom_flash_rw fail)。 */
}

/*============================================================
 * 进入休眠时钟模式 (设置 UI 入口)
 *============================================================*/

void clock_mode_enter(void)
{
    /* 仅设标志，真正的进入流程在 disp_task 上下文执行(见 clock_mode_enter_run)。
     * 不能在 lvgl 回调上下文直接 pm_enter_mode(HIBERNATION)，会触发 PM 框架
     * 内部 UsageFault(除零, UFSR:0x100)。 */
    if (http_server_is_running()) {
        printf("[CLOCK] HTTP running, refusing to enter clock mode\r\n");
        return;
    }
    printf("[CLOCK] enter requested (deferred to disp_task)\r\n");
    s_clock_enter_pending = true;
}

void clock_mode_enter_run(void)
{
    uint8_t h, m, s;

    if (!s_clock_enter_pending) {
        return;
    }
    s_clock_enter_pending = false;

    printf("[CLOCK] entering clock mode (in disp_task context)\r\n");

    /* 1. NTP -> RTC(写 magic weekday) */
    clock_rtc_set_from_ntp();
    /* 1b. 把精度/横屏配置写进 RTC 日期字段, 使后续分钟唤醒(无 SD)也能读回 */
    clock_rtc_store_config();

    /* 2. 取 EPD 所有权(现在在 disp_task 上下文，安全挂起 lvgl)，
     *    渲染首帧，GC 全刷(进时钟第一帧要干净清残影)，EPD 深度睡眠。
     *    后续分钟刷新仍用快刷(DU)。 */
    epd_take_ownership();

    /* 3. 关停外设(WiFi/SD)。时钟模式只需 EPD/RTC/GPIO。 */
    clock_shutdown_peripherals();

    /* 4. 充电中进入 → 保持唤醒, 不反复休眠-唤醒(由循环内处理拔出/退出) */
    clock_charge_gpio_init();
    if (clock_charging()) {
        clock_awake_loop();   /* 不返回 */
    }

    /* 未充电: 渲染首帧(GC 全刷)再进 HIBERNATION */
    clock_rtc_read(&h, &m, &s);
    clock_render_framebuffer(h, m);
    if (clock_epd_refresh_sync(true) != 0) {
        printf("[CLOCK] first frame refresh failed\r\n");
    }

    /* 配置下次唤醒(WKTIMER 间隔 + PA6 退出 + PA21 插入充电) */
    clock_schedule_next_wake();

    /* fix-power-saving: 【不手动关 EXT LDO!】(同 screensaver)
     * EXT LDO 给 VDDIO 供电, PA6/WKTIMER 唤醒依赖它。
     * 让硬件 PMU 在 hibernation 时自动管理电源域。 */

    /* 5. 进 HIBERNATION(不返回)。在 disp_task 上下文调用，与 screensaver 一致。 */
    printf("[CLOCK] entering HIBERNATION\r\n");
    pm_enter_mode(PM_MODE_HIBERNATION);

    while (1) {
    }
}
