#ifndef __SCREENSAVER_H__
#define __SCREENSAVER_H__

#include <stdint.h>
#include "lv_port_indev.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCREENSAVER_DIR              "/ScreenSaver"
#define SCREENSAVER_FILE_PATH        "/ScreenSaver/screensaver.bin"
#define SCREENSAVER_IDLE_TIMEOUT_MS  (120 * 1000)

void screensaver_init(void);
void screensaver_task(void);
void screensaver_task_force_enter(void);

/* 按下 PA6 的即时反馈: 显示 "Going to sleep..." 遮罩并立即推到 EPD。
 * 与 screensaver_enter() 的遮罩显示分支一致; 充电模式在替换画面之前
 * 先调用它, 让用户看到按键已触发。 */
void screensaver_show_sleep_overlay(void);
int screensaver_handle_touch(TouchState_t state);
int screensaver_is_active(void);
int screensaver_has_image(void);
int screensaver_save_raw_file(const uint8_t *data, uint32_t len);
int screensaver_get_status_json(char *buf, int buf_size);

/* 渲染用户屏保到 EPD 并 deep sleep（充电模式拔出充电器等复用）。
 * 需要调用方先 fs_ctrl_mount 挂载 SD 才能读到自定义屏保图；
 * 无图时 fallback 显示 "Tuwa Reader" 文字。返回 1=有图 0=文字。 */
int screensaver_render_to_epd(void);

#ifdef __cplusplus
}
#endif

#endif