#ifndef _FS_CTRL_FORMAT_H_
#define _FS_CTRL_FORMAT_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * @brief Format the SD card: unmount FatFs (keep mmc_card), rebuild FAT32, remount.
 * @return 0 on success, -1 on failure
 */
int fs_ctrl_format(void);

#ifdef __cplusplus
}
#endif

#endif /* _FS_CTRL_FORMAT_H_ */
