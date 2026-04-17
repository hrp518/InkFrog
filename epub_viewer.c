/**
 * @file epub_viewer.c
 * @brief EPUB阅读器LVGL显示界面实现 - 自适应双模式（缓存+流式）
 *
 * 核心改进：
 * - 根Bug修复：update_display()以前没用page_char_offsets[]做精确定位，导致
 *   第一页读章末8KB（字节偏移与字符偏移错位），翻页只有7-8行
 * - 自适应双模式：章节<=160KB全缓存（直接切片），>160KB流式（按需读文件）
 * - 充分利用PSRAM（扩大堆到384KB）
 * - 字体样式感知：H1/H2/H3正确区分（大小+粗体）
 * - HTML解析增强：pre标签保留格式、更多实体解码
 */

#include "epub_viewer.h"
#include "file_manager.h"
#include "lv_port_disp.h"
#include "lvgl/lvgl.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/dma_heap.h>

extern void epd_mark_refresh_pending(void);
extern void epd_disable_all_animations_recursive(lv_obj_t *obj);

/*====================
 *   调试开关
 *====================*/
#define VIEWER_DEBUG 1
#if VIEWER_DEBUG
#define VIEW_LOG(fmt, ...) printf("[VIEWER] " fmt, ##__VA_ARGS__)
#define VIEW_ERR(fmt, ...) printf("[VIEWER ERR] " fmt, ##__VA_ARGS__)
#else
#define VIEW_LOG(fmt, ...)
#define VIEW_ERR(fmt, ...)
#endif

/*====================
 *   内部数据结构
 *====================*/

/* 流式解析配置 */
#define EPUB_MAX_PAGES_PER_CHAPTER  1000
#define EPUB_WORK_BUF_SIZE          8192   /* 工作缓冲区：8KB */
#define EPUB_CHUNK_DECODED         16384   /* 每块解码后大小 */

/* PSRAM 缓存模式阈值（<=160KB全缓存，>160KB流式） */
#ifndef EPUB_CACHE_THRESHOLD
#define EPUB_CACHE_THRESHOLD        (160 * 1024)
#endif

struct EpubViewer {
    EpubReader *reader;
    lv_obj_t *screen;
    lv_obj_t *content_container;
    lv_obj_t *title_label;
    lv_obj_t *page_label;
    lv_obj_t *toc_list;
    lv_obj_t *loading_msg;

    /* PSRAM工作缓冲区 */
    char *html_buf;           /* 8KB - 读取HTML块 */
    char *stripped_buf;       /* 16KB - 去除标签后 */
    char *decoded_buf;        /* 16KB - 解码HTML实体后 */
    char *reflowed_buf;      /* 16KB - 文本重排后 */
    char *page_text_buf;      /* 8KB - 单页纯文本输出 */

    /* 【双模式核心】章节内容 */
    char *chapter_decoded_cache;   /* 全章解码缓存（PSRAM，可选） */
    uint32_t chapter_decoded_len;  /* 缓存字符数（解码后） */
    uint32_t chapter_uncomp_size;  /* 解压后章节总大小（字节） */
    bool use_cache_mode;           /* true=缓存模式 false=流式模式 */

    /* 流式解析核心：页码-字符偏移索引 */
    uint32_t *page_char_offsets;  /* [page_index] = decoded_char_offset */
    int max_pages;
    int total_pages;
    int current_page;
    char temp_file_path[64];

    int current_chapter;
    epub_chapter_loaded_cb chapter_cb;
};

/*====================
 *   显示配置
 *====================*/

#define SCREEN_WIDTH   240
#define SCREEN_HEIGHT  415

#define CONTENT_X      10
#define CONTENT_Y      50
#define CONTENT_WIDTH  (SCREEN_WIDTH - 20)
#define CONTENT_HEIGHT (SCREEN_HEIGHT - 121)

/* 字体配置 */
#define FONT       get_reader_font()
#define FONT_H1    get_reader_font_h1()
#define FONT_H2    get_reader_font_h2()
#define FONT_H3    get_reader_font_h3()

/* 行高 */
#define LH_BODY    22
#define LH_H3      26
#define LH_H2      30
#define LH_H1      34

#define LINE_HEIGHT     22
#define CHAR_WIDTH      8
#define CHARS_PER_LINE (CONTENT_WIDTH / CHAR_WIDTH)
#define LINES_PER_PAGE (CONTENT_HEIGHT / LINE_HEIGHT)

/*====================
 *   内部函数声明
 *====================*/

static int strip_html_tags_with_styles(const char *html, int html_len,
                                       char *output, int out_size);
static void decode_html_entities(const char *input, char *output, int out_size);
static void sanitize_utf8(char *str);
static void fix_chinese_punctuation(char *str);
static void filter_unsupported_chars(char *str);

/* 流式解析核心 */
static int build_page_index(EpubViewer *viewer, int chapter_index);
static void free_page_index(EpubViewer *viewer);
static void update_display(EpubViewer *viewer);

static void prev_page_handler(EpubViewer *viewer);
static void next_page_handler(EpubViewer *viewer);
static void toc_btn_cb(lv_event_t *e);
static void toc_item_cb(lv_event_t *e);
static void toc_btn_close_cb(lv_event_t *e);
static void close_viewer_cb(lv_event_t *e);
static void page_prev_cb(lv_event_t *e);
static void page_next_cb(lv_event_t *e);

/*====================
 *   HTML处理
 *====================*/

/* 过滤导致 LVGL 崩溃的全角中文标点符号 */
static void fix_chinese_punctuation(char *str) {
    char *p = str;
    char *out = str;
    while (*p) {
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBC) {
            if ((unsigned char)p[2] == 0x88) { *out++ = '('; p += 3; continue; }
            if ((unsigned char)p[2] == 0x89) { *out++ = ')'; p += 3; continue; }
            if ((unsigned char)p[2] == 0x81) { *out++ = '!'; p += 3; continue; }
            if ((unsigned char)p[2] == 0x9A) { *out++ = ':'; p += 3; continue; }
            if ((unsigned char)p[2] == 0x9B) { *out++ = ';'; p += 3; continue; }
            if ((unsigned char)p[2] == 0x8C) { *out++ = ','; p += 3; continue; }
            if ((unsigned char)p[2] == 0x9F) { *out++ = '?'; p += 3; continue; }
        }
        if ((unsigned char)p[0] == 0xE3 && (unsigned char)p[1] == 0x80) {
            if ((unsigned char)p[2] == 0x82) { *out++ = '.'; p += 3; continue; }
            if ((unsigned char)p[2] == 0x81) { *out++ = ','; p += 3; continue; }
        }
        if ((unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x80) {
            if ((unsigned char)p[2] == 0x9C || (unsigned char)p[2] == 0x9D) { *out++ = '"'; p += 3; continue; }
            if ((unsigned char)p[2] == 0x98 || (unsigned char)p[2] == 0x99) { *out++ = '\''; p+= 3; continue; }
            if ((unsigned char)p[2] == 0x94) { *out++ = '-'; p += 3; continue; }
        }
        *out++ = *p++;
    }
    *out = '\0';
}

/* 清理可能被截断的尾部无效 UTF-8 字符 */
static void sanitize_utf8(char *str) {
    int len = strlen(str);
    if (len == 0) return;
    int i = len - 1;
    while (i >= 0 && (str[i] & 0xC0) == 0x80) {
        i--;
    }
    if (i >= 0) {
        int expected = 1;
        if ((str[i] & 0xE0) == 0xC0) expected = 2;
        else if ((str[i] & 0xF0) == 0xE0) expected = 3;
        else if ((str[i] & 0xF8) == 0xF0) expected = 4;
        if (len - i < expected) {
            str[i] = '\0';
        }
    }
}

