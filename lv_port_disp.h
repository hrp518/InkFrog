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
 * 获取系统运行时间（毫秒）- 调试用
 */
uint32_t epd_get_tick(void);

#endif /* LV_PORT_DISP_H */
