#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "screensaver.h"
#include "lvgl/lvgl.h"
#include "fs/fatfs/ff.h"
#include "common/framework/fs_ctrl.h"
#include "kernel/os/os.h"
#include "FreeRTOS.h"
#include "task.h"

#include "epd.h"
#include "lv_port_disp.h"
#include "http_server.h"

extern OS_Thread_t lvgl_thread;
extern void main_ui_create(void);

typedef struct {
    uint8_t initialized;
    uint8_t active;
    uint8_t swallow_until_release;
    uint32_t last_enter_tick;
} ScreensaverState;

static ScreensaverState g_ss = {0};

static int screensaver_load_to_framebuffer(void)
{
    FIL fp;
    FRESULT fr;
    UINT br = 0;
    uint32_t i;

    fr = f_open(&fp, SCREENSAVER_FILE_PATH, FA_READ);
    if (fr != FR_OK) {
        printf("[SS] open failed: %d\n", fr);
        return -1;
    }

    fr = f_read(&fp, framebuffer, EPD_BUFFER_SIZE, &br);
    f_close(&fp);
    if (fr != FR_OK || br != EPD_BUFFER_SIZE) {
        printf("[SS] read failed: fr=%d br=%u\n", fr, (unsigned int)br);
        return -1;
    }

    /*
     * 当前网页编辑器导出的 1bpp 极性与设备侧直接显示到 EPD 的极性相反。
     * 为了兼容已经保存在 SD 卡上的 screensaver.bin，这里在加载时统一翻转一次。
     * 这样用户不需要删除旧文件重传，现有屏保也能直接正常显示。
     */
    for (i = 0; i < EPD_BUFFER_SIZE; i++) {
        framebuffer[i] ^= 0xFF;
    }

    return 0;
}

static int screensaver_enter(void)
{
    lv_disp_t *disp;

    if (g_ss.active) {
        return 0;
    }
    if (http_server_is_running()) {
        return -1;
    }
    if (screensaver_load_to_framebuffer() != 0) {
        return -1;
    }

    printf("[SS] entering screensaver\n");
    disp = lv_disp_get_default();
    epd_pause_refresh();
    if (disp) {
        lv_disp_enable_invalidation(disp, false);
    }

    vTaskSuspend(lvgl_thread.handle);
    EPD_3IN52_Init();
    EPD_3IN52_Display();
    EPD_Sleep();
    vTaskResume(lvgl_thread.handle);

    g_ss.active = 1;
    g_ss.swallow_until_release = 0;
    g_ss.last_enter_tick = epd_get_tick();
    return 0;
}

static void screensaver_exit(void)
{
    lv_disp_t *disp = lv_disp_get_default();

    printf("[SS] exiting screensaver\n");
    vTaskSuspend(lvgl_thread.handle);
    EPD_3IN52_Init();
    EPD_3IN52_Init_DU();
    main_ui_create();
    vTaskResume(lvgl_thread.handle);

    if (disp) {
        lv_disp_enable_invalidation(disp, true);
        lv_disp_trig_activity(disp);
    }

    epd_resume_refresh();
    epd_mark_refresh_pending();

    g_ss.active = 0;
    g_ss.swallow_until_release = 1;
}

void screensaver_init(void)
{
    if (g_ss.initialized) {
        return;
    }
    fs_ctrl_mount(FS_MNT_DEV_TYPE_SDCARD, 0);
    f_mkdir(SCREENSAVER_DIR);
    g_ss.initialized = 1;
}

void screensaver_task(void)
{
    lv_disp_t *disp;
    uint32_t inactive_ms;

    if (!g_ss.initialized || g_ss.active) {
        return;
    }
    if (http_server_is_running()) {
        return;
    }
    if (!screensaver_has_image()) {
        return;
    }

    disp = lv_disp_get_default();
    if (!disp) {
        return;
    }

    inactive_ms = lv_disp_get_inactive_time(disp);
    if (inactive_ms >= SCREENSAVER_IDLE_TIMEOUT_MS) {
        screensaver_enter();
    }
}

int screensaver_handle_touch(TouchState_t state)
{
    if (!g_ss.active && !g_ss.swallow_until_release) {
        return 0;
    }

    if (g_ss.active) {
        if (state == TOUCH_STATE_PRESSED) {
            screensaver_exit();
            return 1;
        }
        return 1;
    }

    if (g_ss.swallow_until_release) {
        if (state == TOUCH_STATE_RELEASED || state == TOUCH_STATE_IDLE) {
            g_ss.swallow_until_release = 0;
        }
        return 1;
    }

    return 0;
}

int screensaver_is_active(void)
{
    return g_ss.active;
}

int screensaver_has_image(void)
{
    FILINFO fno;
    return (f_stat(SCREENSAVER_FILE_PATH, &fno) == FR_OK && !(fno.fattrib & AM_DIR) && fno.fsize == EPD_BUFFER_SIZE);
}

int screensaver_save_raw_file(const uint8_t *data, uint32_t len)
{
    FIL fp;
    FRESULT fr;
    UINT bw = 0;

    if (!data || len != EPD_BUFFER_SIZE) {
        return -1;
    }

    fs_ctrl_mount(FS_MNT_DEV_TYPE_SDCARD, 0);
    f_mkdir(SCREENSAVER_DIR);
    fr = f_open(&fp, SCREENSAVER_FILE_PATH, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        printf("[SS] save open failed: %d\n", fr);
        return -1;
    }

    fr = f_write(&fp, data, len, &bw);
    f_close(&fp);
    if (fr != FR_OK || bw != len) {
        printf("[SS] save write failed: fr=%d bw=%u\n", fr, (unsigned int)bw);
        return -1;
    }

    printf("[SS] saved %u bytes to %s\n", (unsigned int)len, SCREENSAVER_FILE_PATH);
    return 0;
}

int screensaver_get_status_json(char *buf, int buf_size)
{
    FILINFO fno;
    FRESULT fr;
    int has_image = 0;
    unsigned long size = 0;

    if (!buf || buf_size <= 0) {
        return -1;
    }

    fr = f_stat(SCREENSAVER_FILE_PATH, &fno);
    if (fr == FR_OK && !(fno.fattrib & AM_DIR)) {
        has_image = 1;
        size = (unsigned long)fno.fsize;
    }

    return snprintf(buf, buf_size,
                    "{\"active\":%s,\"has_image\":%s,\"file\":\"%s\",\"size\":%lu,\"expected\":%u}",
                    g_ss.active ? "true" : "false",
                    has_image ? "true" : "false",
                    SCREENSAVER_FILE_PATH,
                    size,
                    (unsigned int)EPD_BUFFER_SIZE);
}