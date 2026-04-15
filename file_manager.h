/**
 * @file file_manager.h
 * @brief 文件管理器 - LVGL文件系统GUI组件
 *
 * 功能：
 * - 目录浏览（基于lv_list）
 * - 文件操作（打开文本、重命名、删除）
 * - 中文UTF-8支持
 *
 * 硬件：240x415 单色墨水屏 (EPD_3IN52)
 */

#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化并启动文件管理器
 * 
 * 调用此函数将创建一个全屏文件管理器界面，
 * 用户可以浏览SD卡上的文件并进行操作。
 */
void file_manager_init(void);

/**
 * @brief 关闭文件管理器并清理资源
 * 
 * 通常不需要手动调用，文件管理器关闭时会自动清理。
 */
void file_manager_close(void);

/**
 * @brief 获取当前字体（支持从SD卡自动加载TTF字体）
 * 
 * 该函数会自动扫描SD卡Font目录下的.ttf文件，
 * 并使用最小的文件作为字体。如果没有找到TTF文件，
 * 则使用编译内置的lv_font_misans_16作为回退。
 * 
 * @return 字体指针
 */
lv_font_t * get_reader_font(void);
lv_font_t * get_reader_font_h1(void);
lv_font_t * get_reader_font_h2(void);
lv_font_t * get_reader_font_h3(void);

void epd_disable_all_animations_recursive(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif

#endif /* FILE_MANAGER_H */
