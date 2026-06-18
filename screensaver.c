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
#include "wlan_manager.h"
#include "wifi_controller.h"
#include "driver/chip/hal_wakeup.h"
#include "driver/chip/hal_prcm.h"
#include "pm/pm.h"
#include "driver/chip/hal_gpio.h"
#include "driver/chip/hal_i2c.h"
#include "driver/chip/sdmmc/sdmmc.h"
#include "driver/chip/sdmmc/hal_sdhost.h"
#include "pm/pm.h"

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
     * 为了兼容已经保存在 SD 卡上的 screensaver.bin,这里在加载时统一翻转一次。
     * 这样用户不需要删除旧文件重传,现有屏保也能直接正常显示。
     */
    for (i = 0; i < EPD_BUFFER_SIZE; i++) {
        framebuffer[i] ^= 0xFF;
    }

    return 0;
}

static int screensaver_enter(int force)
{
    int has_image = 0;

    if (g_ss.active) {
        return 0;
    }
    if (http_server_is_running()) {
        printf("[SS] HTTP running, refusing to enter hibernation\n");
        return -1;
    }

    /* 尝试加载屏保图片 */
    if (screensaver_load_to_framebuffer() == 0) {
        has_image = 1;
    }

    /* 无图片时:显示 Tuwa Reader 文字 */
    if (!has_image) {
        printf("[SS] no image, showing Tuwa Reader text\n");
        EPD_DrawStringCentered("Tuwa Reader");
    }

    printf("[SS] entering hibernation (has_image=%d, force=%d)\n", has_image, force);

    /* WiFi 处理: 关闭后台线程 + 解除 PM 的 wlan poweroff 回调
     * net_sys_onoff(0) 在我们环境下稳定触发 wsm_remove_key_request:1027
     * 断言崩溃 (wlan 闭源驱动)。pm_unregister 让 PM 跳过该回调,
     * hibernation 内部 SetSys1SleepPowerFlags 直接硬断电 Sys3(WLAN CPU)。 */
    wifi_controller_stop();          /* 停后台线程 */
    pm_unregister_wlan_power_onoff();/* PM 内部不再调 net_sys_onoff(0) */

    /* EPD deep sleep */
    EPD_3IN52_Init();
    EPD_3IN52_Display();
    EPD_Sleep();
    printf("[SS] EPD entered deep sleep\n");

    /* ========================================
     * 【休眠前电源管理 - 临时全部关闭以定位唤醒异常】
     * ======================================== */
#if 0  // 暂时关闭, 定位唤醒后 flash 读取失败 (rom_flash_rw fail)
    /* 1. 卸载 SD 卡文件系统 */
    printf("[SS] Unmount SD card filesystem\n");
    f_mount(NULL, "0:", 0);

    /* 2. 反初始化 SD 卡 */
    printf("[SS] Deinit SD card\n");
    struct mmc_card *card = mmc_card_open(0);
    if (card != NULL) {
        if (mmc_card_present(card)) {
            mmc_card_deinit(card);
        }
        mmc_card_close(0);
    }

    /* 3. 关闭 SD 控制器 */
    printf("[SS] Deinit SD controller\n");
    HAL_SDC_Deinit(0);

    /* 4. 关闭 EXT LDO (SD 卡 3.3V 断电) */
    printf("[SS] Power off EXT LDO (SD card)\n");
    HAL_PRCM_SetEXTLDOMode(PRCM_EXTLDO_ALWAYS_OFF);

    /* 3. 拉低 PA23 (关闭 SY8088 DC-DC) */
    printf("[SS] Power off PA23 (SY8088 DC-DC)\n");
    GPIO_InitParam param;
    param.mode = GPIOx_Pn_F1_OUTPUT;
    param.driving = GPIO_DRIVING_LEVEL_1;
    param.pull = GPIO_PULL_NONE;
    HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_23, &param);
    HAL_GPIO_WritePin(GPIO_PORT_A, GPIO_PIN_23, GPIO_PIN_LOW);

    /* 4. 拉高 PA07 (关闭 EPD 供电 MOS) */
    printf("[SS] Power off PA07 (EPD MOS)\n");
    HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_7, &param);
    HAL_GPIO_WritePin(GPIO_PORT_A, GPIO_PIN_7, GPIO_PIN_HIGH);

    /* 5. 配置 SPI GPIO 为输入模式 (防止漏电) */
    printf("[SS] Configure SPI GPIOs to input mode\n");
    param.mode = GPIOx_Pn_F0_INPUT;
    param.driving = GPIO_DRIVING_LEVEL_1;
    param.pull = GPIO_PULL_NONE;
    HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_0, &param);  // MOSI
    HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_1, &param);  // DC
    HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_2, &param);  // CLK
    HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_3, &param);  // CS

    /* 6. 关闭 I2C 总线 (CHSC6540 触摸控制器) */
    printf("[SS] Deinit I2C bus\n");
    HAL_I2C_DeInit(I2C0_ID);

    /* 7. 配置 I2C GPIO 为输入模式 */
    HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_19, &param);  // I2C0_SCL
    HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_20, &param);  // I2C0_SDA
