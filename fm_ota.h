/**
 * @file fm_ota.h
 * @brief 文件管理器触发的本地固件 OTA（整包 AWIH / xr_system.img）
 */
#ifndef FM_OTA_H
#define FM_OTA_H

#ifdef __cplusplus
extern "C" {
#endif

/** 扩展名为 .bin / .img（大小写不敏感）时返回 1 */
int fm_ota_is_firmware_file(const char *filename);

/**
 * 启动 OTA：校验包头后开后台线程刷入待命区。
 * @param filepath FatFs 路径（支持 //Font/...、0:/... 等）
 * @return 0 已启动后台任务；<0 预检失败（未写 Flash）
 */
int fm_ota_start(const char *filepath);

/** 是否正在 OTA */
int fm_ota_is_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* FM_OTA_H */
