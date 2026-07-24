/**
 * @file lv_port_disp.h
 * LVGL显示端口适配头文件
 */

#ifndef LV_PORT_DISP_H
#define LV_PORT_DISP_H

/*====================
 * 函数声明
 *===================*/

/**
 * 初始化显示端口
 */
void lv_port_disp_init(void);

/**
 * 显示刷新任务 (供主循环调用)
 */
void lv_port_disp_task(void);

/**
 * 标记EPD需要刷新（供UI事件调用）
 */
void epd_mark_refresh_pending(void);

/**
 * 通知EPD触摸按下（由触摸驱动调用）
 * 会阻断刷新直到释放
 */
void epd_notify_touch_down(void);

/**
 * 通知EPD触摸释放（由触摸驱动调用）
 * 会触发待处理的刷新
 */
void epd_notify_touch_up(void);

/**
 * 暂停EPD刷新（用于UI重建期间，防止刷出半成品）
 */
void epd_pause_refresh(void);

/**
 * 恢复EPD刷新（UI重建完成后调用）
 */
void epd_resume_refresh(void);

/**
 * 标记内容已变化（滑动翻页等场景），跳过释放时的旧刷新
 * 在更新LVGL内容前调用，状态机自动在新内容渲染完成后触发刷新
 */
void epd_set_content_dirty(void);

/**
 * 执行EPD刷新 - 由disp_task定期调用，刷新期间交替释放CPU
 */
void epd_do_refresh(void);

/**
 * 阻塞等待首帧 EPD 刷新完成（启动 WiFi 前调用）
 * @return 0 成功, -1 超时
 */
int epd_wait_first_frame_done(uint32_t timeout_ms);

/**
 * 首帧是否已刷完（WiFi CONNECTING 时仍允许首屏）
 */
int epd_is_first_frame_done(void);

/**
 * 获取系统运行时间（毫秒）- 调试用
 */
uint32_t epd_get_tick(void);

/**
 * 获取 EPD/framebuffer 独占所有权 (GPT clock 方案 §六)。
 *
 * 用于 screensaver_enter() / clock_mode_enter() 这类需要在 LVGL 之外
 * 直接改 framebuffer 并做整帧刷新的场景：
 *   - 暂停自动刷新(epd_pause_refresh)
 *   - 等待正在进行的 epd_do_refresh 完成(refresh_in_progress 清0)
 *   - 挂起 lvgl 线程(防止 flush_cb 在刷新期间改 framebuffer)
 *   - 清掉 pending 刷新标记
 * 调用者随后可安全地直接操作 framebuffer + EPD_3IN52_Display_*()。
 *
 * 注意：本函数会挂起 lvgl 线程，因此**只能在非 lvgl 线程**调用
 * (如 disp_task)。绝不能从 lvgl 按钮回调(lvgl_task 上下文)调用，
 * 否则会挂起自己导致死锁。从 lvgl 上下文进入、且即将进 HIBERNATION
 * 不返回的路径用 epd_wait_refresh_drain()。
 *
 * 进入 HIBERNATION 前的路径只需 take，不需要 release(系统即将复位)；
 * 非休眠的状态切换(如 screensaver_exit)用 release 恢复。
 */
void epd_take_ownership(void);

/**
 * 排空 EPD 刷新队列(不挂起 lvgl 线程)。
 *
 * 供"从 lvgl 上下文(按钮回调)进入、即将进 HIBERNATION 不返回"的路径用，
 * 例如 clock_mode_enter()：
 *   - 暂停自动刷新
 *   - 等待正在进行的 epd_do_refresh 完成
 *   - 清掉 pending 刷新标记
 *
 * 与 epd_take_ownership() 的区别：**不挂起 lvgl 线程**(因为调用者本身
 * 就在 lvgl 线程里)。由于调用者随后会关停外设并进 HIBERNATION,系统复位,
 * lvgl 线程不会再并发跑 flush_cb,因此无需挂起。
 */
void epd_wait_refresh_drain(void);

/**
 * 释放 EPD/framebuffer 所有权 (GPT clock 方案 §六)。
 * 恢复 lvgl 线程并重新允许自动刷新。仅用于不复位的状态切换路径。
 */
void epd_release_ownership(void);

#endif /* LV_PORT_DISP_H */
