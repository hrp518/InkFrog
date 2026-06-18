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
int screensaver_handle_touch(TouchState_t state);
int screensaver_is_active(void);
int screensaver_has_image(void);
int screensaver_save_raw_file(const uint8_t *data, uint32_t len);
int screensaver_get_status_json(char *buf, int buf_size);

#ifdef __cplusplus
}
#endif

#endif