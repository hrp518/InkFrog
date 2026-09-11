/*
 * sdk_fixes.c — SDK bugfix 的链接期外移 (零 SDK 改动)
 *
 * 两个修复原先都写在 SDK 源码里, 现通过 gcc --wrap (SDK 链接本就大量
 * 使用该机制: main/malloc/time 等) 在工程侧实现, SDK 保持原版:
 *
 * 修复1 — SD 容量换算 (原 src/fs/fatfs/driver/sdmmc_diskio.c):
 *   sdmmc_get_sector_count() 把以字节为单位的 csd.capacity 按 KB×2 换算,
 *   扇区数放大 1024 倍, f_mkfs 建出 ~500GB 假卷死循环。
 *   这里包装 SDMMC_ioctl (ff.c 经 src/fs/fatfs/diskio.c 跨对象调用,
 *   wrap 生效), GET_SECTOR_COUNT 用公共 API 按字节÷512 正确换算,
 *   其余命令透传 __real_SDMMC_ioctl。
 *
 * 修复2 — HIBERNATION 保持 32K 校准 (原 src/pm/pm.c):
 *   pm_enter_mode() 对 >=STANDBY 的状态一律关闭 32K 校准, 但休眠唤醒是
 *   冷启动, boot ROM 需要准确的 32K 做 flash 初始化时序, 关闭后
 *   rom_flash_rw fail 无法启动。这里包装 HAL_PRCM_DisableInter32KCalib,
 *   置位 sdk_keep_32k_calib(1) 后跳过关闭; 唤醒即冷启动, 标志自然复位。
 */
#include <stdint.h>
#include "fs/fatfs/diskio.h"
#include "driver/chip/sdmmc/sdmmc.h"
#include "sdk_fixes.h"

/* ---------- 修复1: SD 卡扇区数 ---------- */
DRESULT __wrap_SDMMC_ioctl(BYTE cmd, void *buff)
{
	if (cmd == GET_SECTOR_COUNT) {
		struct mmc_card *card = mmc_card_open(0);
		uint32_t sectors = 0;

		if (card != NULL) {
			if (mmc_card_present(card)) {
				uint32_t capacity = mmc_get_capacity(card); /* 单位: 字节 */
				if (capacity >= 512)
					sectors = capacity / 512;               /* 转扇区数 */
			}
			mmc_card_close(0);
		}

		if (sectors != 0) {
			*(uint32_t *)buff = sectors;
			return RES_OK;
		}
		return RES_PARERR;
	}
	if (cmd == GET_BLOCK_SIZE) {
		/* _USE_MKFS=0 的原版 libfs 对此命令返回 dummy RES_OK (buff 不动),
		 * f_mkfs 会读到未初始化值; 这里给确定的 1 扇区对齐 */
		*(uint32_t *)buff = 1;
		return RES_OK;
	}
	return __real_SDMMC_ioctl(cmd, buff);
}

/* ---------- 修复2: HIBERNATION 保持 32K 校准 ---------- */
static uint8_t s_keep_32k_calib = 0;

void sdk_keep_32k_calib(int on)
{
	s_keep_32k_calib = (uint8_t)(on ? 1 : 0);
}

uint32_t __wrap_HAL_PRCM_DisableInter32KCalib(void)
{
	if (s_keep_32k_calib)
		return 0;   /* 保持校准, 跳过关闭 */
	return __real_HAL_PRCM_DisableInter32KCalib();
}
