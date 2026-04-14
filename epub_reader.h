/**
 * @file epub_reader.h
 * @brief EPUB文件解析器 - 使用标准miniz库(mz_zip_reader)
 *
 * 功能：
 * - 解析EPUB文件结构（ZIP压缩包）
 * - 使用miniz的mz_zip_reader进行ZIP解析
 * - 读取container.xml获取content.opf路径
 * - 解析content.opf获取元数据
 *
 * 内存优化：
 * - 使用XIP技术，代码放在XIP区域执行
 * - 使用PSRAM作为章节缓冲区
 * - 按需加载章节内容
 */

#ifndef EPUB_READER_H
#define EPUB_READER_H

#include <stdint.h>
#include <stdbool.h>

/* 包含FatFs头文件以获取FIL定义 */
#include "fs/fatfs/ff.h"

/* 包含miniz.h以获取mz_zip_archive定义 */
#include "third_party/miniz/miniz.h"

#ifdef __cplusplus
extern "C" {
#endif

/*====================
 *   常量定义
 *====================*/

#define EPUB_MAX_TITLE_LEN     128
#define EPUB_MAX_PATH_LEN      256
#define EPUB_MAX_SPINE_COUNT   8       /* 减少预分配以节省heap空间 */
#define EPUB_MAX_TOC_COUNT     4       /* 减少预分配以节省heap空间 */

/*====================
 *   数据结构
 *====================*/

/* EPUB书籍信息 */
typedef struct {
    char title[EPUB_MAX_TITLE_LEN];
    char author[128];
    char cover_image[256];
    char toc_ncx[256];
    char base_path[256];
    char file_path[512];
} EpubBook;

/* Spine条目（阅读顺序） */
typedef struct {
    char id[64];
    char href[256];
} EpubSpineItem;

/* 目录条目 */
typedef struct {
    char title[128];
    char href[256];
    int  spine_index;
} EpubTocEntry;

/* EPUB阅读器句柄 */
typedef struct {
    EpubBook book;
    EpubSpineItem spine[EPUB_MAX_SPINE_COUNT];
    int spine_count;
    EpubTocEntry toc[EPUB_MAX_TOC_COUNT];
    int toc_count;
    bool loaded;
    char *chapter_buffer;
    int chapter_buffer_size;
    
    /* === 核心修改部分 === */
    FIL archive_fp;              /* FatFs 文件指针：保持EPUB文件在SD卡上处于打开状态 */
    mz_zip_archive zip_archive;  /* miniz ZIP读取器上下文 */
} EpubReader;

/*====================
 *   函数接口
 *====================*/

EpubReader* epub_reader_create(void);
bool epub_reader_open(EpubReader *reader, const char *filepath);
void epub_reader_close(EpubReader *reader);
void epub_reader_destroy(EpubReader *reader);
const char* epub_reader_get_title(EpubReader *reader);
int epub_reader_get_chapter_count(EpubReader *reader);
int epub_reader_get_toc_count(EpubReader *reader);
EpubTocEntry* epub_reader_get_toc(EpubReader *reader, int index);
int epub_reader_read_chapter(EpubReader *reader, int chapter_index, char *buffer, int buffer_size);
int epub_reader_jump_to_toc(EpubReader *reader, int toc_index);

/* 流式解析支持 - 将章节解压到SD卡临时文件 */
int epub_reader_extract_chapter_to_file(EpubReader *reader, int chapter_index, const char *temp_file_path);

#ifdef __cplusplus
}
#endif

#endif /* EPUB_READER_H */
