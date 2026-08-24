/*
 * LVGL E-Paper 3.52 inch Demo (EPD_3IN52)
 * 
 * GPIO Configuration:
 *   PA04: RST (复位输出)
 *   PA05: BUSY (中断输入)
 *   PA06: DC (数据/命令选择)
 *   PA07: CS (片选)
 *   PA08: CLK (SPI时钟)
 *   PA09: DIN (SPI数据)
 *   PA19: I2C1_SCL (CHSC6540)
 *   PA20: I2C1_SDA (CHSC6540)
 *   PA23: 3.3V enable (SY8088 DC-DC)
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "kernel/os/os.h"
#include "driver/chip/hal_gpio.h"
#include "driver/chip/hal_prcm.h"
#include "driver/chip/hal_adc.h"
#include "driver/chip/sdmmc/sdmmc.h"
#include "lvgl/lvgl.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "epd.h"
#include "chsc6540.h"
#include "fs/fatfs/ff.h"
#include "common/framework/fs_ctrl.h"
#include "file_manager.h"
#include "heap_debug.h"
#include "wlan_manager.h"
#include "http_server.h"
#include "wifi_controller.h"
#include "screensaver.h"
#include "driver/chip/hal_wakeup.h"
#include "pm/pm.h"
#include "coremark/coremark_runner.h"
#include "font_priority_loader.h"
#include "settings_screen.h"
#include "font_warm.h"
#include "time_sync.h"
#include "settings_storage.h"
#include "bookshelf.h"
#include "clock_mode.h"
#include "charge_mode.h"
#include "loading.h"
#include "boot_screen.h"
#include "sd_recovery.h"

extern const lv_font_t lv_font_montserrat_12;

/* SD卡测试函数声明 - 来自cmd_sd.c */
extern int32_t mmc_test_init(uint32_t host_id, void *sdc_param, uint32_t scan);
extern int32_t mmc_test_exit(uint16_t sd_id, uint16_t host_id);
extern struct mmc_card *mmc_scan_init(uint16_t sd_id, uint16_t sdc_id, void *card_param);

/* LVGL定时器周期 (ms) */
#define LVGL_TIMER_PERIOD    5

/* 显示刷新任务周期 (ms) - 优化二：提高响应速度 */
#define DISP_TASK_PERIOD     20

/* 线程栈 — 全部从 SRAM 堆分配，须控制总量（堆约 95KB） */
#define DISP_TASK_STACK_SIZE   (8 * 1024)
#define LVGL_TASK_STACK_SIZE   (32 * 1024)

/* LVGL线程句柄 - 用于刷新时挂起LVGL任务防止SPI冲突 */
OS_Thread_t lvgl_thread;
static OS_Thread_t disp_task_thread;

/* HTTP对话框 */
static lv_obj_t *g_http_dialog = NULL;
static lv_obj_t *g_http_dialog_bg = NULL;

/* Home 顶部 WiFi 状态栏已删除 (按设计简化 UI) */

/* WiFi/HTTP 状态全部在 wifi_controller.g_wifi 里, 不再散落 */

/* VBAT电压显示 */
static lv_obj_t *g_vbat_label = NULL;
static char g_vbat_text[16] = "";
static uint8_t g_adc_inited = 0;
static volatile int g_vbat_retry_countdown = 0;   /* ADC 读失败后快速重试计数 (20ms/格) */

/* Home 顶部 WiFi 图标 (仅 CONNECTED 时显示) */
static lv_obj_t *g_wifi_icon_label = NULL;
/* Home 顶部时间 (授时后显示 HH:MM) */
static lv_obj_t *g_time_label = NULL;

/* PA6 按键屏保触发 */
#define PA6_BUTTON_PIN  GPIO_PIN_6

/* Settings 页面引用 */
static lv_obj_t *g_settings_scr = NULL;
static lv_obj_t *g_settings_http_btn = NULL;

/* 全局样式 - 必须在文件作用域，因为 main_ui_create() 可被多次调用 */
static lv_style_t style_no_anim;
static lv_style_t style_sw;
static lv_style_t style_tile;
static lv_style_t style_tile_pressed;