#endif
    /* ======================================== */

    /* 配置唤醒源:
     *   WKIO2 = PA06  -> 翻页/唤醒按键 (下降沿唤醒)
     * 注意:HAL_Wakeup_SetIO 的 pn 是 WKIO 索引 0~9,不是 GPIO 引脚号。
     *   ARCH_VER==2 映射: WKIO2=PA6。传字面 2 对应 PA6。
     *   wakeup_io_en 会累加掩码: en=0x4。
     *
     * PA21 (充电检测) 不再作为唤醒源 -- 充电时 PA21 持续为低电平,
     * 作为下降沿唤醒源会干扰休眠,且会引发"充上电就开机→自动休眠→
     * 又开机"的循环。充电唤醒逻辑改由软件层处理。 */
    HAL_Wakeup_SetIO(2, WKUPIO_WK_MODE_FALLING_EDGE, GPIO_PULL_UP);   /* PA6  按键   */

    /* 进入 MCU hibernation */
    pm_enter_mode(PM_MODE_HIBERNATION);

    return 0;
}

static void screensaver_exit(void)
{
    printf("[SS] exiting screensaver\n");

    /* 在挂起LVGL线程之前,先获取当前屏幕对象 */
    lv_disp_t *disp = lv_disp_get_default();
    lv_obj_t *scr = NULL;
    if (disp) {
        scr = lv_scr_act();
    }

    /* 恢复LVGL显示能力,但暂时不启用失效,因为我们要先唤醒EPD */
    if (disp) {
        lv_disp_enable_invalidation(disp, true);
    }

    /* 现在挂起LVGL线程进行EPD操作 */
    vTaskSuspend(lvgl_thread.handle);

    printf("[SS] Waking EPD from deep sleep...\n");
    EPD_3IN52_Init();
    printf("[SS] EPD full init done, now init DU mode...\n");
    EPD_3IN52_Init_DU();
    printf("[SS] EPD DU init done\n");

    vTaskResume(lvgl_thread.handle);

    /* 清理屏幕和创建新UI在LVGL线程运行时进行 */
    if (scr) {
        lv_obj_clean(scr);
    }

    main_ui_create();

    if (disp) {
        lv_disp_trig_activity(disp);
    }

    epd_resume_refresh();

    printf("[SS] Marking content dirty for full screen refresh after screensaver exit\n");
    epd_set_content_dirty();

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

    disp = lv_disp_get_default();
    if (!disp) {
        return;
    }

    inactive_ms = lv_disp_get_inactive_time(disp);
    if (inactive_ms >= SCREENSAVER_IDLE_TIMEOUT_MS) {
        screensaver_enter(0);
    }
}

void screensaver_task_force_enter(void)
{
    if (!g_ss.initialized || g_ss.active) {
        return;
    }
    if (http_server_is_running()) {
        return;
    }
    screensaver_enter(1);
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