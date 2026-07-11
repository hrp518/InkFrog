/**
 * @file lv_port_disp.c
 * LVGL显示端口适配 - EPD_3IN52墨水屏 (240x415单色)
 *
 * EPD刷新策略：
 * 1. disp_task中检测到pending后，触发EPD刷新
 * 2. EPD刷新使用OS_ThreadSleep(0)交替释放CPU，不完全阻塞lvgl_task
 * 3. touch_down清除pending防止触摸时刷新（仅WAIT_RELEASE状态）
 * 4. touch_up在WAIT_RELEASE状态清除pending（仅在normal flush场景）
 * 5. swipe时epd_set_content_dirty设dirty=1，flush_cb检测到后设pending=1
 */

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "lvgl/lvgl.h"
#include "epd.h"
#include "lv_port_indev.h"
#include "driver/chip/hal_gpio.h"
#include "kernel/os/os.h"
#include "task.h"

/* LVGL线程句柄 - 用于刷新时挂起LVGL任务防止SPI冲突 */
extern OS_Thread_t lvgl_thread;

/*====================
 * EPD_3IN52 配置
 *===================*/
#define EPD_HORZ  240
#define EPD_VERT  415

/* EPD刷新状态 */
typedef enum {
    EPD_STATE_IDLE,            // 空闲，等待刷新
    EPD_STATE_WAIT_RELEASE,    // 触摸按下，等待释放后刷新
    EPD_STATE_CONTENT_UPDATE,  // 内容已变化，等待新渲染完成后刷新
    EPD_STATE_REFRESHING       // 正在刷新
} EPD_State;

/*====================
 * EPD刷新同步管理器
 *===================*/
typedef struct {
    volatile uint32_t last_refresh_time;
    uint32_t min_interval_ms;
    volatile uint8_t  refresh_pending;    // 待刷新标志
    volatile uint8_t  refresh_busy;      // 刷新中标志
    volatile uint8_t  refresh_paused;    // 刷新暂停标志（UI重建期间）
    volatile uint8_t  content_dirty;      // 内容已变化，需要重新渲染后再刷新
    EPD_State state;
} EPD_SyncManager;

static EPD_SyncManager epd_sync = {
    .last_refresh_time = 0,
    .min_interval_ms = 50,
    .refresh_pending = 0,
    .refresh_busy = 0,
    .refresh_paused = 0,
    .content_dirty = 0,
    .state = EPD_STATE_IDLE
};

// 获取系统Tick (毫秒)
uint32_t epd_get_tick(void) {
    return (uint32_t)OS_GetTicks() * 1000 / configTICK_RATE_HZ;
}

// 检查是否可以刷新
static uint8_t epd_sync_can_refresh(void) {
    uint32_t now = epd_get_tick();
    if (epd_sync.refresh_paused) return 0;
    return (now - epd_sync.last_refresh_time >= epd_sync.min_interval_ms) ? 1 : 0;
}

// EPD帧缓冲
extern uint8_t framebuffer[EPD_BUFFER_SIZE];

// LVGL绘制缓冲 - 使用PSRAM
#ifdef __CONFIG_PSRAM
static lv_color_t lv_draw_buf[EPD_HORZ * EPD_VERT] __attribute__((section(".psram_bss")));
#else
static lv_color_t lv_draw_buf[EPD_HORZ * EPD_VERT];
#endif

// 安全测试PSRAM是否可访问
static int test_psram_access(void *addr, uint32_t size) {
    volatile uint32_t test_word;
    volatile uint8_t test_byte;

    // 读取测试
    test_word = *(volatile uint32_t *)addr;
    test_byte = *(volatile uint8_t *)addr;

    // 写入测试（保持原值）
    *(volatile uint32_t *)addr = test_word;
    *(volatile uint8_t *)addr = test_byte;

    return 0;  // OK
}
static lv_disp_draw_buf_t disp_buf;
static lv_disp_drv_t disp_drv;
volatile uint8_t epd_refresh_requested = 0;   // 请求刷新EPD (exported for lv_refr.c)
volatile uint8_t epd_refresh_in_progress = 0; // 刷新进行中 (exported for lv_refr.c)
volatile uint8_t epd_micro_render_drained = 0; // 微渲染已排干标志 (exported for lv_refr.c)

