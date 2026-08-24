#include "sd_recovery.h"
#include "lvgl/lvgl.h"
#include "fs/fatfs/ff.h"
#include "common/framework/fs_ctrl.h"
#include "driver/chip/hal_prcm.h"
#include "driver/chip/sdmmc/sdmmc.h"
#include "driver/chip/sdmmc/hal_sdhost.h"
#include "sys/xr_debug.h"
#include "prj_config.h"
#include <stdio.h>
#include <string.h>

/* HAL_SDC_Update_Clk 是 ROM 导出的绝对符号(.map 中 =0x11459), 与 HAL_SDC_Create
 * 同一机制, 应用层可直接 extern 调用, 用于把协商后的 SD 时钟进一步降频。 */
extern int32_t HAL_SDC_Update_Clk(struct mmc_host *host, uint32_t clk);

#define SD_RECOVERY_POLL_MS     20
#define SD_RECOVERY_TIMEOUT_MS  8000

static volatile int s_requested;
static volatile uint32_t s_done_gen;   /* 完成代数, 避免信号量计数残留 */
static int s_ready;
static volatile int s_format_busy;     /* 格式化期间禁止休眠, 防止 f_mkfs 被挂起写坏 FAT */

void sd_format_set_busy(int busy)
{
    s_format_busy = busy;
}

int sd_format_is_busy(void)
{
    return s_format_busy;
}

/* 完整重建 SD 子系统: 与 board_sdcard_init / charge_ensure_sd_ready 相同参数。
 * 原为 LVGL 定时器专用 (sd_recovery_timer_cb), 现公开给 boot 自愈路径:
 * boot 阶段单线程直调也安全 (此时无并发 SD 访问)。 */
void sd_recovery_rebuild_now(void)
{
    printf("[SD-REC] full SD re-init begin\r\n");

    /* 1. 卸载卷 + 删卡 (清 fs_ctrl.fs, 否则 fs_ctrl_mount 短路返回) */
    fs_ctrl_unmount(FS_MNT_DEV_TYPE_SDCARD, 0);

    /* 2. 关闭 SD 主机 (时钟关 + 总线复位) */
    HAL_SDC_Deinit(0);

    /* 3. 补 EXT LDO: Deinit 可能关掉了 LDO, 重新打开并等稳定 */
    HAL_PRCM_SelectEXTLDOVolt(PRCM_EXT_LDO_3V3);
    HAL_PRCM_SetEXTLDOMode(PRCM_EXTLDO_ALWAYS_ON);
    OS_MSleep(100);

    /* 4. 重建主机: HAL_SDC_Deinit 后 _mci_host[0] 仍指向被清零的旧 host,
     *    HAL_SDC_Create 对"已存在"的 host 只打印 "has already created!"
     *    就原样返回, 不会恢复 param/debug_mask/dma_use —— 用这种半初始化
     *    host 去 HAL_SDC_Init + mmc_rescan 必然失败。先 HAL_SDC_Destory
     *    释放旧 host (置 _mci_host[0]=NULL), 再 Create 全新分配, 保证
     *    param/dma_use/debug_mask 与开机一致。 */
    SDC_InitTypeDef sdc_param;
    memset(&sdc_param, 0, sizeof(sdc_param));
    sdc_param.debug_mask = ROM_WRN_MASK | ROM_ERR_MASK | ROM_ANY_MASK;
    sdc_param.dma_use = 1;
    /* 必须与 board_sdcard_init 一致传 CARD_ALWAYS_PRESENT: 漏设 cd_mode 时
     * (memset 后为 0), HAL_SDC_Init 在 CONFIG_DETECT_CARD 下不会置 present=1,
     * 重建后的 host 永远"no medium present", 重扫必然失败。 */
    sdc_param.cd_mode = PRJCONF_MMC_DETECT_MODE;
    struct mmc_host *host = HAL_SDC_Create(0, &sdc_param);
    if (host != NULL) {
        HAL_SDC_Destory(host);   /* 释放 Deinit 后残留的零化旧 host */
    }
    host = HAL_SDC_Create(0, &sdc_param);
    if (host == NULL) {
        printf("[SD-REC] host create failed\r\n");
        return;
    }
    if (HAL_SDC_Init(host) == NULL) {
        printf("[SD-REC] host init failed\r\n");
        return;
    }
    OS_MSleep(50);

    /* 5. 重新扫描 + 挂载 FatFs */
    if (fs_ctrl_mount(FS_MNT_DEV_TYPE_SDCARD, 0) != 0) {
        printf("[SD-REC] remount failed\r\n");
        return;
    }
    sd_apply_clock_policy();
    printf("[SD-REC] full SD re-init done, SD ready\r\n");
}

/* SD 时钟策略: DCE 的根因是 DAT 时序/供电余量不足(大文件上传 + WiFi 并发持续
 * 写盘时, 每台都复现)。驱动已在 HAL_SDC_Calibrate 关掉 High-Speed(停在 DS
 * 25MHz), 但 25MHz 下仍会 DCE, 因此这里再用 ROM 的 HAL_SDC_Update_Clk 进一步
 * 降频拉开余量做验证。必须等 mmc_rescan 完成速度协商后再调用(此时 host->clk
 * 已定); 函数幂等, boot 挂载后与恢复重挂后各调一次即可。
 * 若 12.5MHz 仍有 DCE, 可继续降到 6250000; 想回到默认 25MHz 就改回 25000000。 */
#define FONTEXP_SD_FORCE_CLK_HZ   12500000u

void sd_apply_clock_policy(void)
{
    struct mmc_host *host;

    host = HAL_SDC_Open(0);
    if (host == NULL) {
        printf("[SD-REC] clock policy: no host\r\n");
        return;
    }
    /* struct mmc_host 在应用层是不透明类型, 无法读 host->clk 判断是否已生效,
     * 且恢复重挂后时钟会被重扫重置回 25MHz, 需要重新降 —— 所以每次都直接调。 */
    if (HAL_SDC_Update_Clk(host, FONTEXP_SD_FORCE_CLK_HZ) == 0) {
        printf("[SD-REC] SD clock forced to %u Hz\r\n",
               (unsigned)FONTEXP_SD_FORCE_CLK_HZ);
    } else {
        printf("[SD-REC] SD clock update failed\r\n");
    }
    HAL_SDC_Close(0);
}

static void sd_recovery_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_requested) {
        return;
    }
    s_requested = 0;
    sd_recovery_rebuild_now();
    s_done_gen++;
}

void sd_recovery_init(void)
{
    lv_timer_t *timer;

    if (s_ready) {
        return;
    }
    timer = lv_timer_create(sd_recovery_timer_cb, SD_RECOVERY_POLL_MS, NULL);
    if (timer == NULL) {
        printf("[SD-REC] timer create failed\r\n");
        return;
    }
    lv_timer_set_repeat_count(timer, -1);   /* 常驻 */
    s_ready = 1;
    printf("[SD-REC] recovery service ready (poll %dms)\r\n", SD_RECOVERY_POLL_MS);
}

int sd_recovery_request_wait(void)
{
    uint32_t gen;
    uint32_t t0;

    if (!s_ready) {
        printf("[SD-REC] service not ready\r\n");
        return -1;
    }
    gen = s_done_gen;
    s_requested = 1;
    t0 = OS_GetTicks();
    while (s_done_gen == gen) {
        if (OS_TicksToMSecs(OS_GetTicks() - t0) > SD_RECOVERY_TIMEOUT_MS) {
            printf("[SD-REC] recovery timed out\r\n");
            return -1;
        }
        OS_MSleep(5);
    }
    return 0;
}
