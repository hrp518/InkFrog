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
#include "screensaver.h"
#include "coremark/coremark_runner.h"

extern const lv_font_t lv_font_montserrat_12;

/* SD卡测试函数声明 - 来自cmd_sd.c */
extern int32_t mmc_test_init(uint32_t host_id, void *sdc_param, uint32_t scan);
extern int32_t mmc_test_exit(uint16_t sd_id, uint16_t host_id);
extern struct mmc_card *mmc_scan_init(uint16_t sd_id, uint16_t sdc_id, void *card_param);

/* LVGL定时器周期 (ms) */
#define LVGL_TIMER_PERIOD    5

/* 显示刷新任务周期 (ms) - 优化二：提高响应速度 */
#define DISP_TASK_PERIOD     20

/* LVGL线程句柄 - 用于刷新时挂起LVGL任务防止SPI冲突 */
OS_Thread_t lvgl_thread;
static OS_Thread_t disp_task_thread;

/* HTTP对话框和SW1全局引用 */
static lv_obj_t *g_http_dialog = NULL;
static lv_obj_t *g_http_dialog_bg = NULL;
static lv_obj_t *g_net_label = NULL;

/* WiFi/HTTP 状态管理 */
static uint8_t g_wifi_connected = 0;
static uint8_t g_http_running = 0;
static char g_ip_text[32] = "WiFi: OFF";
static lv_obj_t *g_wifi_icon = NULL;  /* WiFi图标，与Home标题对齐 */
static OS_Thread_t wifi_connect_thread;  /* WiFi连接后台任务线程 */

/* Settings 页面引用 */
static lv_obj_t *g_settings_scr = NULL;
static lv_obj_t *g_settings_wifi_sw = NULL;
static lv_obj_t *g_settings_http_btn = NULL;

/* 全局样式 - 必须在文件作用域，因为 main_ui_create() 可被多次调用 */
static lv_style_t style_no_anim;
static lv_style_t style_sw;
static lv_style_t style_tile;
static lv_style_t style_tile_pressed;

/* platform_init声明 */
extern void platform_init(void);