/* 每轮渲染的flush统计 */
#define MAX_FLUSHES_PER_RENDER 20
typedef struct {
    int x1, y1, x2, y2;
    int w, h;
    int pixels;
    uint32_t enter_tick;   // 进入flush_cb的时间
    uint32_t exit_tick;    // 退出flush_cb的时间
} FlushInfo;

static FlushInfo flushes[MAX_FLUSHES_PER_RENDER];
static int flush_count_this_render = 0;
static int flush_num = 0;  // 全局render序号
static uint32_t total_pix_this_render = 0; // 本轮总像素数（用于微渲染检测）
static uint32_t black_pix_this_render = 0;  // 本轮黑像素数量

/*====================
 * 像素格式转换
 *===================*/

static void set_pixel(uint8_t *fb, int x, int y, lv_color_t color) {
    if (x < 0 || x >= EPD_HORZ || y >= EPD_VERT) return;
    
    int offset = (y * EPD_HORZ + x) / 8;
    uint8_t bit = x % 8;
    
    if (color.full) {
        fb[offset] |= (0x80 >> bit);
    } else {
        fb[offset] &= ~(0x80 >> bit);
    }
}

/*====================
 * FLUSH回调 - 详细日志版本
 *===================*/

static void epd_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t t_enter = epd_get_tick();
    int32_t x, y;
    int area_w = area->x2 - area->x1 + 1;
    int area_h = area->y2 - area->y1 + 1;
    int is_last = lv_disp_flush_is_last(drv);
    
    // 第一个flush开启本轮渲染统计
    if (flush_count_this_render == 0) {
        flush_num++;
        total_pix_this_render = 0;
        black_pix_this_render = 0;  // 重置黑像素计数
        epd_micro_render_drained = 0;  // 重置微渲染标志
    }
    
    // 记录本条flush（不打印）
    if (flush_count_this_render < MAX_FLUSHES_PER_RENDER) {
        FlushInfo *fi = &flushes[flush_count_this_render];
        fi->x1 = area->x1; fi->y1 = area->y1;
        fi->x2 = area->x2; fi->y2 = area->y2;
        fi->w = area_w; fi->h = area_h;
        fi->pixels = area_w * area_h;
        fi->enter_tick = t_enter;
        fi->exit_tick = 0;
    }
    flush_count_this_render++;
    total_pix_this_render += (uint32_t)area_w * (uint32_t)area_h;
    
    // 像素写入帧缓冲
    for (y = area->y1; y <= area->y2; y++) {
        for (x = area->x1; x <= area->x2; x++) {
            // 统计黑像素
            if (color_p->full == 0) {
                black_pix_this_render++;
            }
            set_pixel(framebuffer, x, y, *color_p);
            color_p++;
        }
    }
    
    uint32_t t_exit = epd_get_tick();  // 退出时间
    
    // 登记退出时间（找对应的那条flush记录）
    if (flush_count_this_render > 0 && flush_count_this_render <= MAX_FLUSHES_PER_RENDER) {
        flushes[flush_count_this_render - 1].exit_tick = t_exit;
    }
    
    if (is_last) {
        // 整轮渲染结束，汇总打印所有flush的耗时
        uint32_t start_tick = flushes[0].enter_tick;
        uint32_t total_render_ms = t_exit - start_tick;
        printf("\n[EPD_RENDER #%d] === LVGL render DONE (T=%ums, duration=%ums, %d flushes, total_pix=%lu, BLACK_pix=%lu) ===\n",
               flush_num, t_exit, total_render_ms, flush_count_this_render, 
               (unsigned long)total_pix_this_render, (unsigned long)black_pix_this_render);
        for (int i = 0; i < flush_count_this_render; i++) {
            FlushInfo *fi = &flushes[i];
            uint32_t enter_gap = (i == 0) ? 0 : fi->enter_tick - flushes[i-1].enter_tick;
            uint32_t flush_dur = fi->exit_tick - fi->enter_tick;
            printf("[EPD_RENDER #%d]   flush[%02d]: (%d,%d)-(%d,%d) %dx%d %dpix  gap=%ums  dur=%ums\n",
                   flush_num, i+1, fi->x1, fi->y1, fi->x2, fi->y2,
                   fi->w, fi->h, fi->pixels, enter_gap, flush_dur);
        }
        
        flush_count_this_render = 0;  // 重置计数
        
        /* =========================================================
         * INFINITE REFRESH LOOP FIX - Core detection point
         *
         * After render completes, check if this was a micro-render:
         * - Total pixels ≤ 256 (about 16x16 area)
         * - This indicates a UI artifact (cursor blink, etc.)
         *   rather than real content update
         *
         * If micro-render detected: don't set pending=1.
         * The framebuffer content is effectively unchanged from
         * EPD's perspective. Suppressing the refresh breaks
         * the infinite loop.
         *
         * Note: lv_refr.c also drains inv_p when px_num ≤ 256,
         * which prevents the micro-invalidations from accumulating
         * into a large batch. But draining inv_p doesn't prevent
         * the FIRST micro-render from setting pending=1.
         * The fix here in flush_cb catches the FIRST one too.
         * =========================================================
         */
        if (total_pix_this_render <= 256) {
            epd_micro_render_drained = 1;
            printf("[EPD_RENDER #%d] -> SUPPRESSED (micro-render: %lu pixels)\n",
                   flush_num, (unsigned long)total_pix_this_render);
        } else {
            // 核心修复：只要渲染完成，无条件将 pending 置 1
            // 稍后 disp_task 会在 IDLE 状态时自动去执行这次刷新
            epd_sync.refresh_pending = 1;
            
            if (epd_sync.state == EPD_STATE_CONTENT_UPDATE) {
                epd_sync.content_dirty = 0;
            }
            printf("[EPD_RENDER #%d] -> pending=1\n", flush_num);
        }
    }
    
    lv_disp_flush_ready(drv);
}

