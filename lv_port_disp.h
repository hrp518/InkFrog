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
 * 阻塞等待当前 pending 的一次 EPD 刷新真正刷到墨水屏（不暂停、不取锁）。
 * 供「先显示一个画面再进入长操作」的路径用，防止随后的 epd_pause_refresh()
 * 吞掉刚排队的刷新导致画面不出现。
 */
void epd_wait_refresh_idle(uint32_t timeout_ms);

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
 * EPD/framebuffer 互斥锁 (安全点机制, 替代 vTaskSuspend 硬挂起)。
 *
 * 背景: epd_do_refresh 原来用 vTaskSuspend(lvgl_thread) 在任意指令处
 * 硬挂 LVGL 线程做 EPD 硬件刷新, 若挂起点落在 lv_timer_handler/回调
 * 中途, vTaskResume 后线程可能回不到主循环 -> WiFi 连接成功瞬间整机
 * "卡死"(渲染/触摸/定时器全停)。
 *
 * 新机制: lvgl_task 每轮循环体持锁(覆盖 lv_timer_handler + phase 回调
 * 里所有写 framebuffer 的路径), disp_task 做 EPD 刷新时在安全点(两轮
 * 循环之间)拿锁。锁语义保证: 拿锁时 LVGL 一定不在渲染/回调中途。
 *
 * @param wait_ms 等待超时(毫秒), 0=trylock, OS_WAIT_FOREVER=永久等待
 * @return 0 拿到锁, -1 超时
 */
int epd_lock(uint32_t wait_ms);

/**
 * 释放 EPD/framebuffer 互斥锁 (与 epd_lock 配对)。
 */
void epd_unlock(void);

/**
 * 请求延迟重绘 (供 loading 遮罩使用)。
 *
 * loading_show/hide 调用 lv_refr_now 同步渲染时, 若恰好有 EPD 刷新在进行
 * (epd_refresh_in_progress/requested=1, _lv_disp_refr_timer 被抑制),
 * 渲染不会发生; 而 epd_do_refresh 完成后会清 inv_p —— 导致遮罩删除后的
 * 画面永远不再上屏 (屏幕停在 "Preparing fonts...")。
 * 调用本函数登记请求后, lvgl_task 会在当前 EPD 刷新结束后补一次完整重绘。
 */
void epd_request_rerender(void);

/**
 * 消费延迟重绘请求 (lvgl_task 每轮调用)。
 * @return 1=有待补的重绘(调用方应在 EPD 空闲时执行), 0=无
 */
int epd_consume_rerender_request(void);

/**
 * 休眠前诊断: dump ROM DPM noirq 设备链表 (定位休眠挂起链崩溃)。
 * 在 pm_enter_mode(HIBERNATION) 之前调用, 用于判断 noirq 链表是否在
 * 进入挂起链之前就已被写坏。
 */
void pm_diag_dump_noirq_list(void);

/* lvgl_task 心跳计数: 每轮主循环 +1, 供 disp_task 看门狗检测卡死 */
extern volatile uint32_t g_lvgl_heartbeat;
/* lvgl_task 运行阶段标记: 每进入一个循环体子步骤置位, 看门狗据此定位卡死阶段 */
extern volatile uint32_t g_lvgl_stage;
/* EPD 刷新进行中标志 (看门狗判据: 刷新期间 LVGL 本就不推进) */
extern volatile uint8_t epd_refresh_in_progress;
/* EPD 刷新已请求标志 (延迟重绘判断用, 见 epd_request_rerender) */
extern volatile uint8_t epd_refresh_requested;

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
