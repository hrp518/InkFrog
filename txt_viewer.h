/**
 * @file txt_viewer.h
 * @brief TXT 阅读器 LVGL 显示界面（与 epub_viewer 同构的分页阅读体验）
 *
 * 功能：
 * - 分页排版渲染（自定义 TTF 字体，复用 file_manager 字体子系统）
 * - 5 挡字号调节（工具栏）
 * - 左右分区翻页 + 顶部弹出工具栏（触摸逻辑与 epub_viewer 一致）
 * - 百分比跳转键盘
 * - 自动书签（复用 settings_storage，按原始文件字节偏移记录）
 * - GBK/UTF-8 自动识别与转码（见 gbk.h）
 * - 流式窗口读取，不限文件大小
 *
 * 与 epub_viewer 的关键差异：
 *   偏移空间 = 原始文件字节（不是解码后偏移），百分比/跳转/书签全部
 *   直接基于文件大小，跨 UTF-8/GBK 编码统一、精确。
 *
 * 解耦：
 *   字体   -> file_manager.c (get_reader_font* / file_manager_set_reader_font_size)
 *   书签   -> settings_storage.c (settings_save/load_bookmark)
 *   编码   -> gbk.c
 *   该模块不依赖 epub_viewer.c。
 */

#ifndef TXT_VIEWER_H
#define TXT_VIEWER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*====================
 *   TxtViewer 句柄
 *====================*/

typedef struct TxtViewer TxtViewer;

/* 关闭回调 - 用于返回文件管理器/书架（与 epub_viewer_set_close_cb 同形） */
typedef void (*txt_close_callback_t)(void);

/*====================
 *   函数接口
 *====================*/

/**
 * @brief 创建 TXT 阅读器视图并打开文件（检测编码、获取文件大小）
 * @param filepath FatFs 路径（如 "/Inkbook/三体.txt"）
 * @return TxtViewer* 句柄，失败返回 NULL
 */
TxtViewer* txt_viewer_create(const char *filepath);

/**
 * @brief 显示阅读器界面（构建 screen / content_container / toolbar）
 */
void txt_viewer_show(TxtViewer *viewer);

/**
 * @brief 关闭阅读器界面（销毁 LVGL 对象，触发 close_cb）
 */
void txt_viewer_close(TxtViewer *viewer);

/**
 * @brief 销毁视图，释放句柄
 */
void txt_viewer_destroy(TxtViewer *viewer);

/**
 * @brief 跳转到原始字节偏移（用于恢复书签 / 百分比跳转）
 *
 * offset 会被规范化到字符边界（向前到下一个 \n 后）。
 */
void txt_viewer_goto_offset(TxtViewer *viewer, int raw_offset);

/**
 * @brief 重新渲染当前页（字号切换后调用）
 */
void txt_viewer_refresh(TxtViewer *viewer);

/**
 * @brief 设置关闭回调
 */
void txt_viewer_set_close_cb(TxtViewer *viewer, txt_close_callback_t cb);

/**
 * @brief 获取当前阅读偏移量（原始文件字节）
 */
int txt_viewer_get_read_offset(TxtViewer *viewer);

/**
 * @brief 计算阅读百分比 (0-100) = read_offset / 文件大小 * 100
 */
int txt_viewer_get_overall_pct(TxtViewer *viewer);

#ifdef __cplusplus
}
#endif

#endif /* TXT_VIEWER_H */