/*====================
 * 显示端口初始化
 *===================*/

void lv_port_disp_init(void) {
    printf("[LVGL] === lv_port_disp_init START ===\r\n");

    // ========== PSRAM访问测试 ==========
    printf("[LVGL] Testing PSRAM access...\r\n");
    extern uint32_t __psram_bss_start__;
    extern uint32_t __psram_bss_end__;
    uint32_t psram_bss_size = (uint32_t)&__psram_bss_end__ - (uint32_t)&__psram_bss_start__;
    printf("[LVGL] PSRAM .bss section: start=0x%08X, end=0x%08X, size=%u bytes\r\n",
           (unsigned int)&__psram_bss_start__, (unsigned int)&__psram_bss_end__, psram_bss_size);

    // 测试lv_draw_buf地址是否可访问
    printf("[LVGL] Testing lv_draw_buf at 0x%08X...\r\n", (unsigned int)lv_draw_buf);
    if (test_psram_access(lv_draw_buf, sizeof(lv_draw_buf)) == 0) {
        printf("[LVGL] PSRAM access OK, lv_draw_buf size=%u bytes\r\n", sizeof(lv_draw_buf));
    } else {
        printf("[LVGL] PSRAM access FAILED!\r\n");
    }
    // ====================================

    // ========== EPD初始化 ==========
    printf("[LVGL] Step 1: EPD_3IN52_Init() calling...\r\n");
    int init_retry = 0;
    while (EPD_3IN52_Init() != 0) {
        init_retry++;
        printf("[EPD] Init failed, retry #%d in 3s...\n", init_retry);
        OS_MSleep(3000);
    }
    if (init_retry > 0) {
        printf("[EPD] Init succeeded after %d retries\n", init_retry);
    }
    printf("[LVGL] Step 1: EPD_3IN52_Init() DONE\r\n");
    
    printf("[LVGL] Step 2: EPD_3IN52_Init_DU() calling...\r\n");
    EPD_3IN52_Init_DU();
    printf("[LVGL] Step 2: EPD_3IN52_Init_DU() DONE\r\n");

     printf("[LVGL] Step 3: Clearing framebuffer to white...\r\n");
     memset(framebuffer, 0xFF, EPD_BUFFER_SIZE);
     printf("[LVGL] Step 3: framebuffer cleared (%d bytes)\r\n", EPD_BUFFER_SIZE);

     /*
      * 删除旧的黑屏硬件测试帧。
      * 正式初始化时不应先刷一帧全黑，而应让第一次完整刷新直接显示首页。
      */
    
    printf("[LVGL] EPD ready\r\n");
    
    epd_sync.last_refresh_time = 0;
    epd_sync.refresh_pending = 0;
    epd_sync.refresh_busy = 0;
    epd_sync.refresh_paused = 1;   // 初始化期间暂停刷新
    epd_sync.content_dirty = 0;
    epd_sync.state = EPD_STATE_IDLE;
    
    lv_disp_draw_buf_init(&disp_buf, lv_draw_buf, NULL, sizeof(lv_draw_buf));
    
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &disp_buf;
    disp_drv.hor_res = EPD_HORZ;
    disp_drv.ver_res = EPD_VERT;
    disp_drv.flush_cb = epd_flush_cb;
    disp_drv.full_refresh = 1;
    
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
    
    printf("[LVGL] Display registered\r\n");
    
    lv_theme_t *th = lv_theme_mono_init(disp, false, LV_FONT_DEFAULT);
    lv_disp_set_theme(disp, th);
    printf("[LVGL] Theme enabled\r\n");
}

