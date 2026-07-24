/**
 * @file settings_storage.h
 * @brief 基于INI格式的持久化存储模块
 * 
 * 存储路径: SD卡 /settings.ini
 * 格式示例:
 *   [bookmark:/政治的人生_XR872DL.epub]
 *   chapter=3
 *   offset=12345
 *   
 *   [wifi]
 *   ssid=MyNetwork
 *   passwd=MyPassword
 * 
 * 特性:
 * - 先写临时文件再rename，防断电损坏
 * - 多本书各自独立的bookmark section
 * - 通用key-value接口，可扩展存储wifi密码等
 */

#ifndef SETTINGS_STORAGE_H
#define SETTINGS_STORAGE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* INI文件最大尺寸 (字节) — 足够存几十本书的书签 */
#define SETTINGS_MAX_FILE_SIZE  4096

/* 路径最大长度 */
#define SETTINGS_MAX_PATH       512

/* section/key最大长度 */
#define SETTINGS_MAX_SECTION    256
#define SETTINGS_MAX_KEY        64
#define SETTINGS_MAX_VALUE      256

/**
 * @brief 保存EPUB阅读位置
 * @param filepath EPUB文件完整路径 (如 "/政治的人生_XR872DL.epub")
 * @param chapter 章节索引 (从0开始)
 * @param offset 章节内偏移量 (解码后文本的byte offset)
 * @param pct 全书阅读百分比 (0-100，阅读器渲染时按章节+章内进度估算)
 * @return 0=成功, -1=失败
 */
int settings_save_bookmark(const char *filepath, int chapter, int offset, int pct);

/**
 * @brief 加载EPUB阅读位置
 * @param filepath EPUB文件完整路径
 * @param chapter_out 输出: 章节索引
 * @param offset_out 输出: 章节内偏移量
 * @return 0=找到书签, -1=无书签或文件不存在
 */
int settings_load_bookmark(const char *filepath, int *chapter_out, int *offset_out);

/**
 * @brief 加载全书阅读百分比 (书架显示进度用)
 * @param filepath EPUB文件完整路径
 * @return 0-100 百分比；找不到书签或无 pct 字段返回 0
 */
int settings_load_bookmark_pct(const char *filepath);

/**
 * @brief 通用: 保存字符串键值
 * @param section section名 (不含方括号)
 * @param key 键名
 * @param value 值
 * @return 0=成功, -1=失败
 */
int settings_set_string(const char *section, const char *key, const char *value);

/**
 * @brief 通用: 读取字符串键值
 * @param section section名 (不含方括号)
 * @param key 键名
 * @param value_out 输出缓冲区
 * @param value_size 缓冲区大小
 * @return 0=成功, -1=未找到
 */
int settings_get_string(const char *section, const char *key, char *value_out, int value_size);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_STORAGE_H */