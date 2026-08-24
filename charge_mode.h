/*
 * charge_mode - 充电模式
 *
 * 设计目标：休眠时插入充电器进入充电模式(显示"充电中 XX%"/"充满了"，
 * 每分钟刷新一次电量)；拔出充电器回普通休眠；按 PA6 进普通模式。
 *
 * 与 clock_mode 一样的架构：
 *  - 模式标志复用 RTC DDHHMMSS 的 weekday 子字段，magic=6。
 *    (clock_mode 用 weekday=7，互不冲突；正常值 0-6 中 6=周日，
 *     但 NTP 校时只在普通模式联网时做，充电模式不联网，不冲突。)
 *  - HIBERNATION 唤醒 = 冷启动，靠 HAL_Wakeup_GetEvent + RTC magic 分流。
 *  - 走最小化初始化路径(只 platform_init_level0 + EPD_GPIO)，不启动 WiFi/SD/LVGL。
 *
 * PA21(WKIO7) 边沿唤醒策略 (避免开机→休眠→开机循环)：
 *  - 普通休眠(未充电, PA21=高)：武装下降沿(高→低=插入充电器)
 *  - 充电模式(充电中, PA21=低)：武装上升沿(低→高=拔出充电器)
 *  两边边沿方向相反，不会循环。
 */
#ifndef CHARGE_MODE_H
#define CHARGE_MODE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RTC weekday magic: 6 表示充电模式 (clock_mode 用 7) */
#define CHARGE_MAGIC_WDAY  ((uint32_t)6)

/* 当前是否处于充电模式 (读 RTC weekday==6)。供 main 冷启动分流用。 */
bool charge_mode_enabled(void);

/* 设置/清除充电模式标志 (写 RTC weekday)。 */
void charge_mode_set_enabled(bool en);

/*
 * 冷启动充电入口(最小初始化版本, 从 main() 早判调用)。
 * 前置: platform_init_level0 + EPD_GPIO_Init_Public 已完成。
 * 渲染充电画面 → 进入轮询循环(不休眠, MCU 活跃)。
 * PA6 按下时 return, 调用方继续正常启动。
 */
void charge_mode_run_minimal(void);

/*
 * 主动进入充电模式 (由屏保/普通模式在 disp_task 上下文调用)。
 * 关外设 → 渲染充电画面 → 进入轮询循环(不休眠)。
 * PA6 按下时 return 到调用方。
 */
void charge_mode_enter(void);

#ifdef __cplusplus
}
#endif

#endif /* CHARGE_MODE_H */