/* 过滤不在字库范围内的字符，防止LVGL索引崩溃 */
static void filter_unsupported_chars(char *str) {
    unsigned char *read_ptr = (unsigned char *)str;
    unsigned char *write_ptr = (unsigned char *)str;

    while (*read_ptr) {
        uint32_t unicode = 0;
        int char_len = 0;

        if (read_ptr[0] < 0x80) {
            unicode = read_ptr[0]; char_len = 1;
        } else if ((read_ptr[0] & 0xE0) == 0xC0 && read_ptr[1]) {
            unicode = ((read_ptr[0] & 0x1F) << 6) | (read_ptr[1] & 0x3F); char_len = 2;
        } else if ((read_ptr[0] & 0xF0) == 0xE0 && read_ptr[1] && read_ptr[2]) {
            unicode = ((read_ptr[0] & 0x0F) << 12) | ((read_ptr[1] & 0x3F) << 6) | (read_ptr[2] & 0x3F); char_len = 3;
        } else if ((read_ptr[0] & 0xF8) == 0xF0 && read_ptr[1] && read_ptr[2] && read_ptr[3]) {
            unicode = ((read_ptr[0] & 0x07) << 18) | ((read_ptr[1] & 0x3F) << 12) | ((read_ptr[2] & 0x3F) << 6) | (read_ptr[3] & 0x3F); char_len = 4;
        } else {
            read_ptr++;
            continue;
        }

        if (unicode == 0x0A || (unicode >= 0x20 && unicode <= 0x7E) ||
            (unicode >= 0x4E00 && unicode <= 0x9FA5) ||
            (unicode >= 0x3000 && unicode <= 0x303F) /* CJK标点 */) {
            for (int i = 0; i < char_len; i++) *write_ptr++ = read_ptr[i];
        } else {
            if (unicode == 0xFF08 || unicode == 0x3014) *write_ptr++ = '(';
            else if (unicode == 0xFF09 || unicode == 0x3015) *write_ptr++ = ')';
            else if (unicode == 0xFF01) *write_ptr++ = '!';
            else if (unicode == 0xFF1A) *write_ptr++ = ':';
            else if (unicode == 0xFF1B) *write_ptr++ = ';';
            else if (unicode == 0xFF0C || unicode == 0x3001) *write_ptr++ = ',';
            else if (unicode == 0xFF1F) *write_ptr++ = '?';
            else if (unicode == 0x3002) *write_ptr++ = '.';
            else if (unicode == 0x201C || unicode == 0x201D) *write_ptr++ = '"';
            else if (unicode == 0x2018 || unicode == 0x2019) *write_ptr++ = '\'';
            else if (unicode == 0x2014 || unicode == 0x2015) *write_ptr++ = '-';
            else if (unicode == 0x300A) *write_ptr++ = '<';
            else if (unicode == 0x300B) *write_ptr++ = '>';
            else *write_ptr++ = ' ';
        }
        read_ptr += char_len;
    }
    *write_ptr = '\0';
}

/**
 * 样式感知HTML标签剥离
 * 识别 <h1>/<h2>/<h3> 并插入样式标记
 * 格式: \x02LEVEL\x03  LEVEL=0正文 1=H1 2=H2 3=H3 4=段落首行
 *
 * 增强：处理<pre>标签（保留格式）、<code>标签
 */
static int strip_html_tags_with_styles(const char *html, int html_len,
                                       char *output, int out_size)
{
    int out_pos = 0;
    int in_tag = 0;
    int in_pre = 0;      /* <pre>标签内：保留原始换行 */
    const char *tag_start = NULL;
    int current_level = 0;
    const char *p = html;
    const char *end = html + html_len;

    /* 跳过起始不完整UTF-8字符 */
    while (p < end) {
        unsigned char uc = (unsigned char)*p;
        int need = 0;
        if ((uc & 0xE0) == 0xC0) need = 1;
        else if ((uc & 0xF0) == 0xE0) need = 2;
        else if ((uc & 0xF8) == 0xF0) need = 3;
        else break;
        if (p + need >= end) { p++; }
        else break;
    }

    #define EMIT_LEVEL(lvl) do { \
        if (out_pos < out_size - 4) { \
            output[out_pos++] = '\x02'; \
            output[out_pos++] = '0' + (lvl); \
            output[out_pos++] = '\x03'; \
        } \
    } while(0)

    while (p < end && out_pos < out_size - 1) {
        if (*p == '<') {
            in_tag = 1;
            tag_start = p;

            /* 检测pre/code标签（保留原始格式） */
            if (strncasecmp(p + 1, "pre", 3) == 0 ||
                strncasecmp(p + 1, "code", 4) == 0) {
                in_pre = 1;
            }

        } else if (*p == '>' && in_tag) {
            in_tag = 0;

            /* 检测闭合pre/code */
            if (tag_start && strncasecmp(tag_start + 1, "/pre", 4) == 0) {
                in_pre = 0;
            }

            int len = (int)(p - tag_start + 1);

            if (len > 2 && tag_start[1] == '/') {
                /* 闭合标签 */
                if (strncasecmp(tag_start + 2, "h1", 2) == 0 ||
                    strncasecmp(tag_start + 2, "h2", 2) == 0 ||
                    strncasecmp(tag_start + 2, "h3", 2) == 0) {
                    if (current_level > 0) {
                        EMIT_LEVEL(0);
                        current_level = 0;
                        if (out_pos < out_size - 1) output[out_pos++] = '\n';
                    }
                }
            } else {
                /* 开始标签 */
                if (strncasecmp(tag_start + 1, "h1", 2) == 0 &&
                    (tag_start[3] == '>' || tag_start[3] == ' ')) {
                    if (current_level != 1) {
                        if (out_pos > 0 && output[out_pos-1] != '\n') {
                            if (out_pos < out_size - 1) output[out_pos++] = '\n';
                        }
                        EMIT_LEVEL(1);
                        current_level = 1;
                    }
                } else if (strncasecmp(tag_start + 1, "h2", 2) == 0) {
                    if (current_level != 2) {
                        if (out_pos > 0 && output[out_pos-1] != '\n') {
                            if (out_pos < out_size - 1) output[out_pos++] = '\n';
                        }
                        EMIT_LEVEL(2);
                        current_level = 2;
                    }
                } else if (strncasecmp(tag_start + 1, "h3", 2) == 0) {
                    if (current_level != 3) {
                        if (out_pos > 0 && output[out_pos-1] != '\n') {
                            if (out_pos < out_size - 1) output[out_pos++] = '\n';
                        }
                        EMIT_LEVEL(3);
                        current_level = 3;
                    }
                } else if (strncasecmp(tag_start + 1, "p", 1) == 0 ||
                           strncasecmp(tag_start + 1, "div", 3) == 0 ||
                           strncasecmp(tag_start + 1, "br", 2) == 0) {
                    if (out_pos > 0 && output[out_pos-1] != '\n') {
                        if (out_pos < out_size - 1) output[out_pos++] = '\n';
                    }
                }
            }
        } else if (!in_tag) {
            if (*p == '&') {
                if (out_pos < out_size - 1) output[out_pos++] = *p;
            } else if (*p == '\n') {
                /* 在pre标签外：换行分隔段落 */
                if (!in_pre) {
                    if (out_pos > 0 && output[out_pos-1] != '\n') {
                        if (out_pos < out_size - 1) output[out_pos++] = '\n';
                    }
                } else {
                    /* 在pre标签内：保留原始换行 */
                    if (out_pos < out_size - 1) output[out_pos++] = '\n';
                }
            } else if (*p != '\r' && *p != '\t') {
                if (out_pos < out_size - 1) output[out_pos++] = *p;
            }
        }
        p++;
    }

    #undef EMIT_LEVEL

    if (out_pos > 0 && output[out_pos-1] != '\n' && out_pos < out_size - 1) {
        output[out_pos++] = '\n';
    }
    output[out_pos] = '\0';
    return out_pos;
}

