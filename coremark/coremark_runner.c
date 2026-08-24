/*
 * coremark_runner.c - CoreMark Runner for XR872
 *
 * CoreMark runs with highest priority and EPD refresh paused.
 * This ensures accurate benchmark results without LVGL interference.
 */

#include "coremark_runner.h"
#include "kernel/os/os.h"
#include "lvgl/lvgl.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "epd.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern int coremark_main(void);

extern void epd_mark_refresh_pending(void);
extern void epd_pause_refresh(void);
extern void epd_resume_refresh(void);
extern void main_ui_create(void);
extern float coremark_get_score(void);

static OS_Thread_t g_coremark_thread;
static volatile float g_final_score = 0.0f;

/* 结果页返回 (屏幕返回与物理返回共用) */
static void coremark_back_do(void) {
    printf("[CoreMark] Back triggered\n");
    main_ui_create();
    epd_mark_refresh_pending();
}

static void coremark_back_btn_cb(lv_event_t *e) {
    (void)e;
    printf("[CoreMark] Back button clicked\n");
    coremark_back_do();
}

static void show_result_screen(float score) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_make(255, 255, 255), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "CoreMark Done!");
    lv_obj_set_style_text_color(title, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *score_title = lv_label_create(scr);
    lv_label_set_text(score_title, "Score:");
    lv_obj_set_style_text_color(score_title, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_text_font(score_title, &lv_font_montserrat_12, 0);
    lv_obj_align(score_title, LV_ALIGN_CENTER, -60, 0);

    char score_text[32];
    if (score > 0) {
        snprintf(score_text, sizeof(score_text), "%.2f", score);
    } else {
        snprintf(score_text, sizeof(score_text), "Error");
    }

    lv_obj_t *score_label = lv_label_create(scr);
    lv_label_set_text(score_label, score_text);
    lv_obj_set_style_text_color(score_label, lv_color_make(0, 100, 0), 0);
    lv_obj_set_style_text_font(score_label, &lv_font_montserrat_12, 0);
    lv_obj_align(score_label, LV_ALIGN_CENTER, 30, 0);

    lv_obj_t *unit_label = lv_label_create(scr);
    lv_label_set_text(unit_label, "iter/sec");
    lv_obj_set_style_text_color(unit_label, lv_color_make(100, 100, 100), 0);
    lv_obj_set_style_text_font(unit_label, &lv_font_montserrat_12, 0);
    lv_obj_align(unit_label, LV_ALIGN_CENTER, 0, 30);

    lv_obj_t *platform = lv_label_create(scr);
    lv_label_set_text(platform, "XR872 @ 384MHz\nCortex-M4F");
    lv_obj_set_style_text_color(platform, lv_color_make(100, 100, 100), 0);
    lv_obj_set_style_text_font(platform, &lv_font_montserrat_12, 0);
    lv_obj_align(platform, LV_ALIGN_BOTTOM_MID, 0, -60);

    lv_obj_t *btn_back = lv_btn_create(scr);
    lv_obj_set_size(btn_back, 120, 40);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(btn_back, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_border_width(btn_back, 2, 0);
    lv_obj_set_style_border_color(btn_back, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_radius(btn_back, 4, 0);
    lv_obj_set_style_transition(btn_back, NULL, LV_PART_MAIN);
    lv_obj_t *btn_back_label = lv_label_create(btn_back);
    lv_label_set_text(btn_back_label, "Back");
    lv_obj_set_style_text_font(btn_back_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(btn_back_label, lv_color_make(255, 255, 255), 0);
    lv_obj_align(btn_back_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_back, coremark_back_btn_cb, LV_EVENT_CLICKED, NULL);

    /* 绑定物理返回键: 与结果屏 Back 按钮等效 */
    touch_register_back_btn_callback(coremark_back_do);

    printf("[CoreMark] Result screen created, score=%.2f\n", score);
}

static void coremark_thread_func(void *arg) {
    (void)arg;

    /* 先把 "CoreMark Running..." 真正刷到 EPD 再跑，否则画面停在上一屏(像卡死)。
     * coremark_runner_start 里只标记了 pending；disp_task 需等 lvgl 释放 EPD 锁
     * 才会整帧刷新。这里以睡眠让步等待刷新结束(最高约 12s)，期间 lvgl/disp 正常调度。 */
    for (int i = 0; i < 600; i++) {
        if (!epd_refresh_in_progress && !epd_refresh_requested) break;
        OS_MSleep(20);
    }

    printf("[CoreMark] Thread started, pausing EPD refresh...\n");
    epd_pause_refresh();

    printf("[CoreMark] Running benchmark (EPD paused, full CPU for test)...\n");
    coremark_main();

    float score = coremark_get_score();
    g_final_score = score;

    printf("\r\n======================================\r\n");
    printf("CoreMark Complete! Score: %.2f\r\n", score);
    printf("======================================\r\n\r\n");

    printf("[CoreMark] Resuming EPD refresh...\n");
    epd_resume_refresh();

    show_result_screen(score);
    epd_mark_refresh_pending();

    printf("[CoreMark] Thread exiting\n");
    OS_ThreadDelete(&g_coremark_thread);
}

int coremark_runner_start(void)
{
    printf("\r\n======================================\r\n");
    printf("CoreMark Benchmark Starting...\r\n");
    printf("XR872 @ 384MHz, ARM Cortex-M4F\r\n");
    printf("======================================\r\n\r\n");

    g_final_score = 0.0f;

    epd_pause_refresh();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_make(255, 255, 255), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "CoreMark Running...");
    lv_obj_set_style_text_color(title, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *progress = lv_label_create(scr);
    lv_label_set_text(progress, "Testing... Please wait");
    lv_obj_set_style_text_color(progress, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_text_font(progress, &lv_font_montserrat_12, 0);
    lv_obj_align(progress, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *platform = lv_label_create(scr);
    lv_label_set_text(platform, "XR872 @ 384MHz\nCortex-M4F");
    lv_obj_set_style_text_color(platform, lv_color_make(100, 100, 100), 0);
    lv_obj_set_style_text_font(platform, &lv_font_montserrat_12, 0);
    lv_obj_align(platform, LV_ALIGN_BOTTOM_MID, 0, -50);

    epd_resume_refresh();
    epd_mark_refresh_pending();

    printf("[CoreMark] Starting benchmark thread (HIGH priority), waiting EPD flush...\n");

    if (!OS_ThreadIsValid(&g_coremark_thread)) {
        OS_Status ret = OS_ThreadCreate(&g_coremark_thread,
                       "CoreMark",
                       coremark_thread_func,
                       NULL,
                       OS_PRIORITY_ABOVE_NORMAL,
                       4096);
        if (ret != OS_OK) {
            printf("[CoreMark] ERROR: Failed to create thread! ret=%d\n", ret);
            epd_resume_refresh();
            return -1;
        }
        printf("[CoreMark] Thread created successfully\n");
    } else {
        printf("[CoreMark] Warning: Thread already running\n");
    }

    return 0;
}
