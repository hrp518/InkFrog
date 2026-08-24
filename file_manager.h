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

/* 阅读器正文字号默认值（与 epub_viewer 挡位表一致） */
#define READER_FONT_SIZE_DEFAULT  18

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
int file_manager_prepare_reader_fonts(void);

/**
 * @brief 切换阅读器字体大小
 * @param new_size 新的字体大小(像素高度)，如 14/16/18/20/22
 * @return 0 成功，-1 失败
 */
int file_manager_set_reader_font_size(int new_size);

/**
 * @brief 获取当前阅读器字体大小
 * @return 当前字体大小(像素高度)
 */
int file_manager_get_reader_font_size(void);

void file_manager_print_memory_stats(const char *tag);

void epd_disable_all_animations_recursive(lv_obj_t *obj);

/**
 * @brief 打开 EPUB（复用 FM 阅读器管道）
 * @param filepath FatFs 路径
 * @param on_ui_return 关闭阅读器后的 UI 回调；NULL 则返回文件管理器
 */
void file_manager_open_epub(const char *filepath, void (*on_ui_return)(void));

/**
 * @brief 打开 TXT 简易查看器
 * @param filepath FatFs 路径
 * @param on_ui_return 点「返回」后的回调；NULL 则返回文件管理器屏幕
 */
void file_manager_open_txt(const char *filepath, void (*on_ui_return)(void));

/**
 * @brief 从阅读器返回时的统一清理入口（书架/FM 共用）
 *        通常由 EPUB close_cb 触发，外部也可在需要时调用。
 */
void file_manager_show(void);

/**
 * @brief 把物理返回键处理交回 FM 处理器
 *        书架打开阅读器后调用, 使阅读中按物理返回能正确回到来源界面。
 */
void fm_assign_physical_back(void);

#ifdef __cplusplus
}
#endif

#endif /* FILE_MANAGER_H */