/* 解码HTML实体（增强版：支持更多实体） */
static void decode_html_entities(const char *input, char *output, int out_size) {
    int out_pos = 0;
    const char *p = input;

    while (*p && out_pos < out_size - 1) {
        if (*p == '&') {
            /* 常见实体 */
            if (p[1] == 'n' && p[2] == 'b' && p[3] == 's' && p[4] == 'p' && p[5] == ';') {
                output[out_pos++] = ' '; p += 6;
            } else if (p[1] == 'l' && p[2] == 't' && p[3] == ';') {
                output[out_pos++] = '<'; p += 4;
            } else if (p[1] == 'g' && p[2] == 't' && p[3] == ';') {
                output[out_pos++] = '>'; p += 4;
            } else if (p[1] == 'a' && p[2] == 'm' && p[3] == 'p' && p[4] == ';') {
                output[out_pos++] = '&'; p += 5;
            } else if (p[1] == 'q' && p[2] == 'u' && p[3] == 'o' && p[4] == 't' && p[5] == ';') {
                output[out_pos++] = '"'; p += 6;
            } else if (p[1] == 'a' && p[2] == 'p' && p[3] == 'o' && p[4] == 's' && p[5] == ';') {
                output[out_pos++] = '\''; p += 6;
            } else if (p[1] == '#' && p[2] >= '0' && p[2] <= '9') {
                /* HTML数字实体 */
                p += 2; int code = 0;
                while (*p >= '0' && *p <= '9') { code = code * 10 + (*p - '0'); p++; }
                if (*p == ';') p++;
                if (code > 0 && code < 0xFFFF) {
                    if (code < 128) {
                        output[out_pos++] = (char)code;
                    } else if (code >= 0x4E00 && code <= 0x9FFF) {
                        output[out_pos++] = (char)(0xE0 | (code >> 12));
                        output[out_pos++] = (char)(0x80 | ((code >> 6) & 0x3F));
                        output[out_pos++] = (char)(0x80 | (code & 0x3F));
                    }
                }
            } else {
                output[out_pos++] = *p++;
            }
        } else {
            output[out_pos++] = *p++;
        }
    }
    output[out_pos] = '\0';
}

/*====================
 *   流式解析核心
 *====================*/

static void free_page_index(EpubViewer *viewer) {
    if (viewer->page_char_offsets) {
        _dma_free(viewer->page_char_offsets, 0);
        viewer->page_char_offsets = NULL;
    }
    if (viewer->chapter_decoded_cache) {
        _dma_free(viewer->chapter_decoded_cache, 0);
        viewer->chapter_decoded_cache = NULL;
    }
    viewer->max_pages = 0;
    viewer->total_pages = 0;
    viewer->use_cache_mode = false;
    viewer->chapter_decoded_len = 0;
}

/**
 * @brief 构建页码索引（自适应双模式）
 *
 * 1. 解压章节到临时文件（记录解压后总大小）
 * 2. 判断章节大小决定缓存模式：
 *    - <= EPUB_CACHE_THRESHOLD → 缓存模式：全量解码到 chapter_decoded_cache
 *    - >  EPUB_CACHE_THRESHOLD → 流式模式：流式读取建索引
 * 3. 构建 page_char_offsets[]（统一使用"解码后字符偏移"）
 *
 * @return 0成功，-1失败
 */
