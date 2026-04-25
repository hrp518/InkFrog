/**
 * @file epub_viewer.h
 * @brief EPUB阅读器LVGL显示界面
 * 
 * 功能：
 * - 章节内容显示（支持中文）
 * - 上下翻页
 * - 目录导航
 * - 进度显示
 * 
 * 字体：使用misans字体支持中文
 */

#ifndef EPUB_VIEWER_H
#define EPUB_VIEWER_H

#include "epub_reader.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*====================
 *   常量定义
 *====================*/

#define EPUB_VIEWER_BUF_SIZE    8192    /* 章节内容缓冲区大小 */
#define EPUB_VIEWER_LINE_HEIGHT 24      /* 行高（像素） */
#define EPUB_VIEWER_FONT_SIZE   16      /* 字体大小 */
#define EPUB_VIEWER_PAGE_LINES  15       /* 每页行数 */
#define EPUB_VIEWER_INDENT      32       /* 首行缩进（像素） */
#define EPUB_VIEWER_LINE_SPACE  6        /* 行间距 */

/* PSRAM 章节缓存阈值（字节，解码后字符数） */
/* <= 96KB 时全缓存；> 96KB 时流式 */
#define EPUB_CACHE_THRESHOLD     (96 * 1024)

/*====================
 *   显示模式
 *====================*/

typedef enum {
    EPUB_VIEW_MODE_TEXT,       /* 纯文本模式 */
    EPUB_VIEW_MODE_CHAPTER,    /* 章节模式 */
} EpubViewMode;

/*====================
 *   回调函数
 *====================*/

/* 章节加载完成回调 */
typedef void (*epub_chapter_loaded_cb)(int chapter_index, int total_chapters);

/* 关闭回调 - 用于返回文件管理器 */
typedef void (*epub_close_callback_t)(void);

/*====================
 *   EpubViewer句柄
 *====================*/

typedef struct EpubViewer EpubViewer;

/*====================
 *   函数接口
 *====================*/

/**
 * @brief 创建EPUB阅读器视图
 * @param reader EPUB阅读器
 * @return EpubViewer* 视图句柄，失败返回NULL
 */
EpubViewer* epub_viewer_create(EpubReader *reader);

/**
 * @brief 显示阅读器界面
 * @param viewer 视图句柄
 */
void epub_viewer_show(EpubViewer *viewer);

/**
 * @brief 关闭阅读器界面
 * @param viewer 视图句柄
 */
void epub_viewer_close(EpubViewer *viewer);

/**
 * @brief 销毁阅读器视图
 * @param viewer 视图句柄
 */
void epub_viewer_destroy(EpubViewer *viewer);

/**
 * @brief 上一页
 * @param viewer 视图句柄
 * @return true 成功切换，false 已在第一页
 */
bool epub_viewer_prev_page(EpubViewer *viewer);

/**
 * @brief 下一页
 * @param viewer 视图句柄
 * @return true 成功切换，false 已在最后一页
 */
bool epub_viewer_next_page(EpubViewer *viewer);

/**
 * @brief 跳转到指定章节
 * @param viewer 视图句柄
 * @param chapter_index 章节索引
 * @return true 成功，false 失败
 */
bool epub_viewer_goto_chapter(EpubViewer *viewer, int chapter_index);

/**
 * @brief 显示目录
 * @param viewer 视图句柄
 */
void epub_viewer_show_toc(EpubViewer *viewer);

/**
 * @brief 获取当前章节索引
 * @param viewer 视图句柄
 * @return int 章节索引
 */
int epub_viewer_get_current_chapter(EpubViewer *viewer);

/**
 * @brief 获取当前页码
 * @param viewer 视图句柄
 * @return int 页码（从1开始）
 */
int epub_viewer_get_current_page(EpubViewer *viewer);

/**
 * @brief 获取总页数
 * @param viewer 视图句柄
 * @return int 总页数
 */
int epub_viewer_get_total_pages(EpubViewer *viewer);

/**
 * @brief 设置章节加载回调
 * @param viewer 视图句柄
 * @param cb 回调函数
 */
void epub_viewer_set_chapter_loaded_cb(EpubViewer *viewer, epub_chapter_loaded_cb cb);

/**
 * @brief 设置关闭回调
 * @param viewer 视图句柄
 * @param cb 关闭回调函数（返回文件管理器时调用）
 */
void epub_viewer_set_close_cb(EpubViewer *viewer, epub_close_callback_t cb);

/*====================
 *   HTML简化处理函数
 *====================*/

/**
 * @brief 去除HTML标签，提取纯文本
 * @param html 输入HTML文本
 * @param len 输入长度
 * @param output 输出缓冲区
 * @param output_size 输出缓冲区大小
 * @return int 输出的字符数
 */
int epub_strip_html_tags(const char *html, int len, char *output, int output_size);

/**
 * @brief 处理HTML实体（如 &nbsp; < > &）
 * @param text 输入文本
 * @param output 输出缓冲区
 * @param output_size 输出缓冲区大小
 * @return int 输出的字符数
 */
int epub_decode_html_entities(const char *text, char *output, int output_size);

#ifdef __cplusplus
}
#endif

#endif /* EPUB_VIEWER_H */