/* 前向声明 - 解决main_ui_create在定义前被调用的问题 */
void main_ui_create(void);

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
    
    /* 初始化 - 将根目录压入栈 */
    strcpy(path_stack[0], path);
    stack_top = 1;
    
    while (stack_top > 0) {
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
                /* 是目录 - 压入栈中待后续处理 */
                if (stack_top < MAX_PATH_STACK - 1) {
                    sprintf(path_stack[stack_top], "%s/%s", path, fno.fname);
                    stack_top++;
                } else {
                    printf("[FatFs WRN] Path stack full, skip dir: %s/%s\r\n", path, fno.fname);
                }
            } else {
                /* 是文件 - 直接输出 */
                printf("  [FILE] %s/%s (size: %u bytes)\r\n", path, fno.fname, (unsigned int)fno.fsize);
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
        printf("[ERROR] Mount failed!\r\n");
        return;
    }
    printf("[OK] FatFs mounted successfully\r\n\r\n");
    
    /* 2. 获取容量信息 */
    printf("[2/3] Get Card Capacity Info...\r\n");
    if (fatfs_get_card_info(&free_cap, &total_cap) == 0) {
        printf("  Total: %u MB, Free: %u MB\r\n", total_cap, free_cap);
    }
    printf("\r\n");
    
    /* 3. 创建Font目录（如果不存在） */
    printf("[3/4] Creating /Font directory...\r\n");
    res = f_mkdir("/Font");
    if (res == FR_OK || res == FR_EXIST) {
        printf("[OK] /Font directory ready\r\n");
    } else {
        printf("[WRN] Failed to create /Font, res=%d\r\n", res);
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

/* HTTP对话框停止按钮回调 */
static void http_dialog_stop_cb(lv_event_t *e) {
    printf("[HTTP_DIALOG] Stop button clicked\n");
    http_server_stop();
    g_http_running = 0;
    
    /* 关闭对话框 */
    if (g_http_dialog_bg) {
        lv_obj_del(g_http_dialog_bg);
        g_http_dialog_bg = NULL;
        g_http_dialog = NULL;
    }
    
    epd_mark_refresh_pending();
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
    
    /* 对话框容器 */
    g_http_dialog = lv_obj_create(g_http_dialog_bg);
    lv_obj_set_size(g_http_dialog, 200, 150);
    lv_obj_align(g_http_dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(g_http_dialog, lv_color_make(255, 255, 255), 0);
    lv_obj_set_style_border_width(g_http_dialog, 2, 0);
    lv_obj_set_style_border_color(g_http_dialog, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_radius(g_http_dialog, 8, 0);
    lv_obj_clear_flag(g_http_dialog, LV_OBJ_FLAG_SCROLLABLE);
    
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
    
    /* 停止按钮 */
    lv_obj_t *btn_stop = lv_btn_create(g_http_dialog);
    lv_obj_set_size(btn_stop, 80, 35);
    lv_obj_align(btn_stop, LV_ALIGN_BOTTOM_MID, 0, -10);
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

/* Settings WiFi开关事件处理 */
static void settings_wifi_switch_event_handler(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * sw = lv_event_get_target(e);

    if(code == LV_EVENT_VALUE_CHANGED) {
        bool is_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
        printf("[Settings] WiFi Switch Toggled! State: %s\n", is_on ? "ON" : "OFF");
        
        if (is_on) {
            /* 启动WiFi */
            printf("[Settings] Connecting WiFi...\n");
            int ret = wlan_manager_connect(WIFI_SSID, WIFI_PASSWD);
            if (ret == 0) {
                if (wlan_manager_wait_for_ip(30000) == 0) {
                    WLAN_IPInfo_t ip_info;
                    wlan_manager_get_ip_info(&ip_info);
                    printf("[Settings] WiFi connected! IP: %s\n", ip_info.ip);
                    
                    g_wifi_connected = 1;
                    snprintf(g_ip_text, sizeof(g_ip_text), "IP: %s", ip_info.ip);
                    
                    /* 更新首页标签 */
                    if (g_net_label) {
                        lv_label_set_text(g_net_label, g_ip_text);
                    }
                    
                    /* 如果在 Settings 页面，显示 HTTP 按钮 */
                    if (g_settings_scr && g_settings_http_btn) {
                        lv_obj_clear_flag(g_settings_http_btn, LV_OBJ_FLAG_HIDDEN);
                    }
                } else {
                    printf("[Settings] WiFi connect timeout!\n");
                    lv_obj_clear_state(sw, LV_STATE_CHECKED);
                    g_wifi_connected = 0;
                }
            } else {
                printf("[Settings] WiFi connect failed!\n");
                lv_obj_clear_state(sw, LV_STATE_CHECKED);
                g_wifi_connected = 0;
            }
        } else {
            /* 关闭WiFi */
            printf("[Settings] Disconnecting WiFi...\n");
            /* 先停止HTTP */
            if (g_http_running) {
                http_server_stop();
                g_http_running = 0;
                if (g_http_dialog_bg) {
                    lv_obj_del(g_http_dialog_bg);
                    g_http_dialog_bg = NULL;
                    g_http_dialog = NULL;
                }
            }
            wlan_manager_disconnect();
            g_wifi_connected = 0;
            snprintf(g_ip_text, sizeof(g_ip_text), "WiFi: OFF");
            
            /* 更新首页标签 */
            if (g_net_label) {
                lv_label_set_text(g_net_label, g_ip_text);
            }
            
            /* 如果在 Settings 页面，隐藏 HTTP 按钮 */
            if (g_settings_scr && g_settings_http_btn) {
                lv_obj_add_flag(g_settings_http_btn, LV_OBJ_FLAG_HIDDEN);
            }
        }
        epd_mark_refresh_pending();
    }
}

/* Settings HTTP Server按钮事件处理 */
static void settings_http_btn_event_handler(lv_event_t * e) {
    printf("[Settings] HTTP Server button clicked\n");
    
    if (!g_wifi_connected) {
        printf("[Settings] WiFi not connected, cannot start HTTP\n");
        return;
    }
    
    if (g_http_running) {
        printf("[Settings] HTTP already running\n");
        return;
    }
    
    /* 启动HTTP服务器（阻塞） */
    http_server_init(HTTP_SERVER_PORT);
    if (http_server_start() == 0) {
        printf("[Settings] HTTP server started!\n");
        g_http_running = 1;
        
        /* 提取 IP 地址显示 */
        char ip_str[32];
        WLAN_IPInfo_t ip_info;
        wlan_manager_get_ip_info(&ip_info);
        snprintf(ip_str, sizeof(ip_str), "%s", ip_info.ip);
        
        /* 弹出对话框 */
        http_dialog_create(ip_str);
    } else {
        printf("[Settings] HTTP start failed!\n");
    }
}

/* Settings 返回按钮事件处理 */
static void settings_back_btn_event_handler(lv_event_t * e) {
    printf("[Settings] Back to home\n");
    
    /* 使用 lv_obj_clean 清空内容，而不是 lv_obj_del 删除屏幕
     * 避免删除当前活动屏幕后 lv_scr_act() 返回无效指针
     */
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    
    /* 清理 Settings 相关指针 */
    g_settings_scr = NULL;
    g_settings_wifi_sw = NULL;
    g_settings_http_btn = NULL;
    
    /* 重建首页 */
    main_ui_create();
    
    epd_mark_refresh_pending();
}

/* Settings 入口按钮事件处理 */
static void settings_btn_event_handler(lv_event_t * e) {
    printf("[Settings] Entering Settings page\n");
    
    /* 【EPD优化】先暂停刷新，防止刷出半成品UI */
    epd_pause_refresh();
    
    lv_obj_t *scr = lv_scr_act();
    
    /* 清屏 */
    lv_obj_clean(scr);
    g_settings_scr = scr;
    
    /* 应用无动画样式 */
    lv_obj_add_style(scr, &style_no_anim, LV_STATE_ANY);
    lv_obj_set_style_bg_color(scr, lv_color_make(255, 255, 255), 0);
    
    /* 标题 */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(title, lv_color_make(0, 0, 0), 0);
    lv_obj_set_size(title, 200, 30);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    /* 返回按钮 - 放在标题下方，避免重叠 */
    lv_obj_t *btn_back = lv_btn_create(scr);
    lv_obj_set_size(btn_back, 60, 30);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 10, 45);  /* Y=45，在标题下方 */
    lv_obj_set_style_bg_color(btn_back, lv_color_make(255, 255, 255), 0);
    lv_obj_set_style_border_width(btn_back, 2, 0);
    lv_obj_set_style_border_color(btn_back, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_radius(btn_back, 4, 0);
    lv_obj_set_style_transition(btn_back, NULL, LV_PART_MAIN);
    lv_obj_t *btn_back_label = lv_label_create(btn_back);
    lv_label_set_text(btn_back_label, "Back");
    lv_obj_set_style_text_font(btn_back_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(btn_back_label, lv_color_make(0, 0, 0), 0);
    lv_obj_center(btn_back_label);
    lv_obj_add_event_cb(btn_back, settings_back_btn_event_handler, LV_EVENT_CLICKED, NULL);
    
    /* WiFi 开关行 - 调整位置到返回按钮下方 */
    lv_obj_t *wifi_label = lv_label_create(scr);
    lv_label_set_text(wifi_label, "WiFi");
    lv_obj_set_style_text_font(wifi_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wifi_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_pos(wifi_label, 20, 90);  /* Y=90，在返回按钮下方 */
    
    g_settings_wifi_sw = lv_switch_create(scr);
    lv_obj_set_size(g_settings_wifi_sw, 60, 30);
    lv_obj_set_pos(g_settings_wifi_sw, 160, 85);  /* Y=85，与WiFi标签对齐 */
    lv_obj_add_style(g_settings_wifi_sw, &style_sw, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_transition(g_settings_wifi_sw, NULL, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_add_event_cb(g_settings_wifi_sw, settings_wifi_switch_event_handler, LV_EVENT_VALUE_CHANGED, NULL);
    
    /* 同步 WiFi 状态 */
    if (g_wifi_connected) {
        lv_obj_add_state(g_settings_wifi_sw, LV_STATE_CHECKED);
    }
    
    /* HTTP Server 按钮（仅 WiFi 连接时显示）- 调整位置 */
    g_settings_http_btn = lv_btn_create(scr);
    lv_obj_set_size(g_settings_http_btn, 200, 45);
    lv_obj_set_pos(g_settings_http_btn, 20, 130);  /* Y=130，在WiFi开关下方 */
    lv_obj_set_style_bg_color(g_settings_http_btn, lv_color_make(255, 255, 255), 0);
    lv_obj_set_style_border_width(g_settings_http_btn, 2, 0);
    lv_obj_set_style_border_color(g_settings_http_btn, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_radius(g_settings_http_btn, 8, 0);
    lv_obj_set_style_transition(g_settings_http_btn, NULL, LV_PART_MAIN);
    
    lv_obj_t *http_label = lv_label_create(g_settings_http_btn);
    lv_label_set_text(http_label, LV_SYMBOL_WIFI "  HTTP Server");
    lv_obj_set_style_text_font(http_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(http_label, lv_color_make(0, 0, 0), 0);
    lv_obj_center(http_label);
    
    lv_obj_add_event_cb(g_settings_http_btn, settings_http_btn_event_handler, LV_EVENT_CLICKED, NULL);
    
    /* 根据 WiFi 状态决定是否显示 HTTP 按钮 */
    if (!g_wifi_connected) {
        lv_obj_add_flag(g_settings_http_btn, LV_OBJ_FLAG_HIDDEN);
    }
    
    /* 【EPD优化】恢复刷新，触发一次完整刷新 */
    epd_resume_refresh();
    
    printf("[UI] Settings page created\n");
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
    
    // 挂载SD卡文件系统
    printf("[FM] Mounting SD card...\n");
    fs_ctrl_mount(FS_MNT_DEV_TYPE_SDCARD, 0);
    
    // 启动文件管理器
    file_manager_init();
    
    // 【EPD优化】FM创建完成后恢复刷新，会自动触发一次完整刷新
    epd_resume_refresh();
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
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(icon, lv_color_make(0, 0, 0), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *label = lv_label_create(tile);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_width(label, w - 12);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -10);

    return tile;
}

static void lvgl_task(void *arg) {
    printf("[LVGL] Task started\r\n");
    
    while (1) {
        /* 【关键修复】：告诉 LVGL 时间过去了 LVGL_TIMER_PERIOD 毫秒 */
        lv_tick_inc(LVGL_TIMER_PERIOD);
        
        /* LVGL定时器处理（内部会检查是否该触发 read_cb 了） */
        lv_timer_handler();
        
        /* 休眠一小段时间 */
        OS_MSleep(LVGL_TIMER_PERIOD);
    }
}

/*====================
 * 显示刷新任务
 *===================*/

static void disp_task(void *arg) {
    printf("[Display] Task started\r\n");
    
    while (1) {
        screensaver_task();

        /* 处理待刷新的显示 - 检查状态 */
        lv_port_disp_task();
        
        /* 执行EPD刷新 - 可能阻塞本线程，但不影响lvgl */
        epd_do_refresh();
        
        /* 休眠 */
        OS_MSleep(DISP_TASK_PERIOD);
    }
}

/*====================
 * WiFi后台连接任务
 *===================*/
static void wifi_connect_task(void *arg) {
    printf("[WiFi_TASK] ========================================\n");
    printf("[WiFi_TASK] Background WiFi connect task STARTED\n");
    printf("[WiFi_TASK] SSID: %s, PASSWORD: %s\n", WIFI_SSID, WIFI_PASSWD);
    printf("[WiFi_TASK] ========================================\n");
    
    OS_MSleep(1000);  /* 等待UI完全初始化和EPD刷新完成 */
    
    printf("[WiFi_TASK] Now connecting to WiFi...\n");
    int ret = wlan_manager_connect(WIFI_SSID, WIFI_PASSWD);
    printf("[WiFi_TASK] wlan_manager_connect returned: %d\n", ret);
    
    if (ret == 0) {
        printf("[WiFi_TASK] Waiting for IP address (timeout 30s)...\n");
        int wait_ret = wlan_manager_wait_for_ip(30000);
        printf("[WiFi_TASK] wlan_manager_wait_for_ip returned: %d\n", wait_ret);
        
        if (wait_ret == 0) {
            WLAN_IPInfo_t ip_info;
            wlan_manager_get_ip_info(&ip_info);
            printf("[WiFi_TASK] WiFi connected! IP: %s\n", ip_info.ip);
            
            g_wifi_connected = 1;
            snprintf(g_ip_text, sizeof(g_ip_text), "IP: %s", ip_info.ip);
            
            /* 更新WiFi图标 - 显示 */
            if (g_wifi_icon) {
                lv_label_set_text(g_wifi_icon, LV_SYMBOL_WIFI);
                printf("[WiFi_TASK] WiFi icon updated to show\n");
            } else {
                printf("[WiFi_TASK] WARNING: g_wifi_icon is NULL!\n");
            }
            
            /* 更新首页网络标签 */
            if (g_net_label) {
                lv_label_set_text(g_net_label, g_ip_text);
                printf("[WiFi_TASK] Network label updated\n");
            }
            
            epd_mark_refresh_pending();
            printf("[WiFi_TASK] EPD refresh marked\n");
        } else {
            printf("[WiFi_TASK] WiFi connect TIMEOUT!\n");
            g_wifi_connected = 0;
        }
    } else {
        printf("[WiFi_TASK] WiFi connect FAILED with error: %d\n", ret);
        g_wifi_connected = 0;
    }
    
    printf("[WiFi_TASK] Background WiFi connect task FINISHED\n");
    printf("[WiFi_TASK] ========================================\n");
    OS_ThreadDelete(&wifi_connect_thread);
}

/*====================
 * 主界面创建函数（可被 CoreMark 返回按钮调用重建）
 *===================*/
void main_ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();

    /* 清屏 */
    lv_obj_clean(scr);

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

    /* 标题 */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Home");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(title, lv_color_make(0, 0, 0), 0);
    lv_obj_set_size(title, 200, 30);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* WiFi图标 - 与Home标题对齐，初始隐藏 */
    g_wifi_icon = lv_label_create(scr);
    lv_label_set_text(g_wifi_icon, "");  /* 初始为空，连接成功后显示WiFi符号 */
    lv_obj_set_style_text_font(g_wifi_icon, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_wifi_icon, lv_color_make(0, 0, 0), 0);
    lv_obj_align(g_wifi_icon, LV_ALIGN_TOP_RIGHT, -10, 10);  /* 与Home标题同一行，右侧对齐 */
    if (g_wifi_connected) {
        lv_label_set_text(g_wifi_icon, LV_SYMBOL_WIFI);
    }

    /* 第一行按钮: Files 和 Settings */
    create_home_tile(scr, 10, 52, 106, 84,
                     LV_SYMBOL_DIRECTORY,
                     "Files",
                     file_manager_btn_event_handler);

    create_home_tile(scr, 124, 52, 106, 84,
                     LV_SYMBOL_SETTINGS,
                     "Settings",
                     settings_btn_event_handler);

    /* 第二行按钮: CoreMark */
    create_home_tile(scr, 10, 144, 220, 60,
                     LV_SYMBOL_CHARGE,
                     "CoreMark",
                     coremark_btn_event_handler);

    /* 网络状态标签 */
    g_net_label = lv_label_create(scr);
    lv_label_set_text(g_net_label, g_ip_text);
    lv_obj_set_style_text_font(g_net_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_net_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_size(g_net_label, 120, 20);
    lv_obj_align(g_net_label, LV_ALIGN_TOP_RIGHT, -10, 34);

    printf("[UI] Main UI created\n");
    
    /* 主界面创建完成后，请求刷新EPD */
    epd_mark_refresh_pending();
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
    
    /* 平台初始化 */
    platform_init();
    
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
    /* 等待3秒确保EXT LDO电压稳定后再初始化SD卡 */
    printf("[SD] Waiting 3s for EXT LDO to stabilize...\r\n");
    OS_Sleep(3);
    
    /* 执行FatFs文件系统测试（FatFs内部会初始化SD卡） */
    fatfs_filesystem_test();
    
    /* 初始化EPUB专用PSRAM缓冲区 */
    extern void epub_buffer_init(void);
    printf("[EPUB] Initializing EPUB buffer...\r\n");
    epub_buffer_init();
    printf("[EPUB] EPUB buffer initialized\r\n");
    
    /* 初始化LVGL - lv_port_disp_init()会初始化EPD */
    printf("[MAIN] Step 1: Calling lv_init()...\r\n");
    printf("[MAIN] Calling lv_init()...\r\n");
    fflush(stdout);
    lv_init();
    printf("[MAIN] lv_init() returned\r\n");
    printf("[MAIN] Step 1: lv_init() completed\r\n");
    
    /* 初始化显示端口 */
    printf("[MAIN] Step 2: Calling lv_port_disp_init()...\r\n");
    lv_port_disp_init();
    printf("[MAIN] Step 2: lv_port_disp_init() completed\r\n");
    
    /* 初始化输入端口 (触摸) */
    printf("[MAIN] Step 3: Calling lv_port_indev_init()...\r\n");
    lv_port_indev_init();
    printf("[MAIN] Step 3: lv_port_indev_init() completed\r\n");
    
    /*====================
     * 网络初始化
     *===================*/
    printf("\r\n");
    printf("======================================\r\n");
    printf("Network Initialization\r\n");
    printf("======================================\r\n\r\n");
    
    /* 初始化WLAN模块 */
    printf("[NET] Initializing WLAN...\r\n");
    if (wlan_manager_init() != 0) {
        printf("[NET ERROR] WLAN init failed!\r\n");
    } else {
        printf("[NET] WLAN initialized\r\n");
        /* WiFi连接将在后台任务中进行，不阻塞UI启动 */
    }
    printf("\r\n");
    
    /* 创建主界面UI - 先显示UI，WiFi后台连接 */
    main_ui_create();
    screensaver_init();
    
    printf("\r\n[OK] LVGL system initialized!\r\n");
    printf("[INFO] Controls: Files and Settings tiles\r\n");
    printf("[INFO] WiFi connecting in background...\r\n\r\n");

    /* UI创建完毕，启动后台任务 */
    printf("[Display] create disp_task stack=%d\r\n", 8192);
    print_heap_info();
    psram_heap_info();
    dma_heap_info();
    if (OS_ThreadCreate(&disp_task_thread, "disp_task", disp_task, NULL,
                        OS_PRIORITY_NORMAL, 8192) != 0) {
        printf("[ERROR] Failed to create disp_task\r\n");
    }
    if (OS_ThreadCreate(&lvgl_thread, "lvgl_task", lvgl_task, NULL,
                        OS_PRIORITY_NORMAL, 20480) != 0) {
        printf("[ERROR] Failed to create lvgl_task\r\n");
    }

    /* 启动WiFi后台连接任务 */
    printf("[WiFi] Starting background WiFi connect task\r\n");
    if (OS_ThreadCreate(&wifi_connect_thread, "wifi_task", wifi_connect_task, NULL,
                        OS_PRIORITY_NORMAL, 4096) != 0) {
        printf("[ERROR] Failed to create wifi_connect_task\r\n");
    }

    // 初始化完成，恢复EPD刷新
    epd_resume_refresh();
    printf("[EPD] Refresh resumed after init\n");

    /* 主线程完成，LVGL任务在后台运行 */
    while (1) {
        OS_MSleep(1000);
    }
    
    return 0;
}