static int build_page_index(EpubViewer *viewer, int chapter_index) {
    if (!viewer || !viewer->reader) return -1;

    VIEW_LOG("Building page index for chapter %d...\n", chapter_index);
    free_page_index(viewer);
    strncpy(viewer->temp_file_path, "0:/epub_temp.html", sizeof(viewer->temp_file_path) - 1);

    /* 解压章节到临时文件，获取解压后总大小 */
    uint32_t uncomp_size = epub_reader_extract_chapter_to_file(
        viewer->reader, chapter_index, viewer->temp_file_path);
    if ((int)uncomp_size < 0) {
        VIEW_ERR("Failed to extract chapter\n");
        return -1;
    }
    viewer->chapter_uncomp_size = uncomp_size;
    VIEW_LOG("Chapter uncompressed size: %u bytes, threshold=%d\n",
             uncomp_size, EPUB_CACHE_THRESHOLD);

    /* 判断模式：<=160KB用缓存，>160KB用流式 */
    viewer->use_cache_mode = (uncomp_size <= EPUB_CACHE_THRESHOLD);

    viewer->max_pages = EPUB_MAX_PAGES_PER_CHAPTER;
    viewer->page_char_offsets = (uint32_t*)_dma_malloc(
        viewer->max_pages * sizeof(uint32_t), DMAHEAP_PSRAM);
    if (!viewer->page_char_offsets) return -1;
    memset(viewer->page_char_offsets, 0, viewer->max_pages * sizeof(uint32_t));

    FIL temp_fp;
    if (f_open(&temp_fp, viewer->temp_file_path, FA_READ) != FR_OK) return -1;

    uint32_t decoded_char_count = 0;
    int page_count = 1;
    viewer->page_char_offsets[0] = 0;

    /* ========== 缓存模式：全量解码到 PSRAM ========== */
    if (viewer->use_cache_mode) {
        /* 分配章节解码缓存（对齐到4KB） */
        uint32_t cache_size = (uncomp_size + 4095) & ~4095UL;
        if (cache_size < 4096) cache_size = 4096;
        /* 确保不超过阈值 + 一点余量 */
        if (cache_size > EPUB_CACHE_THRESHOLD + 4096) cache_size = EPUB_CACHE_THRESHOLD + 4096;

        viewer->chapter_decoded_cache = (char*)_dma_malloc(cache_size, DMAHEAP_PSRAM);
        if (!viewer->chapter_decoded_cache) {
            VIEW_ERR("Cache alloc failed (%u bytes), falling back to streaming\n", cache_size);
            viewer->use_cache_mode = false;
        } else {
            viewer->chapter_decoded_cache[0] = '\0';
            VIEW_LOG("Using CACHE mode, cache_size=%u\n", cache_size);
        }
    }

    /* ========== 流式遍历：建 page_char_offsets[] ========== */
    int y_offset = 0;
    int current_level = 0;
    int current_lh = LH_BODY;
    lv_font_t *current_font = FONT;
    char *cache_ptr = viewer->chapter_decoded_cache; /* 缓存写入位置 */
    uint32_t cache_left = viewer->use_cache_mode ? EPUB_CACHE_THRESHOLD : 0;

    while (1) {
        UINT br = 0;
        f_read(&temp_fp, viewer->html_buf, EPUB_WORK_BUF_SIZE, &br);
        if (br == 0) break;

        /* 解析HTML并去标签 */
        memset(viewer->stripped_buf, 0, EPUB_WORK_BUF_SIZE * 2);
        int stripped_len = strip_html_tags_with_styles(
            viewer->html_buf, (int)br, viewer->stripped_buf, EPUB_WORK_BUF_SIZE * 2);

        /* 解码HTML实体 */
        memset(viewer->decoded_buf, 0, EPUB_WORK_BUF_SIZE * 2);
        decode_html_entities(viewer->stripped_buf, viewer->decoded_buf, EPUB_WORK_BUF_SIZE * 2);
        sanitize_utf8(viewer->decoded_buf);
        fix_chinese_punctuation(viewer->decoded_buf);

        /* 缓存模式：将解码后内容追加到缓存 */
        if (viewer->use_cache_mode && viewer->chapter_decoded_cache) {
            uint32_t decoded_len = strlen(viewer->decoded_buf);
            if (decoded_len > 0 && cache_left > decoded_len + 1) {
                memcpy(cache_ptr, viewer->decoded_buf, decoded_len);
                cache_ptr[decoded_len] = '\0';
                cache_ptr += decoded_len;
                cache_left -= decoded_len;
            }
        }

        /* 边遍历边建页码索引（分页判断） */
        const char *p = viewer->decoded_buf;
        const char *block_start = viewer->decoded_buf;

        while (*p) {
            /* 检测样式标记 \x02N\x03 */
            if ((unsigned char)*p == 0x02 && *(p+1) >= '0' && *(p+1) <= '3' &&
                (unsigned char)*(p+2) == 0x03) {
                if (p > block_start) {
                    int block_len = (int)(p - block_start);
                    while (block_len > 0 && (block_start[block_len-1] == '\n' || block_start[block_len-1] == ' '))
                        block_len--;
                    if (block_len > 0) {
                        memcpy(viewer->reflowed_buf, block_start, block_len);
                        viewer->reflowed_buf[block_len] = '\0';

                        lv_point_t txt_size;
                        lv_txt_get_size(&txt_size, viewer->reflowed_buf, current_font, 0, 0,
                                       CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
                        int actual_h = txt_size.y;
                        if (actual_h < current_lh) actual_h = current_lh;

                        y_offset += actual_h;
                        decoded_char_count += block_len;

                        if (y_offset >= CONTENT_HEIGHT && page_count < viewer->max_pages) {
                            viewer->page_char_offsets[page_count] = decoded_char_count - block_len;
                            page_count++;
                            y_offset = actual_h;
                        }
                    }
                }
                current_level = *(p+1) - '0';
                p += 3;
                block_start = p;
                switch (current_level) {
                    case 1: current_font = FONT_H1; current_lh = LH_H1; break;
                    case 2: current_font = FONT_H2; current_lh = LH_H2; break;
                    case 3: current_font = FONT_H3; current_lh = LH_H3; break;
                    default: current_font = FONT; current_lh = LH_BODY; break;
                }
            } else if (*p == '\n') {
                if (p > block_start) {
                    int block_len = (int)(p - block_start);
                    while (block_len > 0 && (block_start[block_len-1] == ' ')) block_len--;
                    if (block_len > 0) {
                        memcpy(viewer->reflowed_buf, block_start, block_len);
                        viewer->reflowed_buf[block_len] = '\0';

                        lv_point_t txt_size;
                        lv_txt_get_size(&txt_size, viewer->reflowed_buf, current_font, 0, 0,
                                       CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
                        int actual_h = txt_size.y;
                        if (actual_h < current_lh) actual_h = current_lh;

                        y_offset += actual_h;
                        decoded_char_count += block_len;

                        if (y_offset >= CONTENT_HEIGHT && page_count < viewer->max_pages) {
                            viewer->page_char_offsets[page_count] = decoded_char_count - block_len;
                            page_count++;
                            y_offset = actual_h;
                        }
                    }
                }
                p++;
                block_start = p;
                decoded_char_count++;
            } else {
                p++;
                decoded_char_count++;
            }
        }
    }
    f_close(&temp_fp);

    viewer->chapter_decoded_len = decoded_char_count;
    viewer->total_pages = page_count;
    VIEW_LOG("Page index built: %d pages (chars=%u, mode=%s)\n",
             viewer->total_pages, decoded_char_count,
             viewer->use_cache_mode ? "CACHE" : "STREAMING");
    return 0;
}

/*====================
 *   渲染显示（双模式）
 *====================*/

/* 计算指定高度需要多少解码字符 */
static uint32_t calc_chars_for_height(const char *text_start, uint32_t text_len,
                                       int target_height) {
    uint32_t used_chars = 0;
    int y_offset = 0;
    int current_level = 0;
    int current_lh = LH_BODY;
    lv_font_t *current_font = FONT;

    const char *p = text_start;
    const char *end = text_start + text_len;
    const char *block_start = text_start;
    char work_buf[256];

    while (*p && p < end) {
        if ((unsigned char)*p == 0x02 && *(p+1) >= '0' && *(p+1) <= '3' &&
            (unsigned char)*(p+2) == 0x03) {
            if (p > block_start) {
                int block_len = (int)(p - block_start);
                while (block_len > 0 && (block_start[block_len-1] == '\n' || block_start[block_len-1] == ' '))
                    block_len--;
                if (block_len > 0 && block_len < (int)sizeof(work_buf)) {
                    memcpy(work_buf, block_start, block_len);
                    work_buf[block_len] = '\0';

                    lv_point_t txt_size;
                    lv_txt_get_size(&txt_size, work_buf, current_font, 0, 0,
                                   CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
                    int actual_h = txt_size.y;
                    if (actual_h < current_lh) actual_h = current_lh;
                    y_offset += actual_h;
                    used_chars += (p - block_start);

                    if (y_offset >= target_height) return used_chars;
                }
            }
            current_level = *(p+1) - '0';
            p += 3;
            block_start = p;
            switch (current_level) {
                case 1: current_font = FONT_H1; current_lh = LH_H1; break;
                case 2: current_font = FONT_H2; current_lh = LH_H2; break;
                case 3: current_font = FONT_H3; current_lh = LH_H3; break;
                default: current_font = FONT; current_lh = LH_BODY; break;
            }
        } else if (*p == '\n') {
            if (p > block_start) {
                int block_len = (int)(p - block_start);
                while (block_len > 0 && (block_start[block_len-1] == ' ')) block_len--;
                if (block_len > 0 && block_len < (int)sizeof(work_buf)) {
                    memcpy(work_buf, block_start, block_len);
                    work_buf[block_len] = '\0';

                    lv_point_t txt_size;
                    lv_txt_get_size(&txt_size, work_buf, current_font, 0, 0,
                                   CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
                    int actual_h = txt_size.y;
                    if (actual_h < current_lh) actual_h = current_lh;
                    y_offset += actual_h;
                    used_chars += (p - block_start);

                    if (y_offset >= target_height) return used_chars;
                }
            }
            p++;
            block_start = p;
            used_chars++;
        } else {
            p++;
            used_chars++;
        }
    }

    /* 最后一个块 */
    if (p > block_start) {
        int block_len = (int)(p - block_start);
        while (block_len > 0 && (block_start[block_len-1] == '\n' || block_start[block_len-1] == ' '))
            block_len--;
        if (block_len > 0) used_chars += block_len;
    }
    return used_chars;
}

/**
 * @brief 显示当前页（自适应双模式）
 *
 * 缓存模式：从 chapter_decoded_cache 切片
 * 流式模式：从临时文件按需读取并解码
 */
static void update_display(EpubViewer *viewer) {
    VIEW_LOG("=== update_display START ===\n");
    if (!viewer || !viewer->content_container) return;
    if (!viewer->page_char_offsets || viewer->total_pages == 0) {
        lv_obj_t *err = lv_label_create(viewer->content_container);
        lv_obj_set_size(err, CONTENT_WIDTH, CONTENT_HEIGHT);
        lv_obj_align(err, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_text_font(err, FONT, 0);
        lv_obj_set_style_text_color(err, lv_color_black(), 0);
        lv_label_set_text(err, "加载中...");
        return;
    }

    if (viewer->current_page < 0) viewer->current_page = 0;
    if (viewer->current_page >= viewer->total_pages)
        viewer->current_page = viewer->total_pages - 1;

    uint32_t page_start = viewer->page_char_offsets[viewer->current_page];
    uint32_t page_end = (viewer->current_page + 1 < viewer->total_pages)
                          ? viewer->page_char_offsets[viewer->current_page + 1]
                          : viewer->chapter_decoded_len;

    VIEW_LOG("Page %d/%d: char_start=%u char_end=%u mode=%s\n",
             viewer->current_page + 1, viewer->total_pages,
             page_start, page_end,
             viewer->use_cache_mode ? "CACHE" : "STREAMING");

    /* ========== 提取页面文本 ========== */
    const char *page_text = NULL;
    uint32_t page_text_len = 0;

    if (viewer->use_cache_mode && viewer->chapter_decoded_cache) {
        /* 缓存模式：直接从缓存切片 */
        page_text = viewer->chapter_decoded_cache + page_start;
        page_text_len = page_end - page_start;
    } else {
        /* 流式模式：从临时文件读取并解码 */
        FIL temp_fp;
        if (f_open(&temp_fp, viewer->temp_file_path, FA_READ) == FR_OK) {
            /* 流式读取：从文件头部开始，跳过 page_start 个解码字符 */
            uint32_t skip_chars = page_start;

            /* 读取足够大的范围（取较大值保证覆盖） */
            uint32_t read_range = page_end - page_start + EPUB_WORK_BUF_SIZE;
            if (read_range > 65536) read_range = 65536;

            memset(viewer->html_buf, 0, EPUB_WORK_BUF_SIZE);
            UINT br = 0;
            uint32_t total_read = 0;
            uint32_t decoded_chars = 0;
            int output_pos = 0;

            uint32_t need = page_end - page_start;

            while (total_read < read_range) {
                UINT chunk = 0;
                if (f_read(&temp_fp, viewer->html_buf, EPUB_WORK_BUF_SIZE, &chunk) != FR_OK || chunk == 0)
                    break;

                memset(viewer->stripped_buf, 0, EPUB_WORK_BUF_SIZE * 2);
                int slen = strip_html_tags_with_styles(viewer->html_buf, (int)chunk,
                                                       viewer->stripped_buf, EPUB_WORK_BUF_SIZE * 2);

                memset(viewer->decoded_buf, 0, EPUB_WORK_BUF_SIZE * 2);
                decode_html_entities(viewer->stripped_buf, viewer->decoded_buf, EPUB_WORK_BUF_SIZE * 2);
                sanitize_utf8(viewer->decoded_buf);
                fix_chinese_punctuation(viewer->decoded_buf);

                uint32_t chunk_chars = strlen(viewer->decoded_buf);

                if (decoded_chars < skip_chars && decoded_chars + chunk_chars > skip_chars) {
                    /* 跳过部分 */
                    uint32_t skip_in_chunk = skip_chars - decoded_chars;
                    const char *src = viewer->decoded_buf + skip_in_chunk;
                    uint32_t src_len = chunk_chars - skip_in_chunk;

                    uint32_t copy_len = (src_len > need) ? need : src_len;

                    if (output_pos + copy_len < EPUB_WORK_BUF_SIZE * 2 - 1) {
                        memcpy(viewer->page_text_buf + output_pos, src, copy_len);
                        output_pos += copy_len;
                        need -= copy_len;
                    }

                    skip_chars = decoded_chars; /* 后续块不再跳过 */
                } else if (decoded_chars >= skip_chars) {
                    /* 正常追加，不超过 page_end 边界 */
                    uint32_t copy_len = (chunk_chars > need) ? need : chunk_chars;

                    if (output_pos + copy_len < EPUB_WORK_BUF_SIZE * 2 - 1) {
                        memcpy(viewer->page_text_buf + output_pos, viewer->decoded_buf, copy_len);
                        output_pos += copy_len;
                        need -= copy_len;
                    }
                    if (need == 0) break;
                }

                decoded_chars += chunk_chars;
                total_read += chunk;

                if (decoded_chars >= page_end)
                    break;
            }

            viewer->page_text_buf[output_pos] = '\0';
            page_text = viewer->page_text_buf;
            page_text_len = output_pos;
            f_close(&temp_fp);
        }
    }

    if (!page_text || page_text_len == 0) {
        lv_obj_t *err = lv_label_create(viewer->content_container);
        lv_obj_align(err, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_text_font(err, FONT, 0);
        lv_obj_set_style_text_color(err, lv_color_black(), 0);
        lv_label_set_text(err, "读取结束");
        return;
    }

    /* 复制到工作缓冲区（保留样式标记） */
    uint32_t copy_len = (page_text_len < EPUB_WORK_BUF_SIZE * 2 - 1) ? page_text_len : (EPUB_WORK_BUF_SIZE * 2 - 1);
    memcpy(viewer->decoded_buf, page_text, copy_len);
    viewer->decoded_buf[copy_len] = '\0';

    filter_unsupported_chars(viewer->decoded_buf);

    /* 修剪首尾空行 */
    char *clean_text = viewer->decoded_buf;
    while (*clean_text == '\n' || *clean_text == ' ' || *clean_text == '\r') clean_text++;

    /* 去除末尾空行 */
    char *end_ptr = clean_text + strlen(clean_text) - 1;
    while (end_ptr > clean_text && (*end_ptr == '\n' || *end_ptr == ' ')) *end_ptr-- = '\0';

    /* ========== 多Label渲染（样式感知） ========== */
    lv_obj_clean(viewer->content_container);

    int y_offset = 0;
    const char *p = clean_text;
    const char *block_start = clean_text;
    int current_level = 0;
    lv_font_t *current_font = FONT;
    int current_lh = LH_BODY;

    while (*p) {
        if ((unsigned char)*p == 0x02 && *(p+1) >= '0' && *(p+1) <= '3' &&
            (unsigned char)*(p+2) == 0x03) {
            if (p > block_start) {
                int block_len = (int)(p - block_start);
                while (block_len > 0 && (block_start[block_len-1] == '\n' || block_start[block_len-1] == ' '))
                    block_len--;
                if (block_len > 0) {
                    if (y_offset + current_lh > CONTENT_HEIGHT) break;

                    memcpy(viewer->reflowed_buf, block_start, block_len);
                    viewer->reflowed_buf[block_len] = '\0';

                    lv_point_t txt_size;
                    lv_txt_get_size(&txt_size, viewer->reflowed_buf, current_font, 0, 0,
                                   CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
                    int actual_h = txt_size.y;
                    if (actual_h < current_lh) actual_h = current_lh;

                    lv_obj_t *line_label = lv_label_create(viewer->content_container);
                    lv_obj_align(line_label, LV_ALIGN_TOP_LEFT, 0, y_offset);
                    lv_obj_set_size(line_label, CONTENT_WIDTH, actual_h);
                    lv_obj_set_style_text_font(line_label, current_font, 0);
                    lv_obj_set_style_text_color(line_label, lv_color_black(), 0);
                    lv_obj_set_style_pad_top(line_label, 0, 0);
                    lv_label_set_long_mode(line_label, LV_LABEL_LONG_WRAP);
                    lv_label_set_text(line_label, viewer->reflowed_buf);

                    y_offset += actual_h;
                }
            }
            current_level = *(p+1) - '0';
            p += 3;
            block_start = p;
            switch (current_level) {
                case 1: current_font = FONT_H1; current_lh = LH_H1; break;
                case 2: current_font = FONT_H2; current_lh = LH_H2; break;
                case 3: current_font = FONT_H3; current_lh = LH_H3; break;
                default: current_font = FONT; current_lh = LH_BODY; break;
            }
        } else if (*p == '\n') {
            if (p > block_start) {
                int block_len = (int)(p - block_start);
                while (block_len > 0 && (block_start[block_len-1] == ' ')) block_len--;
                if (block_len > 0) {
                    if (y_offset + current_lh > CONTENT_HEIGHT) break;

                    memcpy(viewer->reflowed_buf, block_start, block_len);
                    viewer->reflowed_buf[block_len] = '\0';

                    lv_point_t txt_size;
                    lv_txt_get_size(&txt_size, viewer->reflowed_buf, current_font, 0, 0,
                                   CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
                    int actual_h = txt_size.y;
                    if (actual_h < current_lh) actual_h = current_lh;

                    lv_obj_t *line_label = lv_label_create(viewer->content_container);
                    lv_obj_align(line_label, LV_ALIGN_TOP_LEFT, 0, y_offset);
                    lv_obj_set_size(line_label, CONTENT_WIDTH, actual_h);
                    lv_obj_set_style_text_font(line_label, current_font, 0);
                    lv_obj_set_style_text_color(line_label, lv_color_black(), 0);
                    lv_obj_set_style_pad_top(line_label, 0, 0);
                    lv_label_set_long_mode(line_label, LV_LABEL_LONG_WRAP);
                    lv_label_set_text(line_label, viewer->reflowed_buf);

                    y_offset += actual_h;
                }
            }
            p++;
            block_start = p;
        } else {
            p++;
        }
    }

    /* 最后一个块 */
    if (*block_start && y_offset < CONTENT_HEIGHT) {
        int block_len = (int)(p - block_start);
        while (block_len > 0 && (block_start[block_len-1] == '\n' || block_start[block_len-1] == ' '))
            block_len--;
        if (block_len > 0) {
            memcpy(viewer->reflowed_buf, block_start, block_len);
            viewer->reflowed_buf[block_len] = '\0';

            lv_point_t txt_size;
            lv_txt_get_size(&txt_size, viewer->reflowed_buf, current_font, 0, 0,
                           CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
            int actual_h = txt_size.y;
            if (actual_h < current_lh) actual_h = current_lh;

            lv_obj_t *line_label = lv_label_create(viewer->content_container);
            lv_obj_align(line_label, LV_ALIGN_TOP_LEFT, 0, y_offset);
            lv_obj_set_size(line_label, CONTENT_WIDTH, actual_h);
            lv_obj_set_style_text_font(line_label, current_font, 0);
            lv_obj_set_style_text_color(line_label, lv_color_black(), 0);
            lv_obj_set_style_pad_top(line_label, 0, 0);
            lv_label_set_long_mode(line_label, LV_LABEL_LONG_WRAP);
            lv_label_set_text(line_label, viewer->reflowed_buf);
        }
    }

    /* 更新页码与标题 */
    char page_str[64];
    snprintf(page_str, sizeof(page_str), "%d/%d", viewer->current_page + 1, viewer->total_pages);
    lv_label_set_text(viewer->page_label, page_str);

    char title_str[64];
    snprintf(title_str, sizeof(title_str), "第%d章", viewer->current_chapter + 1);
    lv_label_set_text(viewer->title_label, title_str);

    epd_mark_refresh_pending();
    VIEW_LOG("=== update_display END (y=%d) ===\n", y_offset);
}

/*====================
 *   翻页处理
 *====================*/

static void prev_page_handler(EpubViewer *viewer) {
    if (!viewer) return;
    if (viewer->current_page > 0) {
        viewer->current_page--;
        update_display(viewer);
    } else if (viewer->current_chapter > 0) {
        viewer->current_chapter--;
        if (viewer->chapter_cb) {
            viewer->chapter_cb(viewer->current_chapter,
                              viewer->reader ? viewer->reader->spine_count : 0);
        }
    }
}

static void next_page_handler(EpubViewer *viewer) {
    if (!viewer) return;
    if (viewer->current_page < viewer->total_pages - 1) {
        viewer->current_page++;
        update_display(viewer);
    } else if (viewer->reader && viewer->current_chapter < viewer->reader->spine_count - 1) {
        viewer->current_chapter++;
        if (viewer->chapter_cb) {
            viewer->chapter_cb(viewer->current_chapter, viewer->reader->spine_count);
        }
    }
}

/*====================
 *   触摸事件
 *====================*/

#define BTN_PREV_X     0
#define BTN_NEXT_X     160

static void content_area_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *screen = lv_event_get_target(e);
    EpubViewer *viewer = (EpubViewer*)lv_obj_get_user_data(screen);
    if (!viewer) return;

    lv_point_t point;
    lv_indev_get_point(lv_indev_get_act(), &point);

    if (code == LV_EVENT_CLICKED || code == LV_EVENT_RELEASED) {
        int x = point.x, y = point.y;

        if (x < CONTENT_X || x > CONTENT_X + CONTENT_WIDTH ||
            y < CONTENT_Y || y > CONTENT_Y + CONTENT_HEIGHT) {
            return;
        }

        if (x < 80) {
            prev_page_handler(viewer);
        } else if (x > 160) {
            next_page_handler(viewer);
        } else {
            epub_viewer_show_toc(viewer);
        }
    }
}

/*====================
 *   目录相关
 *====================*/

static void toc_item_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    EpubViewer *viewer = (EpubViewer*)lv_obj_get_user_data(btn);
    if (!viewer) return;

    lv_obj_t *parent = lv_obj_get_parent(btn);
    uint32_t child_cnt = lv_obj_get_child_cnt(parent);
    uint32_t btn_idx = 0;
    for (uint32_t i = 0; i < child_cnt; i++) {
        if (lv_obj_get_child(parent, i) == btn) { btn_idx = i; break; }
    }

    if (viewer->reader) {
        int chapter = epub_reader_jump_to_toc(viewer->reader, btn_idx);
        if (chapter >= 0) epub_viewer_goto_chapter(viewer, chapter);
    }

    if (viewer->toc_list) {
        lv_obj_del_async(viewer->toc_list);
        viewer->toc_list = NULL;
    }
}

static void toc_btn_close_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    EpubViewer *viewer = (EpubViewer*)lv_obj_get_user_data(btn);
    if (viewer && viewer->toc_list) {
        lv_obj_del_async(viewer->toc_list);
        viewer->toc_list = NULL;
        epd_mark_refresh_pending();
    }
}

/*====================
 *   回调
 *====================*/

static void close_viewer_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    EpubViewer *viewer = (EpubViewer*)lv_obj_get_user_data(btn);
    if (viewer) epub_viewer_close(viewer);
}

static void page_prev_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    EpubViewer *viewer = (EpubViewer*)lv_obj_get_user_data(btn);
    if (viewer) prev_page_handler(viewer);
}

static void page_next_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    EpubViewer *viewer = (EpubViewer*)lv_obj_get_user_data(btn);
    if (viewer) next_page_handler(viewer);
}

static void toc_btn_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    EpubViewer *viewer = (EpubViewer*)lv_obj_get_user_data(btn);
    if (viewer) epub_viewer_show_toc(viewer);
}

/*====================
 *   公共API
 *====================*/

EpubViewer* epub_viewer_create(EpubReader *reader) {
    EpubViewer *viewer = (EpubViewer*)_dma_malloc(sizeof(EpubViewer), DMAHEAP_PSRAM);
    if (!viewer) { VIEW_LOG("Alloc failed\n"); return NULL; }

    memset(viewer, 0, sizeof(EpubViewer));
    viewer->reader = reader;
    viewer->current_chapter = 0;
    viewer->current_page = 0;
    viewer->total_pages = 1;

    /* 分配工作缓冲区 */
    viewer->html_buf       = (char*)_dma_malloc(EPUB_WORK_BUF_SIZE, DMAHEAP_PSRAM);
    viewer->stripped_buf   = (char*)_dma_malloc(EPUB_WORK_BUF_SIZE * 2, DMAHEAP_PSRAM);
    viewer->decoded_buf    = (char*)_dma_malloc(EPUB_WORK_BUF_SIZE * 2, DMAHEAP_PSRAM);
    viewer->reflowed_buf   = (char*)_dma_malloc(EPUB_WORK_BUF_SIZE * 2, DMAHEAP_PSRAM);
    viewer->page_text_buf  = (char*)_dma_malloc(EPUB_WORK_BUF_SIZE * 2, DMAHEAP_PSRAM);

    if (!viewer->html_buf || !viewer->stripped_buf || !viewer->decoded_buf ||
        !viewer->reflowed_buf || !viewer->page_text_buf) {
        VIEW_LOG("PSRAM buffer alloc failed\n");
        if (viewer->html_buf) _dma_free(viewer->html_buf, 0);
        if (viewer->stripped_buf) _dma_free(viewer->stripped_buf, 0);
        if (viewer->decoded_buf) _dma_free(viewer->decoded_buf, 0);
        if (viewer->reflowed_buf) _dma_free(viewer->reflowed_buf, 0);
        if (viewer->page_text_buf) _dma_free(viewer->page_text_buf, 0);
        _dma_free(viewer, 0);
        return NULL;
    }

    memset(viewer->html_buf, 0, EPUB_WORK_BUF_SIZE);
    memset(viewer->stripped_buf, 0, EPUB_WORK_BUF_SIZE * 2);
    memset(viewer->decoded_buf, 0, EPUB_WORK_BUF_SIZE * 2);
    memset(viewer->reflowed_buf, 0, EPUB_WORK_BUF_SIZE * 2);
    memset(viewer->page_text_buf, 0, EPUB_WORK_BUF_SIZE * 2);

    VIEW_LOG("Buffers allocated: html=%dKB stripped=%dKB decoded=%dKB reflowed=%dKB page_text=%dKB\n",
             EPUB_WORK_BUF_SIZE/1024, EPUB_WORK_BUF_SIZE*2/1024,
             EPUB_WORK_BUF_SIZE*2/1024, EPUB_WORK_BUF_SIZE*2/1024,
             EPUB_WORK_BUF_SIZE*2/1024);

    strncpy(viewer->temp_file_path, "0:/epub_temp.html", sizeof(viewer->temp_file_path) - 1);
    return viewer;
}

void epub_viewer_show(EpubViewer *viewer) {
    if (!viewer) return;

    viewer->screen = lv_obj_create(NULL);
    lv_obj_set_size(viewer->screen, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(viewer->screen, lv_color_white(), 0);
    lv_obj_clear_flag(viewer->screen, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC |
                      LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN);
    epd_disable_all_animations_recursive(viewer->screen);

    /* Header */
    lv_obj_t *header = lv_obj_create(viewer->screen);
    lv_obj_set_size(header, SCREEN_WIDTH, 40);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_white(), 0);
    epd_disable_all_animations_recursive(header);

    lv_obj_t *back_btn = lv_btn_create(header);
    lv_obj_set_size(back_btn, 50, 30);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_white(), 0);
    lv_obj_set_style_border_width(back_btn, 1, 0);
    lv_obj_set_user_data(back_btn, viewer);
    lv_obj_add_event_cb(back_btn, close_viewer_cb, LV_EVENT_CLICKED, viewer);
    epd_disable_all_animations_recursive(back_btn);

    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "返回");
    lv_obj_set_style_text_font(back_label, FONT, 0);
    lv_obj_center(back_label);

    viewer->title_label = lv_label_create(header);
    lv_label_set_text(viewer->title_label, "加载中...");
    lv_obj_set_style_text_font(viewer->title_label, FONT, 0);
    lv_obj_align(viewer->title_label, LV_ALIGN_TOP_MID, 0, 10);

    viewer->page_label = lv_label_create(header);
    lv_label_set_text(viewer->page_label, "1/1");
    lv_obj_set_style_text_font(viewer->page_label, FONT, 0);
    lv_obj_align(viewer->page_label, LV_ALIGN_RIGHT_MID, -10, 0);

    /* 内容容器 */
    viewer->content_container = lv_obj_create(viewer->screen);
    lv_obj_set_size(viewer->content_container, CONTENT_WIDTH, CONTENT_HEIGHT);
    lv_obj_align(viewer->content_container, LV_ALIGN_TOP_LEFT, CONTENT_X, CONTENT_Y);
    lv_obj_set_style_bg_color(viewer->content_container, lv_color_white(), 0);
    lv_obj_set_style_pad_all(viewer->content_container, 0, 0);
    lv_obj_clear_flag(viewer->content_container, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC |
                      LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN);
    epd_disable_all_animations_recursive(viewer->content_container);

    lv_obj_set_user_data(viewer->screen, viewer);
    lv_obj_add_event_cb(viewer->screen, content_area_event_cb, LV_EVENT_CLICKED, viewer);

    /* 导航栏 */
    lv_obj_t *nav_bar = lv_obj_create(viewer->screen);
    lv_obj_set_size(nav_bar, SCREEN_WIDTH, 50);
    lv_obj_align(nav_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(nav_bar, lv_color_white(), 0);
    epd_disable_all_animations_recursive(nav_bar);

    lv_obj_t *page_prev_btn = lv_btn_create(nav_bar);
    lv_obj_set_size(page_prev_btn, 55, 22);
    lv_obj_align(page_prev_btn, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_border_width(page_prev_btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(page_prev_btn, 1, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(page_prev_btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_user_data(page_prev_btn, viewer);
    lv_obj_add_event_cb(page_prev_btn, page_prev_cb, LV_EVENT_CLICKED, viewer);
    epd_disable_all_animations_recursive(page_prev_btn);

    lv_obj_t *page_prev_label = lv_label_create(page_prev_btn);
    lv_label_set_text(page_prev_label, "上一页");
    lv_obj_set_style_text_font(page_prev_label, FONT, 0);
    lv_obj_center(page_prev_label);

    lv_obj_t *toc_btn_obj = lv_btn_create(nav_bar);
    lv_obj_set_size(toc_btn_obj, 50, 22);
    lv_obj_align(toc_btn_obj, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_set_style_border_width(toc_btn_obj, 1, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(toc_btn_obj, 1, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(toc_btn_obj, 0, LV_STATE_FOCUSED);
    lv_obj_set_user_data(toc_btn_obj, viewer);
    lv_obj_add_event_cb(toc_btn_obj, toc_btn_cb, LV_EVENT_CLICKED, viewer);
    epd_disable_all_animations_recursive(toc_btn_obj);

    lv_obj_t *toc_label = lv_label_create(toc_btn_obj);
    lv_label_set_text(toc_label, "目录");
    lv_obj_set_style_text_font(toc_label, FONT, 0);
    lv_obj_center(toc_label);

    lv_obj_t *page_next_btn = lv_btn_create(nav_bar);
    lv_obj_set_size(page_next_btn, 55, 22);
    lv_obj_align(page_next_btn, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_style_border_width(page_next_btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(page_next_btn, 1, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(page_next_btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_user_data(page_next_btn, viewer);
    lv_obj_add_event_cb(page_next_btn, page_next_cb, LV_EVENT_CLICKED, viewer);
    epd_disable_all_animations_recursive(page_next_btn);

    lv_obj_t *page_next_label = lv_label_create(page_next_btn);
    lv_label_set_text(page_next_label, "下一页");
    lv_obj_set_style_text_font(page_next_label, FONT, 0);
    lv_obj_center(page_next_label);

    lv_disp_load_scr(viewer->screen);
    VIEW_LOG("Viewer shown\n");
}

void epub_viewer_close(EpubViewer *viewer) {
    if (!viewer) return;
    if (viewer->screen) {
        lv_obj_del_async(viewer->screen);
        viewer->screen = NULL;
    }
    if (viewer->toc_list) {
        lv_obj_del_async(viewer->toc_list);
        viewer->toc_list = NULL;
    }
    viewer->content_container = NULL;
    viewer->title_label = NULL;
    viewer->page_label = NULL;
}

void epub_viewer_destroy(EpubViewer *viewer) {
    if (viewer) {
        epub_viewer_close(viewer);
        free_page_index(viewer);
        if (viewer->html_buf) _dma_free(viewer->html_buf, 0);
        if (viewer->stripped_buf) _dma_free(viewer->stripped_buf, 0);
        if (viewer->decoded_buf) _dma_free(viewer->decoded_buf, 0);
        if (viewer->reflowed_buf) _dma_free(viewer->reflowed_buf, 0);
        if (viewer->page_text_buf) _dma_free(viewer->page_text_buf, 0);
        _dma_free(viewer, 0);
    }
}

bool epub_viewer_prev_page(EpubViewer *viewer) {
    if (!viewer) return false;
    if (viewer->current_page > 0) {
        viewer->current_page--;
        update_display(viewer);
        return true;
    } else if (viewer->current_chapter > 0) {
        viewer->current_chapter--;
        return true;
    }
    return false;
}

bool epub_viewer_next_page(EpubViewer *viewer) {
    if (!viewer) return false;
    if (viewer->current_page < viewer->total_pages - 1) {
        viewer->current_page++;
        update_display(viewer);
        return true;
    } else if (viewer->reader && viewer->current_chapter < viewer->reader->spine_count - 1) {
        viewer->current_chapter++;
        return true;
    }
    return false;
}

bool epub_viewer_goto_chapter(EpubViewer *viewer, int chapter_index) {
    if (!viewer || !viewer->reader) return false;

    if (chapter_index < 0 || chapter_index >= viewer->reader->spine_count) {
        VIEW_ERR("Chapter index out of range: %d (max: %d)\n",
                 chapter_index, viewer->reader->spine_count - 1);
        return false;
    }

    viewer->current_chapter = chapter_index;
    viewer->current_page = 0;

    VIEW_LOG("Goto chapter %d...\n", chapter_index);

    if (build_page_index(viewer, chapter_index) != 0) {
        VIEW_ERR("Failed to build page index for chapter %d\n", chapter_index);
        lv_obj_t *err = lv_label_create(viewer->content_container);
        lv_obj_align(err, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_text_font(err, FONT, 0);
        lv_label_set_text(err, "章节加载失败");
        viewer->total_pages = 1;
        viewer->current_page = 0;
        return false;
    }

    update_display(viewer);
    VIEW_LOG("Chapter %d ready: %d pages\n", chapter_index, viewer->total_pages);
    return true;
}

void epub_viewer_show_toc(EpubViewer *viewer) {
    if (!viewer || !viewer->reader || !viewer->screen) return;

    if (viewer->toc_list) {
        lv_obj_del_async(viewer->toc_list);
        viewer->toc_list = NULL;
        return;
    }

    viewer->toc_list = lv_obj_create(viewer->screen);
    lv_obj_set_size(viewer->toc_list, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_align(viewer->toc_list, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(viewer->toc_list, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(viewer->toc_list, LV_OPA_COVER, 0);

    lv_obj_t *toc_title = lv_label_create(viewer->toc_list);
    lv_label_set_text(toc_title, "目录");
    lv_obj_set_style_text_font(toc_title, FONT, 0);
    lv_obj_align(toc_title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *close_btn = lv_btn_create(viewer->toc_list);
    lv_obj_set_size(close_btn, 50, 30);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -10, 5);
    lv_obj_set_user_data(close_btn, viewer);
    lv_obj_add_event_cb(close_btn, toc_btn_close_cb, LV_EVENT_CLICKED, viewer);

    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "关闭");
    lv_obj_set_style_text_font(close_label, FONT, 0);
    lv_obj_center(close_label);

    int toc_count = epub_reader_get_toc_count(viewer->reader);
    if (toc_count > 0) {
        lv_obj_t *list = lv_list_create(viewer->toc_list);
        lv_obj_set_size(list, SCREEN_WIDTH - 20, SCREEN_HEIGHT - 100);
        lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_set_style_pad_row(list, 2, 0);

        for (int i = 0; i < toc_count && i < 20; i++) {
            EpubTocEntry *toc = epub_reader_get_toc(viewer->reader, i);
            if (toc) {
                lv_obj_t *btn = lv_list_add_btn(list, NULL, toc->title);
                lv_obj_set_style_text_font(btn, FONT, 0);
                lv_obj_set_user_data(btn, viewer);
                lv_obj_add_event_cb(btn, toc_item_cb, LV_EVENT_CLICKED, NULL);
            }
        }
    } else {
        lv_obj_t *info = lv_label_create(viewer->toc_list);
        lv_label_set_text(info, "无目录信息\n\n显示章节列表");
        lv_obj_set_style_text_font(info, FONT, 0);
        lv_obj_align(info, LV_ALIGN_CENTER, 0, 0);
    }

    VIEW_LOG("TOC shown, %d entries\n", toc_count);
    epd_mark_refresh_pending();
}

int epub_viewer_get_current_chapter(EpubViewer *viewer) { return viewer ? viewer->current_chapter : 0; }
int epub_viewer_get_current_page(EpubViewer *viewer) { return viewer ? viewer->current_page + 1 : 1; }
int epub_viewer_get_total_pages(EpubViewer *viewer) { return viewer ? viewer->total_pages : 1; }

void epub_viewer_set_chapter_loaded_cb(EpubViewer *viewer, epub_chapter_loaded_cb cb) {
    if (viewer) viewer->chapter_cb = cb;
}

/*====================
 *   HTML处理API
 *====================*/

int epub_strip_html_tags(const char *html, int len, char *output, int out_size) {
    return strip_html_tags_with_styles(html, len, output, out_size);
}

int epub_decode_html_entities(const char *text, char *output, int out_size) {
    decode_html_entities(text, output, out_size);
    return strlen(output);
}