static void ui_apply_static_btn_style(lv_obj_t *btn, int border_width, int radius)
{
    if (!btn) return;

    lv_obj_set_style_transition(btn, NULL, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_anim_time(btn, 0, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_bg_color(btn, lv_color_white(), LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_border_width(btn, border_width, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_border_color(btn, lv_color_make(0, 0, 0), LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_radius(btn, radius, LV_PART_MAIN | LV_STATE_ANY);

    uint32_t child_cnt = lv_obj_get_child_cnt(btn);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(btn, i);
        if (child) {
            lv_obj_set_style_transition(child, NULL, LV_PART_ANY | LV_STATE_ANY);
            lv_obj_set_style_anim_time(child, 0, LV_PART_ANY | LV_STATE_ANY);
            lv_obj_set_style_text_color(child, lv_color_make(0, 0, 0), LV_PART_MAIN | LV_STATE_ANY);
        }
    }
}

/* platform_init声明 */
extern void platform_init(void);
/* 省电优化: 仅 level0 (flash/image/PSRAM/cache), 跳过 level1(WiFi)/level2(SD)。
 * platform_init_level0 是 __weak 函数(platform_init.c:548), 未在头文件声明。 */
extern void platform_init_level0(void);
/* 拆开 level1/level2: 让 boot_screen 能在 level0 之后、WiFi 之前显示。 */
extern void platform_init_level1(void);
extern void platform_init_level2(void);

/* 前向声明 - 解决main_ui_create在定义前被调用的问题 */
void main_ui_create(void);
static void settings_btn_event_handler(lv_event_t * e);
static void settings_sleep_clock_btn_cb(lv_event_t * e);
static void settings_sd_diag_btn_cb(lv_event_t *e);
static void coremark_btn_event_handler(lv_event_t *e);
static void settings_test_project_btn_cb(lv_event_t *e);
static void clock_nowifi_dialog(void);

/*====================
 * SD卡测试函数
 *===================*/

/* SD卡测试函数 - 完整的init, scan, test, bench并列出文件 */
static void sd_benchmark_test(void)
{
    int32_t err;
    struct mmc_card *card;
    OS_Time_t tick_start, tick_end;
    uint32_t throughput_kb, throughput_mb;
    uint8_t *buf;
    int i;
    
    printf("\r\n");
    printf("======================================\r\n");
    printf("SD Card Full Test\r\n");
    printf("======================================\r\n\r\n");
    
    /* 1. 初始化SD卡控制器 */
    printf("[1/4] SD Card Init...\r\n");
    mmc_test_init(0, NULL, 0);
    printf("[OK] SD Card Init done\r\n\r\n");
    
    /* 2. 扫描SD卡 */
    printf("[2/4] SD Card Scan...\r\n");
    card = mmc_scan_init(0, 0, NULL);
    if (!card) {
        printf("[ERROR] No SD card found!\r\n");
        mmc_test_exit(0, 0);
        return;
    }
    printf("[OK] SD Card found!\r\n");
    printf("  Card ID: %d\r\n", card->id);
    printf("  Card Type: %d\r\n", card->type);
    printf("  RCA: 0x%04x\r\n", card->rca);
    printf("  OCR: 0x%08x\r\n", card->ocr.ocr);
    printf("  Capacity: %u bytes\r\n", card->csd.capacity);
    printf("\r\n");
    
    /* 3. 性能测试 - 读写测试 */
    printf("[3/4] Read/Write Performance Test...\r\n");
    buf = (uint8_t *)malloc(16 * 512);  /* 16 sectors = 8KB */
    if (!buf) {
        printf("[ERROR] malloc failed\r\n");
        mmc_card_close(0);
        mmc_test_exit(0, 0);
        return;
    }
    
    /* 填充测试数据 */
    for (i = 0; i < 16 * 512 / 4; i++) {
        ((uint32_t *)buf)[i] = i;
    }
    
    /* 写测试 */
    tick_start = OS_GetTicks();
    err = mmc_block_write(card, buf, 1000, 16);
    tick_end = OS_GetTicks();
    if (err == 0) {
        uint32_t tick_used = OS_TicksToMSecs(tick_end - tick_start);
        uint32_t bytesWritten = 16 * 512;
        if (tick_used == 0) tick_used = 1;
        throughput_kb = bytesWritten * 1000 / 1024 / tick_used;
        throughput_mb = throughput_kb / 1024;
        printf("  Write: %d bytes in %d ms -> %d.%d MB/s\r\n",
               bytesWritten, tick_used, throughput_mb, throughput_kb % 1024);
    } else {
        printf("  Write FAILED, err=%d\r\n", err);
    }
    
    /* 清空缓冲区并读取 */
    memset(buf, 0, 16 * 512);
    tick_start = OS_GetTicks();
    err = mmc_block_read(card, buf, 1000, 16);
    tick_end = OS_GetTicks();
    if (err == 0) {
        uint32_t tick_used = OS_TicksToMSecs(tick_end - tick_start);
        uint32_t bytesRead = 16 * 512;
        if (tick_used == 0) tick_used = 1;
        throughput_kb = bytesRead * 1000 / 1024 / tick_used;
        throughput_mb = throughput_kb / 1024;
        printf("  Read:  %d bytes in %d ms -> %d.%d MB/s\r\n",
               bytesRead, tick_used, throughput_mb, throughput_kb % 1024);
    } else {
        printf("  Read FAILED, err=%d\r\n", err);
    }
    
    /* 数据验证 */
    err = 0;
    for (i = 0; i < 16 * 512 / 4; i++) {
        if (((uint32_t *)buf)[i] != i) {
            printf("  Data verification FAILED at index %d\r\n", i);
            err = -1;
            break;
        }
    }
    if (err == 0) {
        printf("  Data verification: PASSED\r\n");
    }
    
    free(buf);
    printf("\r\n");
    
    /* 4. 清理 */
    printf("[4/4] Cleanup...\r\n");
    mmc_card_close(0);
    mmc_test_exit(0, 0);
    printf("[OK] Cleanup done\r\n\r\n");
    
    printf("======================================\r\n");
    printf("SD Card Full Test Complete\r\n");
    printf("======================================\r\n\r\n");
}

/*====================
 * FatFs文件系统测试
 *===================*/

/* 获取SD卡容量信息 */
static int fatfs_get_card_info(uint32_t *free_capacity, uint32_t *total_capacity)
{
    DWORD nclst;
    FATFS *fs;
    
    if (f_getfree("0:/", &nclst, &fs) == FR_OK) {
        uint32_t sector_size;
#if (_MAX_SS == _MIN_SS)
        sector_size = _MAX_SS;
#else
        sector_size = fs->ssize;
#endif
        *free_capacity = (uint32_t)((float)fs->free_clst * fs->csize / 1024 / 1024 * sector_size);
        *total_capacity = (uint32_t)((float)(fs->n_fatent - 2) * fs->csize / 1024 / 1024 * sector_size);
        printf("[FatFs] Free clusters: %u, Cluster size: %u, Sector size: %u\r\n",
               (unsigned int)fs->free_clst, (unsigned int)fs->csize, (unsigned int)sector_size);
    } else {
        printf("[FatFs ERR] Get card info failed\r\n");
        return -1;
    }
    
    return 0;
}

/* 非递归扫描文件 - 使用显式栈避免栈溢出 */
#define MAX_DEPTH 16
#define MAX_PATH_STACK 32

static FRESULT fatfs_scan_files(char *path)
{
    FRESULT res;
    DIR dir;
    static FILINFO fno;
    
    static char path_stack[MAX_PATH_STACK][256];
    static int stack_top = 0;
    int scanned_dirs = 0;
    
    /* 初始化 - 将根目录压入栈 */
    strcpy(path_stack[0], path);
    stack_top = 1;
    
    while (stack_top > 0) {
        /* 防垃圾文件系统 (格式化中断/卡损坏) 导致无限遍历 */
        if (++scanned_dirs > MAX_PATH_STACK * 4) {
            printf("[FatFs WRN] scan aborted: too many dirs, FS may be corrupted\n");
            break;
        }
        /* 弹出栈顶目录 */
        stack_top--;
        strcpy(path, path_stack[stack_top]);
        
        /* 打开目录 */
        res = f_opendir(&dir, path);
        if (res != FR_OK) {
            printf("[FatFs WRN] f_opendir '%s' failed, res=%d\r\n", path, res);
            continue;
        }
        
        /* 遍历目录项 */
        while (1) {
            res = f_readdir(&dir, &fno);
            if (res != FR_OK || fno.fname[0] == 0) break;
            
            if (fno.fattrib & AM_DIR) {
                /* 是目录 - 压入栈中待后续处理; 长度超限跳过,
                 * 防止垃圾文件名把 256B 路径缓冲打爆 (历史 usage fault) */
                if (stack_top < MAX_PATH_STACK - 1 &&
                    (int)strlen(path) + 1 + (int)strlen(fno.fname) < 256) {
                    sprintf(path_stack[stack_top], "%s/%s", path, fno.fname);
                    stack_top++;
                } else {
                    printf("[FatFs WRN] Path stack full / path too long, skip dir: %s/%s\r\n", path, fno.fname);
                }
            } else {
                /* 是文件 - 不再打印 (减少启动日志) */
            }
        }
        
        f_closedir(&dir);
    }
    
    return FR_OK;
}

/* 创建测试文件 */
static int fatfs_create_test_file(const char *filename, const char *content)
{
    FIL fp;
    FRESULT res;
    UINT bw;
    
    res = f_open(&fp, filename, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) {
        printf("[FatFs ERR] Create file %s failed, res=%d\r\n", filename, res);
        return -1;
    }
    
    res = f_write(&fp, content, strlen(content), &bw);
    if (res != FR_OK) {
        printf("[FatFs ERR] Write file %s failed, res=%d\r\n", filename, res);
        f_close(&fp);
        return -1;
    }
    
    f_close(&fp);
    printf("[FatFs] Created file: %s (%u bytes)\r\n", filename, (unsigned int)bw);
    return 0;
}

/* 读取测试文件 */
static int fatfs_read_test_file(const char *filename)
{
    FIL fp;
    FRESULT res;
    char buf[256];
    UINT br;
    
    res = f_open(&fp, filename, FA_READ);
    if (res != FR_OK) {
        printf("[FatFs ERR] Open file %s failed, res=%d\r\n", filename, res);
        return -1;
    }
    
    printf("[FatFs] Reading file %s:\r\n", filename);
    while (1) {
        res = f_read(&fp, buf, sizeof(buf) - 1, &br);
        if (res != FR_OK || br == 0) break;
        buf[br] = '\0';
        printf("  > %s", buf);
    }
    printf("\r\n");
    
    f_close(&fp);
    return 0;
}

/* 删除测试文件 */
static int fatfs_delete_test_file(const char *filename)
{
    FRESULT res;
    
    res = f_unlink(filename);
    if (res != FR_OK) {
        printf("[FatFs ERR] Delete file %s failed, res=%d\r\n", filename, res);
        return -1;
    }
    
    printf("[FatFs] Deleted file: %s\r\n", filename);
    return 0;
}

/* FatFs文件系统完整测试 - 只读版本（不创建文件） */
static void fatfs_filesystem_test(void)
{
    FRESULT res;
    uint32_t free_cap, total_cap;
    
    printf("\r\n");
    printf("======================================\r\n");
    printf("FatFs File System Test (Read Only)\r\n");
    printf("======================================\r\n\r\n");
    
    /* 1. 挂载文件系统 */
    printf("[1/3] Mount FatFs File System...\r\n");
    if (fs_ctrl_mount(FS_MNT_DEV_TYPE_SDCARD, 0) != 0) {
        printf("[ERROR] Mount failed! (用控制台 'format yes' 可重建卷)\r\n");
        return;
    }
    printf("[OK] FatFs mounted successfully\r\n\r\n");

    /* 2. 获取容量信息 */
    printf("[2/3] Get Card Capacity Info...\r\n");
    if (fatfs_get_card_info(&free_cap, &total_cap) == 0) {
        printf("  Total: %u MB, Free: %u MB\r\n", total_cap, free_cap);
        /* 只告警不自动格式化: 防误检测清掉用户数据, 修复交给控制台 'format yes' */
        if (total_cap == 0 || total_cap > 128 * 1024 || free_cap > total_cap) {
            printf("[FS WRN] volume params look insane, run 'format yes' to rebuild\r\n");
        }
    }
    printf("\r\n");
    
    /* 3. 创建目录（如果不存在） */
    printf("[3/4] Creating directories...\r\n");
    res = f_mkdir("/Font");
    if (res == FR_OK || res == FR_EXIST) {
        printf("[OK] /Font ready\r\n");
    }
    res = f_mkdir("/Inkbook");
    if (res == FR_OK || res == FR_EXIST) {
        printf("[OK] /Inkbook ready\r\n");
    }
    printf("\r\n");
    
    /* 4. 扫描根目录文件 */
    printf("[4/4] Scan Root Directory...\r\n");
    char path[256];
    strcpy(path, "/");
    res = fatfs_scan_files(path);
    if (res != FR_OK) {
        printf("[FatFs WRN] Scan directory failed, res=%d\r\n", res);
    }
    printf("\r\n");
    
    /* 不卸载文件系统，保持SD卡一直挂载 */
    printf("[OK] File system stays mounted\r\n");
    
    printf("======================================\r\n");
    printf("FatFs File System Test Complete\r\n");
    printf("======================================\r\n\r\n");
}

/*====================
 * LVGL主线程
 *===================*/

/*====================
 * 开关事件处理函数 - CLICKED模式（墨水屏优化）
 * 核心逻辑：使用LV_EVENT_CLICKED避开高频中断抖动
 * - LV_EVENT_CLICKED只在"按下且在对象范围内抬手"时触发一次
 * - 完美避开了触摸IC高频中断的干扰
 *===================*/

/* 关闭 HTTP 对话框并停止服务器（Stop 按钮 / 点背景 / 点对话框空白 复用）。
 * 只做非阻塞动作：清状态 + shutdown socket，由 HTTP 工作线程自行收尾；
 * 切勿在回调里等线程退出，否则 UI 会卡死。 */
static void http_dialog_stop_and_dismiss(void)
{
    g_wifi.http_running = 0;
    wifi_controller_set_http_running(0);

    if (g_http_dialog_bg) {
        lv_obj_del(g_http_dialog_bg);
        g_http_dialog_bg = NULL;
        g_http_dialog = NULL;
    }

    /* 先重绘底层设置页，再刷新 EPD（勿在回调里阻塞等 HTTP 线程） */
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
    http_server_request_stop();
    epd_mark_refresh_pending();
}

/* HTTP对话框停止按钮回调 */
static void http_dialog_stop_cb(lv_event_t *e) {
    (void)e;
    printf("[HTTP_DIALOG] Stop button clicked\n");
    http_dialog_stop_and_dismiss();
}

/* 点对话框外的全屏遮罩 → 同样停止服务器并关闭。
 * 修复：原来遮罩无事件处理，按钮只有 80x35 且紧贴对话框底边，
 * 点偏一点（如 Y=287）就落在遮罩上，屏幕毫无变化、服务器永远退不出。 */
static void http_dialog_bg_click_cb(lv_event_t *e) {
    (void)e;
    printf("[HTTP_DIALOG] Background tapped, stopping\n");
    http_dialog_stop_and_dismiss();
}

/* 点对话框空白处 → 与 Stop 按钮等效，杜绝"点不到按钮就出不去" */
static void http_dialog_click_cb(lv_event_t *e) {
    (void)e;
    printf("[HTTP_DIALOG] Dialog tapped, stopping\n");
    http_dialog_stop_and_dismiss();
}

/* 创建HTTP服务器信息对话框 */
static void http_dialog_create(const char *ip_str) {
    if (g_http_dialog_bg) return; /* 已存在 */
    
    /* 全屏遮罩 */
    g_http_dialog_bg = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(g_http_dialog_bg);
    lv_obj_set_size(g_http_dialog_bg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g_http_dialog_bg, 0, 0);
    lv_obj_set_style_bg_color(g_http_dialog_bg, lv_color_make(255, 255, 255), 0);
    lv_obj_set_style_bg_opa(g_http_dialog_bg, LV_OPA_90, 0);
    /* 点遮罩(对话框外)即可停止服务器并退出，避免点不到按钮时卡死在对话框 */
    lv_obj_add_event_cb(g_http_dialog_bg, http_dialog_bg_click_cb, LV_EVENT_CLICKED, NULL);
    
    /* 对话框容器 */
    g_http_dialog = lv_obj_create(g_http_dialog_bg);
    lv_obj_set_size(g_http_dialog, 200, 150);
    lv_obj_align(g_http_dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(g_http_dialog, lv_color_make(255, 255, 255), 0);
    lv_obj_set_style_border_width(g_http_dialog, 2, 0);
    lv_obj_set_style_border_color(g_http_dialog, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_radius(g_http_dialog, 8, 0);
    lv_obj_clear_flag(g_http_dialog, LV_OBJ_FLAG_SCROLLABLE);
    /* 点对话框空白处同样停止服务器并退出（与按钮等效，加大可命中区域） */
    lv_obj_add_event_cb(g_http_dialog, http_dialog_click_cb, LV_EVENT_CLICKED, NULL);
    
    /* 标题 */
    lv_obj_t *dlg_title = lv_label_create(g_http_dialog);
    lv_label_set_text(dlg_title, "HTTP Server");
    lv_obj_set_style_text_font(dlg_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(dlg_title, lv_color_make(0, 0, 0), 0);
    lv_obj_align(dlg_title, LV_ALIGN_TOP_MID, 0, 10);
    
    /* IP地址 */
    lv_obj_t *dlg_ip = lv_label_create(g_http_dialog);
    char ip_text[64];
    snprintf(ip_text, sizeof(ip_text), "http://%s", ip_str);
    lv_label_set_text(dlg_ip, ip_text);
    lv_obj_set_style_text_font(dlg_ip, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(dlg_ip, lv_color_make(0, 0, 0), 0);
    lv_obj_align(dlg_ip, LV_ALIGN_CENTER, 0, -10);
    
    /* 停止按钮 - 加大尺寸(150x42), 覆盖对话框底部大部分区域, 墨水屏触摸易命中 */
    lv_obj_t *btn_stop = lv_btn_create(g_http_dialog);
    lv_obj_set_size(btn_stop, 150, 42);
    lv_obj_align(btn_stop, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_color(btn_stop, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_border_width(btn_stop, 2, 0);
    lv_obj_set_style_border_color(btn_stop, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_radius(btn_stop, 4, 0);
    lv_obj_set_style_transition(btn_stop, NULL, LV_PART_MAIN);
    lv_obj_t *btn_stop_label = lv_label_create(btn_stop);
    lv_label_set_text(btn_stop_label, "Stop");
    lv_obj_set_style_text_font(btn_stop_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(btn_stop_label, lv_color_make(255, 255, 255), 0);
    lv_obj_center(btn_stop_label);
    lv_obj_add_event_cb(btn_stop, http_dialog_stop_cb, LV_EVENT_CLICKED, NULL);
    
    epd_mark_refresh_pending();
}

/* Settings WiFi 开关已合并到 settings_screen.c 的 wifi_switch_event_handler
 * (手机式: 开关和扫描在同一页). 此函数废弃. */

/* Settings HTTP Server按钮事件处理 */
static void settings_http_btn_event_handler(lv_event_t * e) {
    printf("[Settings] HTTP Server button clicked\n");

    if (g_wifi.phase != WLAN_PHASE_CONNECTED) {
        printf("[Settings] WiFi not connected, cannot start HTTP\n");
        return;
    }

    if (g_wifi.http_running || http_server_is_running()) {
        printf("[Settings] HTTP already running\n");
        return;
    }

    /* 启动HTTP服务器（阻塞） */
    http_server_init(HTTP_SERVER_PORT);
    if (http_server_start() == 0) {
        printf("[Settings] HTTP server started!\n");
        g_wifi.http_running = 1;
        wifi_controller_set_http_running(1);

        /* 提取 IP 地址显示 */
        char ip_str[32];
        strncpy(ip_str, g_wifi.ip, sizeof(ip_str) - 1);
        ip_str[sizeof(ip_str) - 1] = '\0';

        /* 弹出对话框 */
        http_dialog_create(ip_str);
    } else {
        printf("[Settings] HTTP start failed!\n");
    }
}

/* 设置页返回: 清理设置状态并重建首页 (屏幕返回与物理返回共用) */
static void settings_goto_home(void)
{
    printf("[Settings] Back to home\n");

    g_wifi.settings_paused = 0;
    
    /* 使用 lv_obj_clean 清空内容，而不是 lv_obj_del 删除屏幕
     * 避免删除当前活动屏幕后 lv_scr_act() 返回无效指针
     */
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);

    /* 清理 Settings 相关指针 */
    g_settings_scr = NULL;
    g_settings_http_btn = NULL;

    /* 重建首页 */
    main_ui_create();

    epd_mark_refresh_pending();
}

/* 物理返回按键: 与设置页屏幕返回按钮等效 */
static void settings_physical_back(void)
{
    settings_goto_home();
}

/* Settings 返回按钮事件处理 */
static void settings_back_btn_event_handler(lv_event_t * e) {
    (void)e;
    settings_goto_home();
}

/* WiFi扫描按钮回调 - 打开WiFi扫描子界面 */
static void settings_wifi_scan_open_btn_cb(lv_event_t *e) {
    (void)e;
    printf("[Settings] Opening WiFi scan screen\n");
    epd_pause_refresh();
    lv_obj_t *scr = lv_scr_act();
    settings_wifi_scan_open(scr);
    epd_resume_refresh();
}

/* 字体选择按钮回调 - 打开字体选择子界面 */
static void settings_font_sel_btn_cb(lv_event_t *e) {
    (void)e;
    printf("[Settings] Opening font select screen\n");
    epd_pause_refresh();
    lv_obj_t *scr = lv_scr_act();
    settings_font_select_open(scr);
    epd_resume_refresh();
}

/* 关于按钮回调 - 打开关于界面 (固件信息 -> 作者信息) */
static void settings_about_btn_cb(lv_event_t *e) {
    (void)e;
    printf("[Settings] Opening about screen\n");
    epd_pause_refresh();
    lv_obj_t *scr = lv_scr_act();
    settings_about_open(scr);
    epd_resume_refresh();
}

/* 休眠时钟精度选择 UI: 点击进入前先选 1/3/5/10/15/30/60 分钟 + 横/竖屏, 确认后再进 */
static int     s_clock_sel_min = 1;
static int     s_clock_landscape = 0;
static lv_obj_t *s_clock_picker_ov = NULL;
static lv_obj_t *s_clock_opt_btns[7] = {0};
static lv_obj_t *s_clock_orient_btn = NULL;
static const int s_clock_opts[7] = {1, 3, 5, 10, 15, 30, 60};

static void clock_picker_close(void)
{
    if (s_clock_picker_ov) {
        lv_obj_del_async(s_clock_picker_ov);
        s_clock_picker_ov = NULL;
    }
}

static void clock_picker_highlight(void)
{
    for (int i = 0; i < 7; i++) {
        if (!s_clock_opt_btns[i]) continue;
        bool sel = (s_clock_opts[i] == s_clock_sel_min);
        lv_obj_set_style_bg_color(s_clock_opt_btns[i],
                                  sel ? lv_color_make(0xDD, 0xDD, 0xDD) : lv_color_white(), 0);
        lv_obj_set_style_border_width(s_clock_opt_btns[i], sel ? 2 : 1, 0);
    }
}

static void clock_picker_opt_cb(lv_event_t *e)
{
    s_clock_sel_min = (int)(intptr_t)lv_event_get_user_data(e);
    printf("[CLOCK] picker select=%d min\n", s_clock_sel_min);
    clock_picker_highlight();
}

static void clock_picker_orient_refresh(void)
{
    if (!s_clock_orient_btn) return;
    lv_obj_t *lbl = lv_obj_get_child(s_clock_orient_btn, 0);
    if (lbl) lv_label_set_text(lbl, s_clock_landscape ? "显示方向 : 横屏" : "显示方向 : 竖屏");
    lv_obj_set_style_bg_color(s_clock_orient_btn,
                              s_clock_landscape ? lv_color_make(0xDD, 0xDD, 0xDD) : lv_color_white(), 0);
}

static void clock_picker_orient_cb(lv_event_t *e) {
    (void)e;
    s_clock_landscape = s_clock_landscape ? 0 : 1;
    printf("[CLOCK] picker orientation=%s\n", s_clock_landscape ? "landscape" : "portrait");
    clock_picker_orient_refresh();
}

static void clock_picker_confirm_cb(lv_event_t *e) {
    (void)e;
    clock_set_interval_min(s_clock_sel_min);
    clock_set_landscape(s_clock_landscape);
    printf("[CLOCK] confirm interval=%d min landscape=%d\n", s_clock_sel_min, s_clock_landscape);
    clock_picker_close();
    epd_mark_refresh_pending();
    /* 未连接 WiFi: 时间无法 NTP 校准, 先询问用户是否进入(并显示当前 RTC 时间) */
    if (g_wifi.phase != WLAN_PHASE_CONNECTED) {
        printf("[CLOCK] WiFi not connected, asking user before entering\n");
        clock_nowifi_dialog();
        return;
    }
    /* clock_mode_enter 内部会 NTP->RTC、刷新首帧、关停外设、进 HIBERNATION */
    clock_mode_enter();
}

/* 未连接 WiFi 时进入休眠时钟的确认对话框 */
static lv_obj_t *g_clock_nowifi_dlg = NULL;

static void clock_nowifi_dlg_dismiss(void)
{
    if (g_clock_nowifi_dlg) {
        lv_obj_del(g_clock_nowifi_dlg);
        g_clock_nowifi_dlg = NULL;
        epd_mark_refresh_pending();
    }
}

static void clock_nowifi_enter_cb(lv_event_t *e) {
    (void)e;
    clock_nowifi_dlg_dismiss();
    clock_mode_enter();
}

static void clock_nowifi_cancel_cb(lv_event_t *e) {
    (void)e;
    clock_nowifi_dlg_dismiss();
}

static void clock_nowifi_dialog(void)
{
    if (g_clock_nowifi_dlg) return;

    char time_str[16];
    if (clock_format_rtc_time(time_str, sizeof(time_str)) != 0) {
        snprintf(time_str, sizeof(time_str), "--:--:--");
    }

    lv_obj_t *bg = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(bg);
    lv_obj_set_size(bg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_set_style_bg_color(bg, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_90, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    g_clock_nowifi_dlg = bg;

    lv_obj_t *box = lv_obj_create(bg);
    lv_obj_set_size(box, 210, 170);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    epd_disable_all_animations_recursive(box);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "未连接 WiFi");
    lv_obj_set_style_text_font(title, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    char msg1[48];
    snprintf(msg1, sizeof(msg1), "当前RTC时间: %s", time_str);
    lv_obj_t *lbl_time = lv_label_create(box);
    lv_label_set_text(lbl_time, msg1);
    lv_obj_set_style_text_font(lbl_time, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(lbl_time, lv_color_make(0x44, 0x44, 0x44), 0);
    lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, 0, 36);

    lv_obj_t *lbl_warn = lv_label_create(box);
    lv_label_set_text(lbl_warn, "时间可能不准确\n仍要进入休眠时钟吗?");
    lv_obj_set_style_text_font(lbl_warn, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(lbl_warn, lv_color_make(0x66, 0x66, 0x66), 0);
    lv_obj_align(lbl_warn, LV_ALIGN_TOP_MID, 0, 68);

    /* 进入 / 取消 */
    lv_obj_t *ok = lv_btn_create(box);
    lv_obj_set_size(ok, 80, 40);
    lv_obj_set_pos(ok, 12, 120);
    ui_apply_static_btn_style(ok, 2, 4);
    lv_obj_add_event_cb(ok, clock_nowifi_enter_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *okl = lv_label_create(ok);
    lv_label_set_text(okl, "进入");
    lv_obj_set_style_text_font(okl, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(okl, lv_color_black(), 0);
    lv_obj_center(okl);

    lv_obj_t *cancel = lv_btn_create(box);
    lv_obj_set_size(cancel, 80, 40);
    lv_obj_set_pos(cancel, 118, 120);
    ui_apply_static_btn_style(cancel, 2, 4);
    lv_obj_add_event_cb(cancel, clock_nowifi_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "取消");
    lv_obj_set_style_text_font(cl, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(cl, lv_color_black(), 0);
    lv_obj_center(cl);

    lv_obj_invalidate(bg);
    lv_refr_now(NULL);
    epd_mark_refresh_pending();
}

static void clock_picker_cancel_cb(lv_event_t *e) {
    (void)e;
    clock_picker_close();
    epd_mark_refresh_pending();
}

static void clock_precision_picker_show(void)
{
    if (s_clock_picker_ov) return;
    s_clock_sel_min = clock_get_interval_min();
    s_clock_landscape = clock_get_landscape();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *ov = lv_obj_create(scr);
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    epd_disable_all_animations_recursive(ov);
    s_clock_picker_ov = ov;

    lv_obj_t *title = lv_label_create(ov);
    lv_label_set_text(title, "休眠时钟精度");
    lv_obj_set_style_text_font(title, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *sub = lv_label_create(ov);
    lv_label_set_text(sub, "选择唤醒刷新间隔");
    lv_obj_set_style_text_font(sub, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(sub, lv_color_make(0x66, 0x66, 0x66), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 32);

    /* 精度选项: 2 列网格, 按钮高度统一 50px(与设置页行按钮一致, 便于点按) */
    static const int col_x[2] = {10, 125};
    const int opt_h = 50, opt_y0 = 48, opt_pitch = 60;
    for (int i = 0; i < 7; i++) {
        lv_obj_t *b = lv_btn_create(ov);
        int r = i / 2, c = i % 2;
        lv_obj_set_size(b, 105, opt_h);
        lv_obj_set_pos(b, col_x[c], opt_y0 + r * opt_pitch);
        ui_apply_static_btn_style(b, 1, 4);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        epd_disable_all_animations_recursive(b);
        char txt[16];
        snprintf(txt, sizeof(txt), "%d 分钟", s_clock_opts[i]);
        lv_obj_t *lb = lv_label_create(b);
        lv_label_set_text(lb, txt);
        lv_obj_set_style_text_font(lb, &lv_font_misans_16, 0);
        lv_obj_set_style_text_color(lb, lv_color_black(), 0);
        lv_obj_center(lb);
        lv_obj_add_event_cb(b, clock_picker_opt_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)s_clock_opts[i]);
        s_clock_opt_btns[i] = b;
    }
    clock_picker_highlight();

    /* 显示方向切换(竖屏/横屏), 整宽 50px */
    s_clock_orient_btn = lv_btn_create(ov);
    lv_obj_set_size(s_clock_orient_btn, 220, opt_h);
    lv_obj_set_pos(s_clock_orient_btn, 10, opt_y0 + 4 * opt_pitch);
    ui_apply_static_btn_style(s_clock_orient_btn, 1, 4);
    lv_obj_clear_flag(s_clock_orient_btn, LV_OBJ_FLAG_SCROLLABLE);
    epd_disable_all_animations_recursive(s_clock_orient_btn);
    lv_obj_add_event_cb(s_clock_orient_btn, clock_picker_orient_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *olb = lv_label_create(s_clock_orient_btn);
    lv_label_set_text(olb, "显示方向");
    lv_obj_set_style_text_font(olb, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(olb, lv_color_black(), 0);
    lv_obj_center(olb);
    clock_picker_orient_refresh();

    /* 确认 / 取消, 高度 50px 与设置按钮一致 */
    lv_obj_t *ok = lv_btn_create(ov);
    lv_obj_set_size(ok, 90, opt_h);
    lv_obj_set_pos(ok, 20, opt_y0 + 5 * opt_pitch);
    ui_apply_static_btn_style(ok, 2, 4);
    lv_obj_add_event_cb(ok, clock_picker_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *okl = lv_label_create(ok);
    lv_label_set_text(okl, "确认");
    lv_obj_set_style_text_font(okl, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(okl, lv_color_black(), 0);
    lv_obj_center(okl);

    lv_obj_t *cn = lv_btn_create(ov);
    lv_obj_set_size(cn, 90, opt_h);
    lv_obj_set_pos(cn, 130, opt_y0 + 5 * opt_pitch);
    ui_apply_static_btn_style(cn, 2, 4);
    lv_obj_add_event_cb(cn, clock_picker_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cnl = lv_label_create(cn);
    lv_label_set_text(cnl, "取消");
    lv_obj_set_style_text_font(cnl, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(cnl, lv_color_black(), 0);
    lv_obj_center(cnl);

    lv_obj_invalidate(ov);
    lv_refr_now(NULL);
    epd_mark_refresh_pending();
}

/* 休眠时钟按钮回调(Settings 入口) - 先选精度再进 */
static void settings_sleep_clock_btn_cb(lv_event_t * e) {
    (void)e;
    printf("[Settings] Opening sleep clock precision picker\n");
    clock_precision_picker_show();
}

/* 首页休眠时钟按钮回调 (fix-power-saving: 把时钟入口提到首页) */
static void home_clock_btn_event_handler(lv_event_t *e) {
    (void)e;
    printf("[Home] Opening sleep clock precision picker\n");
    clock_precision_picker_show();
}

/*====================
 * Home 顶部 WiFi 状态栏 (由 wifi_controller phase 回调驱动)
 *===================*/

/* 在 LVGL 线程中由 wlan_manager_poll() 触发
 * 主要作用: WiFi 掉了自动停 HTTP + 更新 home WiFi 图标 */

/* 同步首页 WiFi 图标到当前 phase，并尽快刷到墨水屏 */
static void home_time_label_apply(int force_epd)
{
    if (!g_time_label) {
        return;
    }
    if (time_sync_is_valid() && time_sync_get_text()[0]) {
        lv_label_set_text(g_time_label, time_sync_get_text());
    } else {
        lv_label_set_text(g_time_label, "");
    }
    if (force_epd) {
        lv_obj_invalidate(g_time_label);
        lv_refr_now(NULL);
        epd_mark_refresh_pending();
    }
}

static void home_wifi_icon_sync(int force_epd)
{
    if (!g_wifi_icon_label) {
        return;
    }

    if (g_wifi.phase == WLAN_PHASE_CONNECTED) {
        lv_label_set_text(g_wifi_icon_label, LV_SYMBOL_WIFI);
    } else {
        lv_label_set_text(g_wifi_icon_label, "");
    }

    if (force_epd) {
        lv_obj_invalidate(g_wifi_icon_label);
        /* early WiFi 常在首帧渲染之后才 CONNECTED；先 refr 进 fb，再请求 EPD */
        lv_refr_now(NULL);
        epd_mark_refresh_pending();
    }
}

static void on_wifi_phase_change(WLAN_Phase_t phase, void *user_data)
{
    (void)user_data;
    printf("[WIFIC] Phase callback: %d\n", (int)phase);

    if (phase == WLAN_PHASE_CONNECTING) {
        /* 关联阶段不刷首页/settings，减少与 WPA 并发的 EPD 请求 */
        return;
    }

    /* Home 顶部 WiFi 图标：CONNECTED 显示，其它清空并刷新 */
    home_wifi_icon_sync(1);

    /* WiFi 掉了 -> 自动停 HTTP */
    if (phase != WLAN_PHASE_CONNECTED && g_wifi.http_running) {
        printf("[WIFIC] WiFi lost, stopping HTTP server\n");
        http_server_stop();
        g_wifi.http_running = 0;
        if (g_http_dialog_bg) {
            lv_obj_del(g_http_dialog_bg);
            g_http_dialog_bg = NULL;
            g_http_dialog = NULL;
        }
    }

    /* 通知 settings WiFi 界面更新开关行 */
    settings_wifi_on_phase_change(phase);
}

/* SD 卡写诊断: 测单块/多块写入是否可靠。
 * 观察日志判断是卡质量问题还是硬件竞争:
 *   - 单块写(BSZ=1)也报 DCE → 卡/电源问题
 *   - 只有多块写(BSZ≥4)报 DCE → DMA 时序/Bus 竞争
 *   - 关了 WiFi 就不报 → WiFi + SD 并发竞争
 */
static void sd_write_diag_run(void)
{
    FIL fp;
    FRESULT fr;
    UINT bw;
    int bs, retry, ok, fail;
    static const int test_bsz[] = {1, 2, 4, 6, 8, 0};  /* sector counts */
    uint8_t buf[4096] __attribute__((aligned(64)));

    printf("\r\n=== SD Write Diagnostic ===\r\n");
    printf("[SD-DIAG] VBAT=%s charging=%d\r\n", g_vbat_text,
           (int)HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_21) == 0);

    memset(buf, 0xA5, sizeof(buf));

    for (bs = 0; test_bsz[bs] != 0; bs++) {
        int bsz = test_bsz[bs];
        int len = bsz * 512;
        ok = fail = 0;

        fr = f_open(&fp, "0:/_sd_diag.tmp", FA_CREATE_ALWAYS | FA_WRITE);
        if (fr != FR_OK) {
            printf("[SD-DIAG] open fail %d\r\n", (int)fr);
            return;
        }

        for (retry = 0; retry < 10; retry++) {
            f_lseek(&fp, retry * len);
            bw = 0;
            fr = f_write(&fp, buf, len, &bw);
            if (fr == FR_OK && bw == (UINT)len) {
                ok++;
            } else {
                fail++;
                printf("[SD-DIAG] BSZ=%d try=%d: fr=%d bw=%u/%d\r\n",
                       bsz, retry, (int)fr, (unsigned)bw, len);
                /* 等卡恢复 */
                OS_MSleep(200);
            }
        }
        f_close(&fp);
        printf("[SD-DIAG] BSZ=%d (%uB): ok=%d fail=%d\r\n",
               bsz, (unsigned)len, ok, fail);
    }
    f_unlink("0:/_sd_diag.tmp");
    printf("=== SD Write Diagnostic END ===\r\n\r\n");
}

/* Settings SD 诊断按钮回调 */
static void settings_sd_diag_btn_cb(lv_event_t *e)
{
    (void)e;
    printf("[Settings] SD Write Diagnostic triggered\r\n");
    sd_write_diag_run();
}

/* Settings 设置页整行按钮: 左侧24px图标(LV_SYMBOL) + 右侧16px中文(MiSans) */
static lv_obj_t *settings_row_create(lv_obj_t *parent,
                                     lv_coord_t y, lv_coord_t h,
                                     const char *symbol, const char *text,
                                     lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 220, h);
    lv_obj_set_pos(btn, 10, y);
    lv_obj_set_style_bg_color(btn, lv_color_make(255, 255, 255), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_transition(btn, NULL, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = lv_label_create(btn);
    lv_label_set_text(ic, symbol);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(ic, lv_color_make(0, 0, 0), 0);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *lb = lv_label_create(btn);
    lv_label_set_text(lb, text);
    lv_obj_set_style_text_font(lb, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(lb, lv_color_make(0, 0, 0), 0);
    lv_obj_align(lb, LV_ALIGN_LEFT_MID, 46, 0);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }
    return btn;
}

/* Settings 入口按钮事件处理 */
static void settings_btn_event_handler(lv_event_t * e) {
    printf("[Settings] Entering Settings page\n");

    /* 暂停后台 WiFi 连接, 避免 Settings 切页 EPD 刷新与 wlan_sta_enable 竞态卡死 lvgl/触摸 */
    g_wifi.settings_paused = 1;
    if (g_wifi.phase != WLAN_PHASE_CONNECTED) {
        wlan_manager_cancel_connect();
    }
    
    /* 【EPD优化】先暂停刷新，防止刷出半成品UI */
    epd_pause_refresh();
    
    lv_obj_t *scr = lv_scr_act();
    
    /* 清屏 */
    lv_obj_clean(scr);
    g_settings_scr = scr;
    g_wifi_icon_label = NULL;
    g_time_label = NULL;
    g_vbat_label = NULL;
    
    /* 应用无动画样式 */
    lv_obj_add_style(scr, &style_no_anim, LV_STATE_ANY);
    lv_obj_set_style_bg_color(scr, lv_color_make(255, 255, 255), 0);
    
    /* 标题: 自动尺寸+居中, 避免固定 200px 标签的文本左移落到返回按钮下方 */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "设置");
    lv_obj_set_style_text_font(title, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(title, lv_color_make(0, 0, 0), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* 返回按钮 - 统一左上角，尺寸与 FM 关闭按钮一致 */
    lv_obj_t *btn_back = lv_btn_create(scr);
    lv_obj_set_size(btn_back, 60, 30);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 4, 2);
    ui_apply_static_btn_style(btn_back, 2, 4);
    lv_obj_t *btn_back_label = lv_label_create(btn_back);
    lv_label_set_text(btn_back_label, "返回");
    lv_obj_set_style_text_font(btn_back_label, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(btn_back_label, lv_color_make(0, 0, 0), 0);
    lv_obj_center(btn_back_label);
    lv_obj_add_event_cb(btn_back, settings_back_btn_event_handler, LV_EVENT_CLICKED, NULL);

    /* 无线网络 - 纯入口, 开关已合并到 WiFi 子页 (手机式) */
    settings_row_create(scr, 78, 50, LV_SYMBOL_WIFI,
                        "无线网络", settings_wifi_scan_open_btn_cb);

    /* HTTP 服务器（仅 WiFi 连接时显示） */
    g_settings_http_btn = settings_row_create(scr, 125, 50, LV_SYMBOL_WIFI,
                                              "HTTP 服务器", settings_http_btn_event_handler);
    /* 根据 WiFi 状态决定是否显示 HTTP 按钮 */
    if (g_wifi.phase != WLAN_PHASE_CONNECTED) {
        lv_obj_add_flag(g_settings_http_btn, LV_OBJ_FLAG_HIDDEN);
    }

    /* 字体选择 */
    settings_row_create(scr, 172, 50, LV_SYMBOL_IMAGE,
                        "字体选择", settings_font_sel_btn_cb);

    /* 性能测试 (CoreMark) - 从首页移入设置 */
    settings_row_create(scr, 219, 50, LV_SYMBOL_CHARGE,
                        "性能测试 (CoreMark)", coremark_btn_event_handler);

    /* 休眠时钟
     * 进入后每分钟刷新一次时钟并 HIBERNATION，按 PA6 退出回正常模式。 */
    settings_row_create(scr, 266, 50, LV_SYMBOL_BELL,
                        "休眠时钟", settings_sleep_clock_btn_cb);

    /* 测试项目 - 收纳 SD 诊断 + 触摸校准(子页内再选, 避免改主屏按钮高度) */
    settings_row_create(scr, 313, 50, LV_SYMBOL_LIST,
                        "测试项目", settings_test_project_btn_cb);

    /* 关于 - 固件信息 -> 作者信息 */
    settings_row_create(scr, 360, 50, LV_SYMBOL_FILE,
                        "关于", settings_about_btn_cb);

    /* 【EPD优化】恢复刷新，触发一次完整刷新 */
    epd_resume_refresh();
    
    /* 绑定物理返回键: 与设置页返回按钮等效 */
    touch_register_back_btn_callback(settings_physical_back);

    printf("[UI] Settings page created\n");
}

/* 触摸校准行回调(包装 void(void) API 为 lv_event_cb_t) */
static void settings_touch_test_row_cb(lv_event_t *e) {
    (void)e;
    settings_touch_test_open();
}

/* 测试项目子页 返回: 回到首页(与其余设置子页一致) */
static void settings_test_project_back_cb(lv_event_t *e) {
    (void)e;
    main_ui_create();
    epd_mark_refresh_pending();
}

/* 物理返回按键: 测试项目子页与屏幕返回按钮等效 */
static void settings_test_project_physical_back(void)
{
    main_ui_create();
    epd_mark_refresh_pending();
}

/* 测试项目入口: 进入子页, 收纳 SD 诊断 + 触摸校准 */
static void settings_test_project_btn_cb(lv_event_t *e) {
    (void)e;
    printf("[Settings] Opening test project sub-screen\n");
    epd_pause_refresh();
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_add_style(scr, &style_no_anim, LV_STATE_ANY);
    lv_obj_set_style_bg_color(scr, lv_color_make(255, 255, 255), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "测试项目");
    lv_obj_set_style_text_font(title, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(title, lv_color_make(0, 0, 0), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *btn_back = lv_btn_create(scr);
    lv_obj_set_size(btn_back, 60, 30);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 4, 2);
    ui_apply_static_btn_style(btn_back, 2, 4);
    lv_obj_t *bb = lv_label_create(btn_back);
    lv_label_set_text(bb, "返回");
    lv_obj_set_style_text_font(bb, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(bb, lv_color_black(), 0);
    lv_obj_center(bb);
    lv_obj_add_event_cb(btn_back, settings_test_project_back_cb, LV_EVENT_CLICKED, NULL);

    /* 子页内的两个测试项(行高/间距与主屏一致) */
    settings_row_create(scr, 78, 50, LV_SYMBOL_LIST, "SD 卡诊断", settings_sd_diag_btn_cb);
    settings_row_create(scr, 125, 50, LV_SYMBOL_WIFI, "触摸校准", settings_touch_test_row_cb);

    epd_resume_refresh();
    printf("[Settings] Test project sub-screen created\n");

    /* 绑定物理返回键: 与子页返回按钮等效 */
    touch_register_back_btn_callback(settings_test_project_physical_back);
}

/* CoreMark按钮事件处理函数 */
static void coremark_btn_event_handler(lv_event_t * e) {
    printf("[CoreMark] Button clicked, starting benchmark...\n");
    coremark_runner_start();
}

/* 文件管理器按钮事件处理函数 */
static void file_manager_btn_event_handler(lv_event_t * e) {
    // 【EPD优化】先暂停刷新，防止刷出半成品UI
    epd_pause_refresh();
    
    // 【内存优化】如果WiFi还没连上，取消后台连接以释放内存给FM/EPUB阅读器
    if (g_wifi.phase != WLAN_PHASE_CONNECTED) {
        printf("[FM] WiFi not connected, canceling background connect to free memory\n");
        wlan_manager_cancel_connect();
    }
    
    // 挂载SD卡文件系统
    printf("[FM] Mounting SD card...\n");
    fs_ctrl_mount(FS_MNT_DEV_TYPE_SDCARD, 0);
    sd_apply_clock_policy();   /* DCE 探测: 降 SD 时钟, 拉开 DAT 时序余量 */
    
    // 启动文件管理器
    file_manager_init();
    
    // 【EPD优化】FM创建完成后恢复刷新，会自动触发一次完整刷新
    epd_resume_refresh();
}

/* 书架入口 */
static void bookshelf_btn_event_handler(lv_event_t * e) {
    (void)e;
    /* 不再先 epd_pause_refresh(): bookshelf_begin_load 要先让"解析中"遮罩
     * 刷新上屏, 再延迟一拍做同步扫描/解析, 避免解析期间无反馈被当成卡死。 */

    if (g_wifi.phase != WLAN_PHASE_CONNECTED) {
        printf("[BS] WiFi not connected, canceling background connect\n");
        wlan_manager_cancel_connect();
    }

    printf("[BS] Mounting SD card...\n");
    fs_ctrl_mount(FS_MNT_DEV_TYPE_SDCARD, 0);

    bookshelf_begin_load();
}

static lv_obj_t *create_home_tile(lv_obj_t *parent,
                                  lv_coord_t x,
                                  lv_coord_t y,
                                  lv_coord_t w,
                                  lv_coord_t h,
                                  const char *symbol,
                                  const char *title,
                                  lv_event_cb_t event_cb)
{
    lv_obj_t *tile = lv_btn_create(parent);
    lv_obj_set_size(tile, w, h);
    lv_obj_set_pos(tile, x, y);
    lv_obj_add_style(tile, &style_tile, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(tile, &style_tile_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_transition(tile, NULL, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_transition(tile, NULL, LV_PART_SCROLLBAR | LV_STATE_ANY);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    if (event_cb) {
        lv_obj_add_event_cb(tile, event_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *icon = lv_label_create(tile);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(icon, lv_color_make(0, 0, 0), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *label = lv_label_create(tile);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_font(label, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_width(label, w - 12);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -8);

    return tile;
}

static void lvgl_task(void *arg) {
    printf("[LVGL] Task started\r\n");

    static uint32_t s_lvgl_loop_count = 0;
    static uint32_t s_last_diag_tick = 0;

    while (1) {
        /* 安全点互斥: 整个循环体持锁。lv_timer_handler 与 phase 回调
         * (wifi_controller_poll → lv_refr_now) 都会写 framebuffer, 只有
         * 两轮循环之间的空隙是 disp_task 做 EPD 硬件刷新的安全点。
         * 取代原 vTaskSuspend 硬挂起, 避免连接成功瞬间挂起丢失恢复。 */
        epd_lock(OS_WAIT_FOREVER);
        g_lvgl_heartbeat++;

        s_lvgl_loop_count++;

        /* 运行阶段栅栏: 每次进入一个子步骤先置位 g_lvgl_stage, 看门狗在
         * 卡死时打印它, 即可定位 lv_timer_handler 挂住还是 wifi_poll 等。
         * 1=将执行 lv_timer_handler; 2=其已返回; 3=重绘分支完成;
         * 4=wifi_controller_poll 已返回; 5=循环体结束将解锁。 */
        g_lvgl_stage = 1;
        /* 【关键修复】：告诉 LVGL 时间过去了 LVGL_TIMER_PERIOD 毫秒 */
        lv_tick_inc(LVGL_TIMER_PERIOD);

        /* LVGL定时器处理（内部会检查是否该触发 read_cb 了） */
        lv_timer_handler();
        g_lvgl_stage = 2;

        /* 延迟重绘: loading 遮罩在 EPD 刷新窗口内被抑制(epd_refresh_in_progress
         * 挡掉 _lv_disp_refr_timer)时登记了 epd_request_rerender, 这里等刷新
         * 结束后补一次完整渲染。否则遮罩删除后 inv_p 被 epd_do_refresh 清掉,
         * 画面停在 "Preparing fonts..." 永远不上屏。全部在锁内执行, 与
         * disp_task 的 EPD 硬件刷新互斥, 安全。
         * 注意: 必须先确认 EPD 空闲再消费请求, 否则会丢掉待补的重绘。 */
        if (!epd_refresh_in_progress && !epd_refresh_requested
            && epd_consume_rerender_request()) {
            lv_obj_invalidate(lv_scr_act());
            lv_refr_now(NULL);
            /* 若仍被抑制 (如阅读器 g_rendering_in_progress), 重新登记下次再试 */
            lv_disp_t *d = lv_disp_get_default();
            if (d && d->inv_p > 0) {
                epd_request_rerender();
            }
        }
        g_lvgl_stage = 3;

        /* 轮询 wifi controller (扫描/连接/phase 回调) - 确保在LVGL线程中调用 */
        wifi_controller_poll();
        g_lvgl_stage = 4;

        /* NTP 对时结果 / 本地钟点刷新 → 首页时间标签 */
        if (time_sync_take_pending()) {
            home_time_label_apply(1);
        }
        g_lvgl_stage = 5;

        /* 诊断: 每 200 次循环 ~1s 打印一次, 验证 lvgl_task 在 resume 后是否还在跑 */
        if (s_lvgl_loop_count % 200 == 0) {
            uint32_t now = OS_GetTicks();
            if (s_last_diag_tick == 0) s_last_diag_tick = now;
            uint32_t gap = now - s_last_diag_tick;
            s_last_diag_tick = now;
            printf("[LVGL_DIAG] loop=%u gap=%ums\n", s_lvgl_loop_count, gap);
        }

        /* 休眠一小段时间 (先释放互斥锁, 给 disp_task 留 EPD 刷新安全点) */
        epd_unlock();
        OS_MSleep(LVGL_TIMER_PERIOD);
    }
}

/*====================
 * 显示刷新任务
 *===================*/

/* ADC函数前向声明（定义在disp_task之后） */
static void adc_vbat_init(void);
static float adc_read_vbat(void);
static void update_vbat_display(int allow_shutdown);

/* VBAT更新间隔（每5秒更新一次） */
#define VBAT_UPDATE_INTERVAL_MS  5000
#define TIME_TICK_INTERVAL_MS   60000

static void disp_task(void *arg) {
    printf("[Display] Task started\r\n");
    uint32_t vbat_counter = 0;
    uint32_t time_counter = 0;
    /* lvgl_task 看门狗状态 (诊断用) */
    static uint32_t s_last_lvgl_hb = 0;
    static uint32_t s_lvgl_stall_tick = 0;

    /* 初始化ADC并在首次读取VBAT (allow_shutdown=1: disp_task 上下文, 可触发低电量软关机) */
    adc_vbat_init();
    update_vbat_display(1);
    
    while (1) {
        /* PA6 按键检测：低电平=按下，进入休眠
         * XR872 修复: 旧代码调 enter_hibernation() 走自己实现,
         *   调 EPD_DrawStringCentered("Tuwa Reader") 覆盖画面。
         *   正确做法是调 screensaver_task_force_enter() → screensaver_enter(),
         *   内部会先加载 screensaver.bin (有图) 或显示 Tuwa Reader (无图),
         *   再断 WiFi + EPD deep sleep + PA6 唤醒源。
         *
         * 时钟模式退出保护(GPT 方案 §三)：PA6 唤醒退出时钟模式后，本检测会立刻
         * 看到 PA6 仍可能处于按下态而再次进休眠。boot_key_guard 由
         * clock_boot_dispatch() 在退出时置位，这里消费它以忽略首次按下。 */
        if (HAL_GPIO_ReadPin(GPIO_PORT_A, PA6_BUTTON_PIN) == 0) {
            if (clock_consume_boot_key_guard()) {
                printf("[PA6] boot_key_guard: swallowed post-clock-mode press\r\n");
                /* 等按键彻底释放后再继续，避免持续触发 */
                while (HAL_GPIO_ReadPin(GPIO_PORT_A, PA6_BUTTON_PIN) == 0) {
                    OS_MSleep(20);
                }
            } else {
                printf("[PA6] Button pressed");
                OS_MSleep(50);
                while (HAL_GPIO_ReadPin(GPIO_PORT_A, PA6_BUTTON_PIN) == 0) {
                    OS_MSleep(20);
                }
                /* 充电模式: 插着电按 PA6 → 先显示休眠遮罩反馈(推屏),
                 * 稍作停留让用户看到, 再进充电界面. */
                if (HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_21) == 0) {
                    printf(", charger connected -> charge mode\r\n");
                    screensaver_show_sleep_overlay();
                    OS_MSleep(600);  /* 让用户看清反馈遮罩 */
                    charge_mode_enter();  /* 不返回 */
                }
                printf(", entering hibernation (via screensaver)\r\n");
                loading_show("Going to sleep...");
                screensaver_task_force_enter();
            }
        }

        screensaver_task();

        /* 休眠时钟模式：lvgl 按钮回调只设标志，真正进入在此(disp_task 上下文)
         * 执行。pm_enter_mode(HIBERNATION) 必须在 disp_task 调用，与 screensaver
         * 一致；在 lvgl 上下文调用会触发 PM 框架 UsageFault。enter_run 不返回。 */
        clock_mode_enter_run();

        /* 处理待刷新的显示 - 检查状态 */
        lv_port_disp_task();
        
        /* 执行EPD刷新 - 可能阻塞本线程，但不影响lvgl */
        epd_do_refresh();

        /* 看门狗: lvgl_task 心跳 3s 未推进且不在 EPD 刷新中 → 告警。
         * 仅日志诊断, 不做自动恢复(互斥锁安全点已消除挂起丢失类卡死)。 */
        {
            uint32_t hb_now = epd_get_tick();
            if (g_lvgl_heartbeat != s_last_lvgl_hb) {
                s_last_lvgl_hb = g_lvgl_heartbeat;
                s_lvgl_stall_tick = hb_now;
            } else if (!epd_refresh_in_progress && (hb_now - s_lvgl_stall_tick) > 3000) {
                printf("[WATCHDOG] lvgl_task stalled %ums (hb=%lu stage=%u epd=%u/%u)\n",
                       (unsigned)(hb_now - s_lvgl_stall_tick),
                       (unsigned long)g_lvgl_heartbeat,
                       (unsigned)g_lvgl_stage,
                       (unsigned)epd_refresh_in_progress,
                       (unsigned)epd_refresh_requested);
                s_lvgl_stall_tick = hb_now;  /* 每 3s 报一次, 不刷屏 */
            }
        }

        /* 定期更新VBAT显示 (disp_task 上下文, 可触发低电量软关机)。
         * ADC 读取失败时 update_vbat_display 置 g_vbat_retry_countdown，
         * 这里倒数归零后立即触发一次重读（尽快出真实电量，不拖到下个周期） */
        vbat_counter += DISP_TASK_PERIOD;
        if (g_vbat_retry_countdown > 0) {
            if (--g_vbat_retry_countdown == 0) {
                vbat_counter = VBAT_UPDATE_INTERVAL_MS;
            }
        }
        if (vbat_counter >= VBAT_UPDATE_INTERVAL_MS) {
            vbat_counter = 0;
            update_vbat_display(1);
        }

        /* 每分钟刷新首页时钟文本（EPD 仅在分钟变化时刷） */
        time_counter += DISP_TASK_PERIOD;
        if (time_counter >= TIME_TICK_INTERVAL_MS) {
            time_counter = 0;
            (void)time_sync_refresh_local();
        }
        
        /* 休眠 */
        OS_MSleep(DISP_TASK_PERIOD);
    }
}

/*====================
 * 主界面创建函数（可被 CoreMark 返回按钮调用重建）
 *===================*/
void main_ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();

    /* 回到主页: 清掉物理返回回调, 主页无返回按钮 */
    touch_register_back_btn_callback(NULL);

    /* 【关键】清屏前先显式关闭 loading 遮罩。
     * 开机 loading_show("Booting...") 创建的遮罩是活动屏的子对象，若直接
     * lv_obj_clean(scr) 会把它连带删除，而 loading.c 的 s_bg/s_box/s_label
     * 静态指针仍非 NULL —— 之后 font_warm 的 loading_show() 会先对已释放的
     * 对象调 lv_obj_del()，造成 use-after-free / TLSF 双重释放，堆被破坏，
     * 表现为开机约 30 秒后整机卡死。此处先 loading_hide() 可把指针置空。 */
    loading_hide();

    /* 清屏 */
    lv_obj_clean(scr);
    g_wifi_icon_label = NULL;
    g_time_label = NULL;
    g_vbat_label = NULL;

    /* 初始化全局样式（仅首次） */
    static int styles_inited = 0;
    if (!styles_inited) {
        lv_style_init(&style_no_anim);
        lv_style_set_anim_time(&style_no_anim, 0);
        lv_style_set_border_width(&style_no_anim, 0);

        lv_style_init(&style_sw);
        lv_style_set_border_width(&style_sw, 2);
        lv_style_set_border_color(&style_sw, lv_color_make(0, 0, 0));
        lv_style_set_radius(&style_sw, LV_RADIUS_CIRCLE);

        lv_style_init(&style_tile);
        lv_style_set_radius(&style_tile, 8);
        lv_style_set_bg_color(&style_tile, lv_color_make(255, 255, 255));
        lv_style_set_bg_opa(&style_tile, LV_OPA_COVER);
        lv_style_set_border_width(&style_tile, 2);
        lv_style_set_border_color(&style_tile, lv_color_make(0, 0, 0));
        lv_style_set_shadow_width(&style_tile, 0);
        lv_style_set_outline_width(&style_tile, 0);
        lv_style_set_pad_all(&style_tile, 0);

        lv_style_init(&style_tile_pressed);
        lv_style_set_radius(&style_tile_pressed, 8);
        lv_style_set_bg_color(&style_tile_pressed, lv_color_make(230, 230, 230));
        lv_style_set_bg_opa(&style_tile_pressed, LV_OPA_COVER);
        lv_style_set_border_width(&style_tile_pressed, 2);
        lv_style_set_border_color(&style_tile_pressed, lv_color_make(0, 0, 0));
        lv_style_set_shadow_width(&style_tile_pressed, 0);
        lv_style_set_outline_width(&style_tile_pressed, 0);

        styles_inited = 1;
    }

    lv_obj_add_style(scr, &style_no_anim, LV_STATE_ANY);
    lv_obj_set_style_bg_color(scr, lv_color_make(255, 255, 255), 0);

    /* 标题 - 左侧; 时间紧跟标题右侧; 右侧 vbat + WiFi (不与时间重叠) */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "首页");
    lv_obj_set_style_text_font(title, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(title, lv_color_make(0, 0, 0), 0);
    lv_obj_set_size(title, 48, 30);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 6, 8);

    /* 顶端时间 HH:MM（WiFi 连通后国家授时中心同步）
     * 放标题右侧, 与右侧 WiFi/vbat 分开, 避免 240px 宽下重合 */
    g_time_label = lv_label_create(scr);
    lv_obj_set_style_text_font(g_time_label, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(g_time_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_width(g_time_label, 60);
    lv_obj_align(g_time_label, LV_ALIGN_TOP_LEFT, 56, 8);
    home_time_label_apply(0);

    /* WiFi 图标 - 12px 小版, 紧贴 vbat 左侧, 仅 CONNECTED 时显示 */
    g_wifi_icon_label = lv_label_create(scr);
    lv_obj_set_style_text_font(g_wifi_icon_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_wifi_icon_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_size(g_wifi_icon_label, 16, 15);
    lv_obj_align(g_wifi_icon_label, LV_ALIGN_TOP_RIGHT, -82, 8);
    /* early WiFi 可能已 CONNECTED：创建时同步，避免首帧无图标 */
    home_wifi_icon_sync(0);

    /* 第一行: 文件和设置 */
    create_home_tile(scr, 10, 78, 106, 88,
                     LV_SYMBOL_DIRECTORY,
                     "文件",
                     file_manager_btn_event_handler);

    create_home_tile(scr, 124, 78, 106, 88,
                     LV_SYMBOL_SETTINGS,
                     "设置",
                     settings_btn_event_handler);

    /* 第二行: 书架 (CoreMark 已移到设置页) */
    create_home_tile(scr, 10, 174, 220, 72,
                     LV_SYMBOL_LIST,
                     "书架",
                     bookshelf_btn_event_handler);

    /* 第三行: 休眠时钟 (fix-power-saving: 从 Settings 提到首页) */
    create_home_tile(scr, 10, 254, 220, 72,
                     LV_SYMBOL_BELL,
                     "休眠时钟",
                     home_clock_btn_event_handler);

    /* VBAT SOC%标签 - 右上角 */
    g_vbat_label = lv_label_create(scr);
    lv_label_set_text(g_vbat_label, g_vbat_text[0] ? g_vbat_text : "");
    lv_obj_set_style_text_font(g_vbat_label, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(g_vbat_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_size(g_vbat_label, 70, 20);
    lv_obj_align(g_vbat_label, LV_ALIGN_TOP_RIGHT, -6, 8);

    /* 立即初始化 ADC 并读一次电量显示，避免开机后空等 5s 周期才有电量。
     * allow_shutdown=0: main_ui_create 可能运行在 main 线程 / LVGL 线程
     * (settings 返回 / 屏保退出), 这里不能触发休眠 (pm_enter_mode 只能在
     * disp_task 上下文调用), 低电量软关机由 disp_task 的周期调用负责。 */
    adc_vbat_init();
    update_vbat_display(0);

    printf("[UI] Main UI created\n");

    /* 主界面创建完成后，请求刷新EPD */
    epd_mark_refresh_pending();
}

/*====================
 * ADC VBAT 读取
 *===================*/
static void adc_vbat_init(void)
{
    if (g_adc_inited) return;
    ADC_InitParam param;
    memset(&param, 0, sizeof(param));
    param.delay = 0;
    param.freq = 1000;
    param.mode = ADC_CONTI_CONV;
    param.vref_mode = ADC_VREF_MODE_1;
    HAL_ADC_Init(&param);
    g_adc_inited = 1;
    printf("[ADC] VBAT ADC initialized\n");
}

static float adc_read_vbat(void)
{
    uint32_t raw = 0;
    if (!g_adc_inited) return 0.0f;
    if (HAL_ADC_Conv_Polling(ADC_CHANNEL_VBAT, &raw, 100) != 0) {
        return 0.0f;
    }
    /* 12-bit ADC, 2.5V reference, VBAT channel ratio=3
     * voltage_mv = raw * 2500 * 3 / 4096
     */
    return (float)raw * 2.5f * 3.0f / 4096.0f;
}

/* XR872 修复: enter_hibernation() 已删除, PA6 休眠改走
 *   screensaver_task_force_enter() → screensaver_enter(force=1),
 *   内部会先加载 screensaver.bin 显示, 再断 WiFi + EPD deep sleep + PA6 唤醒源。
 *   之前 enter_hibernation 自己画 "Tuwa Reader" 覆盖了屏保图。 */

/* batt_status.md: 电压 -> SOC 查表 (12 段, 单调, 不插值)
 * v < kSocLut[0].voltage -> 0
 * v >= kSocLut[last].voltage -> 100
 * 否则返回最大 i 使 kSocLut[i].voltage <= v 的 SOC */
typedef struct { float voltage; int soc; } soc_lut_t;
static const soc_lut_t kSocLut[] = {
    {2.70f,   0},   /* 截止 */
    {3.00f,   0},
    {3.50f,  10},
    {3.70f,  20},
    {3.76f,  30},
    {3.78f,  40},
    {3.81f,  50},
    {3.85f,  60},
    {3.90f,  70},
    {3.95f,  80},
    {4.00f,  90},
    {4.10f,  90},
    {4.20f, 100},
};
#define SOC_LUT_LEN (sizeof(kSocLut)/sizeof(kSocLut[0]))

static int voltage_to_soc(float v)
{
    if (v <= kSocLut[0].voltage) return 0;
    if (v >= kSocLut[SOC_LUT_LEN-1].voltage) return kSocLut[SOC_LUT_LEN-1].soc;
    int soc = 0;
    for (int i = (int)SOC_LUT_LEN - 1; i >= 0; i--) {
        if (v >= kSocLut[i].voltage) {
            soc = kSocLut[i].soc;
            break;
        }
    }
    return soc;
}

static void update_vbat_display(int allow_shutdown)
{
    static int last_soc = -1;
    static int last_charging = -1;

    float vbat = adc_read_vbat();
    if (vbat <= 0.0f) {
        /* 读取失败（ADC 未就绪/超时，raw=0）：绝不显示 0%，保留上次显示值，
         * 也不触发低电量关机；置快速重试（~200ms 后再读） */
        printf("[VBAT] read failed (0.00V), keep last display, retry soon\n");
        g_vbat_retry_countdown = 10;
        return;
    }
    int pa21 = HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_21);
    int charging = (pa21 == 0);
    int soc = voltage_to_soc(vbat);

    printf("[VBAT] raw=%.2fV soc=%d%% PA21=%d charging=%d\n", vbat, soc, pa21, charging);

    /* 低电量软关机 (fix-power-saving):
     *   未充电且电量见底 (soc==0 或 vbat<3.0V) → 直接走 screensaver 进
     *   HIBERNATION。soc==0 在 kSocLut 下对应 vbat<3.50V (3.50V 起才是 10%),
     *   即 ~5% 电量就软关机, 避免低电量时反复开机 + 每次开机 EPD 全刷的
     *   掉电复位死循环 (见 EPD_3IN52_Init 内嵌 Clear)。充电中不触发
     *   (插电走充电模式/充电唤醒)。vbat>1.0V 的 sanity 检查防止 ADC 读数
     *   失败(0V)时误判为低电量。allow_shutdown 只由 disp_task 传 1:
     *   pm_enter_mode(HIBERNATION) 必须在 disp_task 上下文调用 (与 PA6 休眠
     *   一致), LVGL 线程调用会触发 PM 框架 UsageFault。 */
    if (allow_shutdown && !charging && vbat > 1.0f && (soc <= 0 || vbat < 3.0f)) {
        printf("[LOWBATT] soft shutdown: soc=%d%% vbat=%.2fV charging=%d\r\n",
               soc, vbat, charging);
        fflush(stdout);
        screensaver_task_force_enter();
        /* 正常情况不返回 (进 HIBERNATION / CPUReset); 若被 HTTP 运行中拒绝
         * 则打印提示后继续, 下次周期再试。 */
        printf("[LOWBATT] screensaver refused, retry next cycle\r\n");
    }

    /* 变化才更新 UI - 避免每 5s 触发 EPD 刷新 */
    if (soc == last_soc && charging == last_charging) return;
    last_soc = soc;
    last_charging = charging;

    if (charging) {
        snprintf(g_vbat_text, sizeof(g_vbat_text), "%d%% +", soc);
    } else {
        snprintf(g_vbat_text, sizeof(g_vbat_text), "%d%%", soc);
    }
    if (g_vbat_label) {
        lv_label_set_text(g_vbat_label, g_vbat_text);
    }
}

/*====================
 * 主函数
 *===================*/

int main(void)
{
    GPIO_InitParam param;
    
    printf("======================================\r\n");
    printf("LVGL E-Paper 3.52 inch Demo\r\n");
    printf("EPD_3IN52 + CHSC6540\r\n");
    printf("======================================\r\n\r\n");
    
    /*====================================================
     * 省电优化: WKTIMER 分钟唤醒走最小化初始化路径
     *   正常时钟模式每分钟冷启动本来要跑完整 platform_init (含 WiFi/SD),
     *   但分钟周期只用 EPD(PSRAM)+RTC+pm_enter_mode, 完全不需要 WiFi/SD。
     *   这里在 platform_init 之前早判 (HAL_Wakeup_GetEvent 在 SystemInit→pm_init
     *   里已填充, main 第一行即可用), 若是 WKTIMER 唤醒则只跑 level0
     *   (flash/image/PSRAM/cache), 跳过 level1(WiFi) 和 level2(SD/audio),
     *   直接进 clock_minute_cycle 渲染时钟。
     *==================================================*/
    if (HAL_Wakeup_GetEvent() & PM_WAKEUP_SRC_WKTIMER) {
        /* WKTIMER 只被时钟模式使用 (RTC weekday==7 → 分钟刷新) */
        printf("[BOOT] WKTIMER wake -> minimal init (skip WiFi/SD)\r\n");
        platform_init_level0();          /* 仅 flash/image/PSRAM/cache */
        EPD_GPIO_Init_Public();          /* EPD GPIO 最小初始化 */
        clock_minute_cycle();            /* 不返回: 渲染→刷新→配置唤醒→HIBERNATION */
        /* 不会到达 */
    }

    /* PA21(WKIO7) 充电唤醒早判: 休眠时插入充电器 → PA21 下降沿唤醒。
     * 直接进充电模式(最小初始化, 不启动 WiFi/SD/LVGL), MCU 活跃轮询。
     * charge_mode_run 在 PA6 按下时返回, 继续正常启动。 */
    if ((HAL_Wakeup_GetEvent() & PM_WAKEUP_SRC_WKIO7)
        && !clock_mode_enabled()
        && (HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_21) == 0)) {
        printf("[BOOT] PA21(WKIO7) wake + charging -> charge mode\r\n");
        platform_init_level0();
        EPD_GPIO_Init_Public();
        charge_mode_set_enabled(true);
        charge_mode_run_minimal();   /* 显示充电画面 + 轮询 PA21/PA6 */
        /* PA6 按下 → return, 继续正常启动 */
        printf("[BOOT] PA6 pressed in charge mode -> normal boot\r\n");
    }

    /* 正常启动: 先 level0 + 显示开机画面, 再 level1(WiFi) + level2(SD).
     * 这样开机画面在最早期就出现, WiFi/SD 初始化期间用户看到的是 boot 画面而非黑屏. */
    platform_init_level0();          /* flash/image/PSRAM/cache */
    EPD_GPIO_Init_Public();          /* EPD GPIO 最小初始化 */
    boot_screen_show();              /* 推开机画面到墨水屏 (在 WiFi 前) */
    platform_init_level1();          /* WiFi/网络子系统 */
    platform_init_level2();          /* SD/音频等 */

    {
        extern void epub_buffer_init(void);
        epub_buffer_init();
    }

    /* 检测唤醒来源 */
    {
        uint32_t wakeup_event = HAL_Wakeup_GetEvent();
        printf("[PM] wakeup_event = 0x%08x\n", wakeup_event);
        if (wakeup_event & PM_WAKEUP_SRC_WKIO2) {
            printf("[PM] Woken by PA06 button\n");
        }
    }

    /* 配置PA6为GPIO输入+上拉（屏保按键） */
    printf("[PA6] Configuring PA6 as input with pull-up (screensaver button)...\r\n");
    param.mode = GPIOx_Pn_F0_INPUT;
    param.driving = GPIO_DRIVING_LEVEL_1;
    param.pull = GPIO_PULL_UP;
    HAL_GPIO_Init(GPIO_PORT_A, PA6_BUTTON_PIN, &param);
    printf("[PA6] PA6 configured\r\n");

    /* 配置PA21为GPIO输入（充电检测） */
    printf("[PA21] Configuring PA21 as input (charge detect)...\r\n");
    param.mode = GPIOx_Pn_F0_INPUT;
    param.driving = GPIO_DRIVING_LEVEL_1;
    param.pull = GPIO_PULL_UP;
    HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_21, &param);
    printf("[PA21] PA21 configured\r\n");

    /* 先配置EXT LDO到3.3V，为SD卡供电 */
    printf("[EXT LDO] Setting EXT LDO to 3.3V...\r\n");
    HAL_PRCM_SelectEXTLDOVolt(PRCM_EXT_LDO_3V3);
    HAL_PRCM_SetEXTLDOMode(PRCM_EXTLDO_ALWAYS_ON);
    printf("[EXT LDO] EXT LDO configured to 3.3V\r\n");
    
    
    
    /* Configure TOP LDO to 1.8V (default) */
    printf("[TOP LDO] Setting TOP LDO voltage to 1.8V...\r\n");
    HAL_PRCM_SetTOPLDOVoltage(PRCM_TOPLDO_VOLT_1V8_DEFAULT);
    printf("[TOP LDO] TOP LDO configured to 1.8V\r\n");
    
    /* Configure PA23 as output low (grounded) */
    printf("[PA23] Configuring PA23 to low level...\r\n");
    param.mode = GPIOx_Pn_F1_OUTPUT;     // 通用输出模式
    param.driving = GPIO_DRIVING_LEVEL_1;
    param.pull = GPIO_PULL_NONE;
    HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_23, &param);
    HAL_GPIO_WritePin(GPIO_PORT_A, GPIO_PIN_23, GPIO_PIN_LOW);  // 设置为低电平
    printf("[PA23] PA23 configured as LOW\r\n");
    
    /* Configure PA07 as output low (CS) */
    printf("[PA07] Configuring PA07 to low level...\r\n");
    param.mode = GPIOx_Pn_F1_OUTPUT;
    param.driving = GPIO_DRIVING_LEVEL_1;
    param.pull = GPIO_PULL_NONE;
    HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_7, &param);
    HAL_GPIO_WritePin(GPIO_PORT_A, GPIO_PIN_7, GPIO_PIN_LOW);
    printf("[PA07] PA07 configured as LOW\r\n");
    
    // 验证GPIO初始化 - 读取并打印GPIO状态
    printf("\r\n=== GPIO Status Check ===\r\n");
    printf("PA0 (MOSI) = %d\r\n", HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_0));
    printf("PA1 (DC)   = %d\r\n", HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_1));
    printf("PA2 (CLK)  = %d\r\n", HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_2));
    printf("PA3 (CS)   = %d\r\n", HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_3));
    printf("PA5 (INT)  = %d\r\n", HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_5));
    printf("PA7 (CS2)  = %d\r\n", HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_7));
    printf("PA8 (BUSY) = %d\r\n", HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_8));
    printf("PA9 (RST)  = %d\r\n", HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_9));
    printf("PA23       = %d\r\n", HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_23));
    printf("============================\r\n\r\n");
    /* XR872 修复: 原 3s 等待过大, 改为 0.1s。
     * EXT LDO 硬件稳定时间 <1ms, 只需给电容充电几 ms 即可。 */
    printf("[SD] Waiting 100ms for EXT LDO to stabilize...\r\n");
    OS_MSleep(100);

    /*====================================================
     * 休眠时钟启动分流 (GPT clock 方案 §二)
     * 必须在 WiFi/SD/LVGL/触摸等普通服务启动之前。
     *   - WKTIMER 唤醒 -> 分钟周期(最小化)，不返回
     *   - PA6 唤醒且处于时钟模式 -> 清标志 + boot guard，继续正常启动
     *   - 其它 -> 正常 InkFrog 启动
     *==================================================*/
    clock_boot_dispatch();

    /* 充电模式 PA6 唤醒 → 清标志, 正常启动 (可看书/设置)。
     * (PA21 早判已在 platform_init 之前处理, 这里只处理从充电模式 PA6 退出) */
    if (charge_mode_enabled()) {
        printf("[BOOT] PA6 wake from charge mode -> normal boot\r\n");
        charge_mode_set_enabled(false);
    }

    /* 执行FatFs文件系统测试（FatFs内部会初始化SD卡） */
    fatfs_filesystem_test();

    /*====================
     * 网络尽早启动（对齐原版：与 EPD/UI 并行，争取 ~4.6s 拿到 IP）
     *===================*/
    printf("\r\n");
    printf("======================================\r\n");
    printf("Network Initialization (early)\r\n");
    printf("======================================\r\n\r\n");

    printf("[NET] Initializing WLAN...\r\n");
    if (wlan_manager_init() != 0) {
        printf("[NET ERROR] WLAN init failed!\r\n");
    } else {
        printf("[NET] WLAN initialized\r\n");
    }

    settings_screen_init();
    http_server_init(HTTP_SERVER_PORT);
    if (http_server_reserve_thread() != 0) {
        printf("[HTTP] WARNING: failed to reserve worker thread at boot\r\n");
    }
    wifi_controller_init();
    wifi_controller_register_cb(on_wifi_phase_change, NULL);
    wifi_controller_start();
    printf("[INFO] WiFi connecting early (parallel with EPD/UI)...\r\n\r\n");

    /* 初始化LVGL - lv_port_disp_init()会初始化EPD（与 WiFi 并行） */
    printf("[MAIN] Step 1: Calling lv_init()...\r\n");
    printf("[MAIN] Calling lv_init()...\r\n");
    fflush(stdout);
    lv_init();
    printf("[MAIN] lv_init() returned\r\n");
    printf("[MAIN] Step 1: lv_init() completed\r\n");

    /* SD 卡已在 fatfs_filesystem_test() 挂载, lv_fs(FATFS) 在 lv_init() 内已就绪。
     * 字体 warmup 在此同步执行: 此时 disp/lvgl 任务还没创建(无并发 LVGL, 线程安全),
     * 首页也尚未构建/刷屏, EPD 上仍是 boot 开机画面 —— 预加载的 ~2s 耗时被这层
     * 开机画面完全遮罩, 因此不会再弹 "Preparing fonts..." 遮罩。
     * 注: 不能用 LVGL 定时器, 定时器要等 lvgl_task 启动后才触发, 那时首页已刷上屏。 */
    font_warm_run_boot(NULL);

    /* 初始化显示端口 */
    printf("[MAIN] Step 2: Calling lv_port_disp_init()...\r\n");
    lv_port_disp_init();
    printf("[MAIN] Step 2: lv_port_disp_init() completed\r\n");

    /* 开机提示: lv_port_disp_init 后 EPD 已初始化(白屏)。
     * 后续触摸初始化 + 首页构建要几百 ms, 先显示提示让用户知道在开机。 */
    loading_show("Booting...");

    /* 初始化输入端口 (触摸) */
    printf("[MAIN] Step 3: Calling lv_port_indev_init()...\r\n");
    lv_port_indev_init();
    printf("[MAIN] Step 3: lv_port_indev_init() completed\r\n");

    /* 创建主界面UI */
    main_ui_create();
    screensaver_init();

    /* SD 数据错误 (DCE 后卡死) 恢复服务: 定时器在 LVGL 上下文内轮询,
     * http 上传遇到 f_write FR_DISK_ERR 时请求整卡重建并重试。 */
    sd_recovery_init();

    printf("\r\n[OK] LVGL system initialized!\r\n");
    printf("[INFO] Controls: Files and Settings tiles\r\n");

    /* UI创建完毕，启动后台任务 */
    printf("[Display] create disp_task stack=%d lvgl stack=%d\r\n",
           DISP_TASK_STACK_SIZE, LVGL_TASK_STACK_SIZE);
    print_heap_info();
    psram_heap_info();
    dma_heap_info();
    if (OS_ThreadCreate(&disp_task_thread, "disp_task", disp_task, NULL,
                        OS_PRIORITY_NORMAL, DISP_TASK_STACK_SIZE) != 0) {
        printf("[ERROR] Failed to create disp_task\r\n");
    }
    print_heap_info();
    if (OS_ThreadCreate(&lvgl_thread, "lvgl_task", lvgl_task, NULL,
                        OS_PRIORITY_NORMAL, LVGL_TASK_STACK_SIZE) != 0) {
        printf("[ERROR] Failed to create lvgl_task\r\n");
    }
    print_heap_info();

    epd_resume_refresh();
    printf("[EPD] Refresh resumed after init\n");

    /* 主线程完成，LVGL任务在后台运行 */
    while (1) {
        OS_MSleep(1000);
    }
    
    return 0;
}
