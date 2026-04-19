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

/* platform_init声明 */
extern void platform_init(void);

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

static void switch_event_handler(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * sw = lv_event_get_target(e);

    if(code == LV_EVENT_VALUE_CHANGED) {
        // 打印开关状态和触摸状态
        TouchState_t touch_state = touch_get_state();
        printf("[UI] Switch Toggled! State: %s, Touch: %s\n",
                lv_obj_has_state(sw, LV_STATE_CHECKED) ? "ON" : "OFF",
                touch_state == TOUCH_STATE_PRESSED ? "PRESSED" :
                touch_state == TOUCH_STATE_RELEASED ? "RELEASED" : "IDLE");
    }
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
        /* 处理待刷新的显示 - 检查状态 */
        lv_port_disp_task();
        
        /* 执行EPD刷新 - 可能阻塞本线程，但不影响lvgl */
        epd_do_refresh();
        
        /* 休眠 */
        OS_MSleep(DISP_TASK_PERIOD);
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
        
        /* 连接WIFI - 使用定义的SSID和密码 */
        printf("[NET] Connecting to WiFi: %s\r\n", WIFI_SSID);
        if (wlan_manager_connect(WIFI_SSID, WIFI_PASSWD) == 0) {
            /* 等待连接成功获取IP - 30秒超时 */
            if (wlan_manager_wait_for_ip(30000) == 0) {
                WLAN_IPInfo_t ip_info;
                wlan_manager_get_ip_info(&ip_info);
                printf("[NET] WiFi connected! IP: %s\r\n", ip_info.ip);
                
                /* 启动HTTP服务器 */
                printf("[NET] Starting HTTP server on port %d...\r\n", HTTP_SERVER_PORT);
                http_server_init(HTTP_SERVER_PORT);
                if (http_server_start() == 0) {
                    printf("[NET] HTTP server started!\r\n");
                    printf("[NET] Access http://%s to manage files\r\n", ip_info.ip);
                } else {
                    printf("[NET ERROR] HTTP server start failed!\r\n");
                }
            } else {
                printf("[NET ERROR] Failed to get IP address!\r\n");
            }
        } else {
            printf("[NET ERROR] WiFi connection failed!\r\n");
        }
    }
    printf("\r\n");
    
    /* 创建演示UI - 控件测试 */
    
    /* 优化三：全局关闭LVGL动画 - 消除墨水屏"狂闪" */
    /*
     * 通过重写 LVGL 的全局样式，将所有状态切换（按下、聚焦、检查等）的过渡时间强制设为 0
     * 这样即使触摸 IC 反复触发状态切换，也不会有任何视觉上的位移动画
     */
    static lv_style_t style_no_anim;
    lv_style_init(&style_no_anim);
    
    /* 核心：将动画时间参数设为 0 */
    lv_style_set_anim_time(&style_no_anim, 0);         // 禁用组件内部动画
    // 【EPD优化】所有按钮按下时无边框变化
    lv_style_set_border_width(&style_no_anim, 0);
    
    lv_obj_t *scr = lv_scr_act();
    printf("[MAIN] scr=%p\n");
    
    /* 针对所有对象（Part Main）在所有状态下应用此样式 */
    lv_obj_add_style(scr, &style_no_anim, LV_STATE_ANY);
    lv_obj_set_style_bg_color(scr, lv_color_make(255, 255, 255), 0);  // 白色背景
    printf("[MAIN] scr styled\n");
    
    // 标题
    printf("[MAIN] Creating title label...\n");
    lv_obj_t *title = lv_label_create(scr);
    printf("[MAIN] title=%p created\n", (void*)title);
    lv_label_set_text(title, "LVGL EPD Controls");
    lv_obj_set_style_text_color(title, lv_color_make(0, 0, 0), 0);
    lv_obj_set_size(title, 200, 30);
    printf("[MAIN] title set_size(200,30) called\n");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // 检查title coords
    lv_area_t title_coords;
    lv_obj_get_coords(title, &title_coords);
    printf("[MAIN] title coords: (%d,%d)-(%d,%d), w=%d, h=%d\n",
           title_coords.x1, title_coords.y1, title_coords.x2, title_coords.y2,
           title_coords.x2 - title_coords.x1 + 1, title_coords.y2 - title_coords.y1 + 1);
    
    // 开关1 - 带标签
    printf("[MAIN] Creating sw1_label...\n");
    lv_obj_t *sw1_label = lv_label_create(scr);
    lv_label_set_text(sw1_label, "Switch 1:");
    lv_obj_set_style_text_color(sw1_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_size(sw1_label, 80, 20);
    lv_obj_align(sw1_label, LV_ALIGN_LEFT_MID, 20, -80);
    
    printf("[MAIN] Creating sw1...\n");
    lv_obj_t *sw1 = lv_switch_create(scr);
    printf("[MAIN] sw1=%p created\n", (void*)sw1);
    lv_obj_set_size(sw1, 50, 25);
    printf("[MAIN] sw1 set_size(50,25) called\n");
    lv_obj_align(sw1, LV_ALIGN_LEFT_MID, 120, -80);
    
    // 检查sw1 coords
    lv_area_t sw1_coords;
    lv_obj_get_coords(sw1, &sw1_coords);
    printf("[MAIN] sw1 coords: (%d,%d)-(%d,%d), w=%d, h=%d\n",
           sw1_coords.x1, sw1_coords.y1, sw1_coords.x2, sw1_coords.y2,
           sw1_coords.x2 - sw1_coords.x1 + 1, sw1_coords.y2 - sw1_coords.y1 + 1);
    lv_obj_add_state(sw1, LV_STATE_CHECKED);  // 默认开启
    // 开关: 设置边框使开关可见
    static lv_style_t style_sw;
    lv_style_init(&style_sw);
    lv_style_set_border_width(&style_sw, 2);
    lv_style_set_border_color(&style_sw, lv_color_make(0, 0, 0));  // 黑色边框
    lv_style_set_radius(&style_sw, LV_RADIUS_CIRCLE);
    lv_obj_add_style(sw1, &style_sw, LV_PART_MAIN);  // 给主体加边框
    
    // 【真正的精准打击】：必须明确指定 INDICATOR、KNOB 和 MAIN
   lv_obj_set_style_transition(sw1, NULL, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_transition(sw1, NULL, LV_PART_INDICATOR | LV_STATE_ANY);
    lv_obj_set_style_transition(sw1, NULL, LV_PART_KNOB | LV_STATE_ANY);
    
    // 为sw1添加事件处理
    lv_obj_add_event_cb(sw1, switch_event_handler, LV_EVENT_VALUE_CHANGED, NULL);
    
    // 开关2 - 带标签
    lv_obj_t *sw2_label = lv_label_create(scr);
    lv_label_set_text(sw2_label, "Switch 2:");
    lv_obj_set_style_text_color(sw2_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_size(sw2_label, 80, 20);
    lv_obj_align(sw2_label, LV_ALIGN_LEFT_MID, 20, -30);
    
    lv_obj_t *sw2 = lv_switch_create(scr);
    lv_obj_set_size(sw2, 50, 25);
    lv_obj_align(sw2, LV_ALIGN_LEFT_MID, 120, -30);
    // sw2 默认关闭状态，不需要特殊设置
    
    // 【真正的精准打击】：必须明确指定 INDICATOR、KNOB 和 MAIN
    // 对开关的所有 Part 彻底禁用过渡动画
    lv_obj_set_style_transition(sw2, NULL, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_transition(sw2, NULL, LV_PART_INDICATOR | LV_STATE_ANY);
    lv_obj_set_style_transition(sw2, NULL, LV_PART_KNOB | LV_STATE_ANY);
    
    // 为sw2添加事件处理
    lv_obj_add_event_cb(sw2, switch_event_handler, LV_EVENT_VALUE_CHANGED, NULL);
    
    // 滑块 - 带标签和数值显示
    lv_obj_t *slider_label = lv_label_create(scr);
    lv_label_set_text(slider_label, "Slider:");
    lv_obj_set_style_text_color(slider_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_size(slider_label, 80, 20);
    lv_obj_align(slider_label, LV_ALIGN_LEFT_MID, 20, 20);
    
    lv_obj_t *slider = lv_slider_create(scr);
    lv_obj_set_size(slider, 150, 30);
    lv_obj_align(slider, LV_ALIGN_LEFT_MID, 120, 20);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 50, LV_ANIM_OFF);
    
    // 【真正的精准打击】：必须明确指定 INDICATOR、KNOB 和 MAIN
    lv_obj_set_style_transition(slider, NULL, LV_PART_INDICATOR);
    lv_obj_set_style_transition(slider, NULL, LV_PART_KNOB);
    lv_obj_set_style_transition(slider, NULL, LV_PART_MAIN);
    
    // 滑块数值标签
    lv_obj_t *slider_value = lv_label_create(scr);
    lv_label_set_text_fmt(slider_value, "%d%%", 50);
    lv_obj_set_style_text_color(slider_value, lv_color_make(0, 0, 0), 0);
    lv_obj_set_size(slider_value, 50, 20);
    lv_obj_align(slider_value, LV_ALIGN_LEFT_MID, 280, 20);
    lv_obj_set_style_text_color(slider_value, lv_color_make(0, 0, 0), 0);
    lv_obj_align(slider_value, LV_ALIGN_LEFT_MID, 280, 20);
    
    // 按钮1
    lv_obj_t *btn1 = lv_btn_create(scr);
    lv_obj_set_size(btn1, 80, 40);
    lv_obj_align(btn1, LV_ALIGN_LEFT_MID, 20, 80);
    lv_obj_t *btn1_label = lv_label_create(btn1);
    lv_label_set_text(btn1_label, "Button 1");
    lv_obj_set_style_text_color(btn1_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_size(btn1_label, 70, 30);
    
    // 【真正的精准打击】：按钮也有过渡，关闭它
    lv_obj_set_style_transition(btn1, NULL, LV_PART_MAIN);
    lv_obj_set_style_transition(btn1, NULL, LV_PART_SCROLLBAR);
    
    // 按钮2
    lv_obj_t *btn2 = lv_btn_create(scr);
    lv_obj_set_size(btn2, 80, 40);
    lv_obj_align(btn2, LV_ALIGN_LEFT_MID, 120, 80);
    lv_obj_t *btn2_label = lv_label_create(btn2);
    lv_label_set_text(btn2_label, "Button 2");
    lv_obj_set_style_text_color(btn2_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_size(btn2_label, 70, 30);
    
    // 【真正的精准打击】：按钮也有过渡，关闭它
    lv_obj_set_style_transition(btn2, NULL, LV_PART_MAIN);
    lv_obj_set_style_transition(btn2, NULL, LV_PART_SCROLLBAR);
    
    // 文件管理器按钮
    lv_obj_t *btn_fm = lv_btn_create(scr);
    lv_obj_set_size(btn_fm, 120, 40);
    lv_obj_align(btn_fm, LV_ALIGN_LEFT_MID, 20, 130);
    lv_obj_set_style_transition(btn_fm, NULL, LV_PART_MAIN);
    lv_obj_set_style_transition(btn_fm, NULL, LV_PART_SCROLLBAR);
    lv_obj_t *btn_fm_label = lv_label_create(btn_fm);
    lv_label_set_text(btn_fm_label, "File Manager");
    lv_obj_set_style_text_color(btn_fm_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_size(btn_fm_label, 110, 30);
    lv_obj_add_event_cb(btn_fm, file_manager_btn_event_handler, LV_EVENT_CLICKED, NULL);
    
    // 网络状态标签
    lv_obj_t *net_label = lv_label_create(scr);
    if (http_server_is_running()) {
        lv_label_set_text(net_label, "WiFi: ON");
    } else {
        lv_label_set_text(net_label, "WiFi: OFF");
    }
    lv_obj_set_style_text_color(net_label, lv_color_make(0, 128, 0), 0);
    lv_obj_set_size(net_label, 100, 20);
    lv_obj_align(net_label, LV_ALIGN_TOP_RIGHT, -10, 10);
    
    printf("\r\n[OK] LVGL system initialized!\r\n");
    printf("[INFO] Controls: 2 switches, 1 slider, 3 buttons\r\n");
    printf("[INFO] Network: %s\r\n\r\n",
           http_server_is_running() ? "enabled" : "disabled");

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
                        OS_PRIORITY_NORMAL, 32768) != 0) {
        printf("[ERROR] Failed to create lvgl_task\r\n");
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
