/*
 * fs_ctrl_format.c — SD 卡格式化 (重建 FAT32 卷)
 *
 * 从 SDK project/common/framework/fs_ctrl.c 解耦而来的工程级扩展:
 * 与 fs_ctrl_unmount 不同, 这里【只摘除 FatFs 挂载】而不删除 mmc_card:
 * f_mkfs 的 diskio 层 (SDMMC_*) 需要卡对象存在, 删卡后 f_mkfs 会因
 * "card not exit" 拿不到任何扇区而返回 FR_NOT_READY (3)。
 *
 * 依赖 SDK fs_ctrl.c 暴露的最小扩展点 (fs_ctrl_get_fs/set_fs/lock/unlock),
 * 因此 SDK 侧不需要包含任何本工程业务代码。
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "sys/xr_debug.h"
#include "common/framework/fs_ctrl.h"
#include "fs/fatfs/ff.h"
#include "driver/chip/sdmmc/hal_sdhost.h"
#include "driver/chip/sdmmc/sdmmc.h"
#include "fs_ctrl_format.h"

#define FS_ERR(fmt, arg...)                         \
	do {                                            \
		printf("[FS ERR] %s():%d, "fmt,             \
		       __func__, __LINE__, ##arg);          \
	} while (0)

static int ensure_sd_card(void)
{
	struct mmc_card *card;
	SDCard_InitTypeDef card_param = { 0 };

	/* 卡对象已存在: 确认 present (必要时 rescan) */
	card = mmc_card_open(0);
	if (card != NULL) {
		if (!mmc_card_present(card)) {
			mmc_rescan(card, 0);
		}
		mmc_card_close(0);
		return 0;
	}

	/* 卡对象不存在 (mount 失败路径会删卡): 重建 + 扫卡, 与 fs_ctrl_mount 同参数 */
	card_param.debug_mask = ROM_WRN_MASK | ROM_ERR_MASK | ROM_ANY_MASK;
	card_param.type = MMC_TYPE_SD;
	if (mmc_card_create(0, &card_param) != 0) {
		FS_ERR("mmc create fail\n");
		return -1;
	}
	card = mmc_card_open(0);
	if (card == NULL) {
		FS_ERR("mmc open fail\n");
		mmc_card_delete(0);
		return -1;
	}
	if (!mmc_card_present(card)) {
		if (mmc_rescan(card, 0) != 0) {
			FS_ERR("mmc scan fail\n");
			mmc_card_close(0);
			mmc_card_delete(0);
			return -1;
		}
	}
	mmc_card_close(0);
	return 0;
}

/*
 * @brief Format the SD card: rebuild a FAT32 volume.
 * @return 0 on success, -1 on failure
 */
int fs_ctrl_format(void)
{
	FRESULT fr;
	FATFS *fs;
	static uint8_t mkfs_work[4096];   /* f_mkfs 工作缓冲 (>= 扇区大小) */
	int ret = -1;

	fs_ctrl_lock();

	if (fs_ctrl_get_fs() != NULL) {
		if (f_mount(NULL, "", 0) != FR_OK) {
			FS_ERR("unmount fail\n");
			goto out;
		}
		free(fs_ctrl_get_fs());
		fs_ctrl_set_fs(NULL);
	}

	if (ensure_sd_card() != 0) {
		FS_ERR("no usable sd card\n");
		goto out;
	}

	fr = f_mkfs("0:", FM_FAT32 | FM_SFD, 0, mkfs_work, sizeof(mkfs_work));
	if (fr != FR_OK) {
		FS_ERR("mkfs fail, err %d\n", fr);
		goto out;
	}

	fs = malloc(sizeof(FATFS));
	if (fs == NULL) {
		FS_ERR("no mem\n");
		goto out;
	}
	fr = f_mount(fs, "", 0);
	if (fr != FR_OK) {
		FS_ERR("mount fail, err %d\n", fr);
		free(fs);
		goto out;
	}
	fs_ctrl_set_fs(fs);
	ret = 0;

out:
	fs_ctrl_unlock();
	return ret;
}