/*====================
 * 刷新任务 - 在disp_task中调用
 * 检测到pending后执行EPD刷新，刷新期间交替释放CPU
 *===================*/

void lv_port_disp_task(void) {
    TouchState_t touch = touch_get_state();
    
    // 处理EPD刷新进行中 - 刷新完成后回到IDLE
    if (epd_refresh_in_progress) {
        // EPD刷新在epd_do_refresh()中完成，这里只检查状态
        // epd_do_refresh会设置state和busy
        return;
    }
    
    switch (epd_sync.state) {
        
        case EPD_STATE_IDLE:
            if (epd_sync.refresh_pending && !epd_sync.refresh_busy) {
                if (touch == TOUCH_STATE_PRESSED) {
                    epd_sync.state = EPD_STATE_WAIT_RELEASE;
                } else if (epd_sync_can_refresh()) {
                    epd_sync.state = EPD_STATE_REFRESHING;
                    epd_sync.refresh_pending = 0;
                    epd_sync.refresh_busy = 1;
                    epd_refresh_requested = 1;
                    printf("[EPD] Refresh requested\n");
                }
            }
            break;
            
        case EPD_STATE_WAIT_RELEASE:
            if (touch != TOUCH_STATE_PRESSED) {
                if (epd_sync.refresh_pending && !epd_sync.refresh_busy && epd_sync_can_refresh()) {
                    if (epd_sync.content_dirty) {
                        epd_sync.refresh_pending = 0;
                        epd_sync.content_dirty = 0;
                        epd_sync.state = EPD_STATE_IDLE;
                    } else {
                        epd_sync.state = EPD_STATE_REFRESHING;
                        epd_sync.refresh_pending = 0;
                        epd_sync.refresh_busy = 1;
                        epd_refresh_requested = 1;
                        printf("[EPD] Refresh requested (after release)\n");
                    }
                } else {
                    epd_sync.state = EPD_STATE_IDLE;
                }
            }
            break;
            
        case EPD_STATE_CONTENT_UPDATE:
            if (epd_sync.refresh_pending && !epd_sync.refresh_busy) {
                epd_sync.content_dirty = 0;
                if (touch != TOUCH_STATE_PRESSED) {
                    epd_sync.state = EPD_STATE_REFRESHING;
                    epd_sync.refresh_pending = 0;
                    epd_sync.refresh_busy = 1;
                    epd_refresh_requested = 1;
                    printf("[EPD] Refresh requested (new content)\n");
                } else {
                    epd_sync.refresh_pending = 0;
                    epd_sync.state = EPD_STATE_IDLE;
                }
            }
            break;
            
        case EPD_STATE_REFRESHING:
            break;
    }
}

/*====================
 * 执行EPD刷新 - 由disp_task定期调用
 * 使用OS_ThreadSleep(0)交替释放CPU，不完全阻塞
 *===================*/

