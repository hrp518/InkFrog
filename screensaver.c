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
#include "driver/chip/hal_adc.h"
#include "driver/chip/sdmmc/sdmmc.h"
#include "driver/chip/sdmmc/hal_sdhost.h"
#include "chsc6540.h"
#include "pm/pm.h"
#include "charge_mode.h"

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

    /* 取 EPD/framebuffer 独占所有权 (GPT clock 方案 §六)：
     * 之前 screensaver_enter 直接改 framebuffer 并 EPD_3IN52_Display()，
     * 而此时 lvgl_task 仍每 5ms 跑 flush_cb，存在 framebuffer 在传输中被
     * 修改的竞争。take 后 lvgl 线程被挂起，可安全操作 framebuffer。 */
    epd_take_ownership();

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

    /* ====== fix-power-saving v2: WiFi 手动下电 + 屏蔽 SDK 重复 teardown ======
     * 历史: 让 SDK 在 pm_enter_mode 内部 net_sys_onoff(0) 关 WiFi 会崩
     *   (PC=0x004120ee UNDEFINSTR, BH 线程与 umac/wpas 竞争 wlan 驱动)。
     * 原版不崩是因为没有用户线程碰 wlan; FontExp 必须手动安全下电。
     *
     * 顺序 (对照原版休眠日志, 但避开 SDK 崩溃路径):
     *   1. wifi_controller_stop()  停 wc_task 线程
     *   2. pm_unregister_wlan_power_onoff()  屏蔽 SDK 的 net_sys_onoff(0)
     *   3. wifi_controller_poweroff()  手动 wlan_sta_disable + 等 Sys3 掉电 */

    /* 1. WiFi: 停 wc_task 线程 */
    wifi_controller_stop();
    /* 2. 屏蔽 SDK 在 pm_enter_mode 内部的 net_sys_onoff(0) (崩溃源) */
    pm_unregister_wlan_power_onoff();
    /* 3. 手动 wlan_sta_disable + while(IsSys3Alive()) 等协处理器掉电 */
    wifi_controller_poweroff();

    /* 4. EPD deep sleep (原版在 cg_flush_task 里做, 我们在这做) */
    EPD_3IN52_Init();
    EPD_3IN52_Display();
    EPD_Sleep();
    printf("[SS] EPD entered deep sleep\n");

    /* 5. 触摸 + I2C0 下电 (新增, 原版由驱动框架 suspend 链处理) */
    CHSC6540_DeInit();
    printf("[SS] Touch controller deinitialized\n");

    /* 6. ADC 下电 (新增, 停 VBAT 连续转换, 省电) */
    HAL_ADC_DeInit();
    printf("[SS] ADC deinitialized\n");

    /* 7. SD 卡: unmount + 卡 deinit + SDC 控制器 deinit (原版 "spi deinit") */
    printf("[SS] Unmount SD card filesystem\n");
    f_mount(NULL, "0:", 0);

    printf("[SS] Deinit SD card\n");
    struct mmc_card *card = mmc_card_open(0);
    if (card != NULL) {
        if (mmc_card_present(card)) {
            mmc_card_deinit(card);
        }
        mmc_card_close(0);
    }

    printf("[SS] Deinit SD controller (spi deinit)\n");
    HAL_SDC_Deinit(0);

    /* 配置唤醒源:
     *   WKIO2 = PA06  -> 翻页/唤醒按键 (下降沿唤醒)
     * 注意:HAL_Wakeup_SetIO 的 pn 是 WKIO 索引 0~9,不是 GPIO 引脚号。
     *   ARCH_VER==2 映射: WKIO2=PA6。传字面 2 对应 PA6。
     *   wakeup_io_en 会累加掩码: en=0x4。
     *
     * PA21(WKIO7) 下降沿唤醒(插入充电器):
     *   休眠时 PA21 被上拉为高, 插入充电器拉低 → 下降沿 → 进入充电模式。
     *   和 PA6 一样用下降沿, 不会循环: 只有高→低才触发, 休眠时正好是高。
     *   充电模式里会用相反的上升沿(低→高=拔出), 两个状态边沿方向不同不会互串。 */
    HAL_Wakeup_SetIO(2, WKUPIO_WK_MODE_FALLING_EDGE, GPIO_PULL_UP);   /* PA6  按键      */
    HAL_Wakeup_SetIO(7, WKUPIO_WK_MODE_FALLING_EDGE, GPIO_PULL_UP);   /* PA21 插入充电器 */
    /* SDK 在 pm_enter_mode→__suspend_enter 内部调 HAL_Wakeup_SetSrc, 这里不用调 */

    /* fix-power-saving: 【不手动关 EXT LDO!】
     * 根据 XR872_User_Manual §3.1: EXT LDO 给 VDDIO/GPADC/CODEC 供电,
     * 而 PA6(WKIO2) 的输入检测电路依赖 VDDIO。手动 SetEXTLDOMode(OFF)
     * 会断掉 VDDIO -> PA6 失电 -> 无法唤醒(实测确认)。
     *
     * 正确做法: 让硬件 PMU 在进 hibernation 时按 SYS1_SLEEP_CTRL 自动
     * 管理电源域(手册 §3.3.3 Wakeup from Hibernation: VDD_EXT 唤醒时
     * 硬件自动恢复)。原版日志里的 mod_vfat_power en=0 是原版应用私有
     * 代码,可能针对不同硬件版本,不适用于我们。
     *
     * 已下电的外设(WiFi/触摸/ADC/SD)不受 EXT LDO 影响,省电效果保留。 */

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
    lv_timer_handler_unblock_after_suspend();

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
        /* 空闲超时: 插着电 → 充电模式(省电), 未充电 → 普通屏保休眠 */
        if (HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_21) == 0) {
            printf("[SS] idle timeout + charging -> charge mode\r\n");
            charge_mode_enter();    /* 不返回 */
        }
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