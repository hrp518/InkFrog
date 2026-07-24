/*
 * clock_mode - 休眠时钟模式 (基于 GPT clock.txt 方案)
 *
 * 设计目标：每分钟冷启动一次，只做一次整帧 DU 快刷(每小时一次 GC)，
 * EPD 深度睡眠，XR872 重新进入 HIBERNATION；按 PA6 退出回到正常 InkFrog。
 *
 * 关键约束(fontexp 现实)：
 *  - 无 RTC 备份寄存器 API。模式标志复用 RTC DDHHMMSS 的 weekday 子字段
 *    (bits[31:29], 3bit, 正常 0-6) 写入 magic=7 作为隐式标志。HIBERNATION
 *    期间 RTC 域不断电，寄存器保留。只在进入/退出时写，每分钟唤醒只读。
 *    双保险：分钟周期由 WKTIMER 唤醒驱动(正常 InkFrog/screensaver 从不启用
 *    WKTIMER)，wake_event==WKTIMER 即等价"在时钟模式"。
 *  - 时间源：进入时钟模式时(WiFi 在)NTP 校时并写入 RTC，之后每分钟唤醒只读 RTC。
 *    LOSC 为内部 32kHz RC，有漂移；每次退出回正常模式联网时 NTP 重新校正。
 *  - 大号数字：epd.c 内置 48x96 1bpp 数字字模表 + EPD_DrawDigitLarge。
 *
 * 详见 GPT clock.txt 方案 §一～§九。
 */
#ifndef _CLOCK_MODE_H_
#define _CLOCK_MODE_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 启动分流(GPT 方案 §二)。必须在 main() 中、普通服务(WiFi/SD/LVGL/触摸)
 * 创建之前调用，且在 GPIO/LDO 基础配置之后调用。
 *
 *  - WKTIMER 唤醒    -> 最小化时钟周期，不返回(进 HIBERNATION)
 *  - WKIO2(PA6) 唤醒 -> 若处于时钟模式则清除标志、等按键释放、设 boot guard
 *  - 其它            -> 正常 InkFrog 启动，返回
 *
 * 本函数内部会调用 EPD_GPIO_Init_Public() / EPD 相关最小初始化，
 * 但**不会**启动 LVGL/WiFi/SD/触摸。
 */
void clock_boot_dispatch(void);

/*
 * 分钟周期：读 RTC → 渲染时钟 → EPD 刷新 → 配置下次 WKTIMER → 进 HIBERNATION。
 * 不返回。供 main() 的 WKTIMER 早判路径直接调用 (省电优化：跳过 WiFi/SD 初始化)。
 * 前置：platform_init_level0() + EPD_GPIO_Init_Public() 已完成。
 */
void clock_minute_cycle(void);

/*
 * 请求进入休眠时钟模式(由设置 UI 的 lvgl 按钮回调调用)。只设置一个
 * pending 标志后立即返回。真正的进入逻辑由 clock_mode_enter_run() 在
 * disp_task 上下文执行。
 *
 * 为什么不能直接在 lvgl 回调里进 HIBERNATION：pm_enter_mode(HIBERNATION)
 * 必须在 disp_task 这类"普通任务"上下文调用(与 screensaver 一致)。在
 * lvgl_task 上下文直接进 HIBERNATION 会触发 PM 框架内部状态错误(实测
 * 会触发 UsageFault 除零, UFSR:0x100)。
 */
void clock_mode_enter(void);

/*
 * 在 disp_task 上下文执行真正的进入时钟模式流程。
 *  1. NTP 校时 -> 写 RTC(weekday=7 magic)
 *  2. 排空 EPD 刷新，渲染当前时间首帧，DU 刷新，EPD 深度睡眠
 *  3. 关停外设(复用 screensaver 序列，不碰 PA7/PA23/PA3)
 *  4. 配置下次分钟唤醒 + PA6 退出唤醒
 *  5. 进 HIBERNATION(不返回)
 *
 * disp_task 每轮调用；若没有 pending 请求则直接返回。
 */
void clock_mode_enter_run(void);

/*
 * 当前是否处于时钟模式(读 RTC weekday==7)。供 main/disp_task 启动判断用。
 */
bool clock_mode_enabled(void);

/*
 * 清除时钟模式标志(weekday 写回真实值 0-6)。退出路径调用。
 */
void clock_mode_set_enabled(bool en);

/*
 * disp_task 的 PA6 按键检测调用：若 boot_key_guard 置位则消费它并返回 true
 * (表示本次按下应被忽略，避免唤醒后立刻又进休眠)。否则返回 false。
 */
bool clock_consume_boot_key_guard(void);

#ifdef __cplusplus
}
#endif

#endif /* _CLOCK_MODE_H_ */