void epd_do_refresh(void) {
    if (!epd_refresh_requested) return;
    if (epd_refresh_in_progress) return;

    epd_refresh_requested = 0;
    epd_refresh_in_progress = 1;
    uint32_t t = epd_get_tick();

    lv_disp_t * disp_before = lv_disp_get_default();
    printf("[EPD] Starting display (T=%ums) inv_p_before=%d\n", t, disp_before ? disp_before->inv_p : -1);

    /* 挂起LVGL任务，执行EPD刷新（关键：只在挂起期间做硬件刷新） */
    vTaskSuspend(lvgl_thread.handle);
    printf("[EPD] lvgl suspended, calling EPD_3IN52_Display_DU\n");
    EPD_3IN52_Display_DU();
    printf("[EPD] EPD_3IN52_Display_DU returned, resuming lvgl\n");
    vTaskResume(lvgl_thread.handle);
    lv_timer_handler_unblock_after_suspend();
    printf("[EPD] lvgl resumed (call returned)\n");

    lv_disp_t * disp = lv_disp_get_default();
    int inv_p_during = disp ? disp->inv_p : -1;
    printf("[EPD] Display done (T=%ums, epd_cost=%ums) inv_p_during=%d\n", epd_get_tick(), epd_get_tick() - t, inv_p_during);

    /* 【关键修复】FM close卡死问题修复：
     * 1. 不再根据 inv_p 是否大于0来决定是否重新 invalidate
     * 2. 只做必要的状态清理：重置 inv_p，保持 invalidation 开启
     * 3. 不乱改 disp 状态，让 flush_cb 的正常流程处理下次刷新
     */
    if (disp) {
        disp->inv_p = 0;
        /* 保持 invalidation 开启，让正常的 LVGL 渲染流程处理 */
        lv_disp_enable_invalidation(disp, true);
    }

    epd_refresh_in_progress = 0;
    epd_sync.refresh_busy = 0;
    epd_sync.last_refresh_time = epd_get_tick();
    epd_sync.state = EPD_STATE_IDLE;

    printf("[EPD] LVGL resumed, invalidation ENABLED (inv_p cleared)\n");
}

/*====================
 * 公开API
 *===================*/

void epd_notify_touch_down(void) {
    lv_disp_t * disp = lv_disp_get_default();
    if (disp && !lv_disp_is_invalidation_enabled(disp)) {
        lv_disp_enable_invalidation(disp, true);
        printf("[EPD] Invalidation RE-ENABLED by touch\n");
    }
    if (epd_sync.state == EPD_STATE_WAIT_RELEASE) {
        epd_sync.refresh_pending = 0;
    }
}

void epd_notify_touch_up(void) {
    if (epd_sync.state == EPD_STATE_WAIT_RELEASE) {
        epd_sync.refresh_pending = 0;
    }
}

void epd_mark_refresh_pending(void) {
    if (epd_sync.state != EPD_STATE_REFRESHING && !epd_sync.refresh_busy && !epd_sync.refresh_paused) {
        lv_disp_t * disp = lv_disp_get_default();
        if (disp && !lv_disp_is_invalidation_enabled(disp)) {
            lv_disp_enable_invalidation(disp, true);
        }
        epd_sync.refresh_pending = 1;
    } else {
        /* Debug: 找出为何 mark 无效 - 关键诊断点 */
        static uint32_t s_mark_skip_count = 0;
        s_mark_skip_count++;
        if ((s_mark_skip_count % 50) == 1) {
            printf("[EPD] mark_refresh SKIPPED #%u (state=%d busy=%d paused=%d)\n",
                   s_mark_skip_count, (int)epd_sync.state,
                   (int)epd_sync.refresh_busy, (int)epd_sync.refresh_paused);
        }
    }
}

void epd_pause_refresh(void) {
    epd_sync.refresh_paused = 1;
    epd_sync.refresh_pending = 0;
}

void epd_resume_refresh(void) {
    epd_sync.refresh_paused = 0;
    // 不能强制置 pending=1！把刷新触发权完全交给 LVGL 的 flush_cb
}

/*====================
 * 滑动/内容更新协同
 *===================*/

void epd_set_content_dirty(void) {
    epd_sync.content_dirty = 1;
    epd_sync.refresh_pending = 0;
    epd_sync.state = EPD_STATE_CONTENT_UPDATE;
}
