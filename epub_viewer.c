/**
 * @file epub_viewer.c
 * @brief EPUB阅读器LVGL显示界面实现 - 自适应双模式（缓存+流式）
 *
 * 核心改进：
 * - 根Bug修复：update_display()以前没用page_char_offsets[]做精确定位，导致
 *   第一页读章末8KB（字节偏移与字符偏移错位），翻页只有7-8行
 * - 自适应双模式：章节<=96KB全缓存（直接切片），>96KB流式（按需读文件）
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

/* PSRAM 缓存模式阈值（<=96KB全缓存，>96KB流式）统一在 epub_viewer.h */

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
    uint32_t chapter_decoded_len;  /* canonical decoded stream总字节数 */
    uint32_t chapter_uncomp_size;  /* 解压后章节总大小（字节） */
    bool use_cache_mode;           /* true=缓存模式 false=流式模式 */

    /* 流式解析核心：页码-字符偏移索引 */
    uint32_t *page_char_offsets;   /* [page_index] = canonical decoded stream byte offset */
    uint8_t *page_start_styles;    /* [page_index] = page start style level */
    int max_pages;
    int total_pages;
    int current_page;
    char temp_file_path[64];
    char decoded_file_path[64];

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
static void strip_style_markers(char *str);
static void filter_unsupported_chars_ex(char *str, bool preserve_markers);
static uint32_t calc_bytes_for_height(const char *text, uint32_t text_len,
                                      lv_font_t *font, int line_height,
                                      int target_height);
static int flush_render_block(EpubViewer *viewer, const char *block_start, int block_len,
                              lv_font_t *font, int line_height, int *y_offset);
static int build_decoded_stream(EpubViewer *viewer);

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

/* 剥离样式标记 \x02N\x03（用于显示前清理） */
static void strip_style_markers(char *str) {
    char *read_ptr = str;
    char *write_ptr = str;
    int markers_stripped = 0;
    while (*read_ptr) {
        if ((unsigned char)*read_ptr == 0x02 &&
            *(read_ptr+1) >= '0' && *(read_ptr+1) <= '3' &&
            (unsigned char)*(read_ptr+2) == 0x03) {
            read_ptr += 3;
            markers_stripped++;
        } else {
            *write_ptr++ = *read_ptr++;
        }
    }
    *write_ptr = '\0';
    if (markers_stripped > 0) {
        VIEW_LOG("[STRIP] removed %d style markers\n", markers_stripped);
    }
}

static void filter_unsupported_chars_ex(char *str, bool preserve_markers) {
    unsigned char *read_ptr = (unsigned char *)str;
    unsigned char *write_ptr = (unsigned char *)str;
    int chars_filtered = 0;

    while (*read_ptr) {
        if (!preserve_markers && (read_ptr[0] == 0x02 || read_ptr[0] == 0x03)) {
            read_ptr++;
            chars_filtered++;
            continue;
        }
        if (preserve_markers && read_ptr[0] == 0x02 && read_ptr[1] >= '0' && read_ptr[1] <= '3' && read_ptr[2] == 0x03) {
            *write_ptr++ = *read_ptr++;
            *write_ptr++ = *read_ptr++;
            *write_ptr++ = *read_ptr++;
            continue;
        }

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
            (unicode >= 0x3000 && unicode <= 0x303F)) {
            for (int i = 0; i < char_len; i++) *write_ptr++ = read_ptr[i];
        } else {
            if (unicode == 0xFF08 || unicode == 0xFF09) *write_ptr++ = '(';
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
    if (chars_filtered > 0) {
        VIEW_LOG("[FILTER] stripped %d control chars\n", chars_filtered);
    }
}

static void filter_unsupported_chars(char *str) {
    filter_unsupported_chars_ex(str, false);
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
                if ((tag_start[1] == 'h' || tag_start[1] == 'H') &&
                    (tag_start[2] >= '1' && tag_start[2] <= '6')) {
                    /* 更健壮的检测：h1-h6，允许任意后续字符（属性、空格、>等） */
                    int level = tag_start[2] - '0';
                    if (current_level != level) {
                        if (out_pos > 0 && output[out_pos-1] != '\n') {
                            if (out_pos < out_size - 1) output[out_pos++] = '\n';
                        }
                        VIEW_LOG("[HTML] H%d tag: %.*s\n", level, 20, tag_start);
                        EMIT_LEVEL(level);
                        current_level = level;
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
    if (viewer->page_start_styles) {
        _dma_free(viewer->page_start_styles, 0);
        viewer->page_start_styles = NULL;
    }
    if (viewer->chapter_decoded_cache) {
        _dma_free(viewer->chapter_decoded_cache, 0);
        viewer->chapter_decoded_cache = NULL;
    }
    viewer->max_pages = 0;
    viewer->total_pages = 0;
    viewer->use_cache_mode = false;
    viewer->chapter_decoded_len = 0;
    viewer->decoded_file_path[0] = '\0';
}

/* 计算指定高度需要多少 UTF-8 安全字节 */
static uint32_t calc_bytes_for_height(const char *text, uint32_t text_len,
                                      lv_font_t *font, int line_height,
                                      int target_height) {
    if (!text || text_len == 0 || target_height <= 0) return 0;

    uint32_t low = 0;
    uint32_t high = text_len;
    uint32_t best = 0;
    char work_buf[512];

    while (low <= high) {
        uint32_t mid = low + ((high - low) / 2);
        while (mid > 0 && (((unsigned char)text[mid]) & 0xC0) == 0x80) {
            mid--;
        }
        if (mid == 0) mid = 1;
        if (mid >= sizeof(work_buf)) mid = sizeof(work_buf) - 1;

        memcpy(work_buf, text, mid);
        work_buf[mid] = '\0';

        lv_point_t txt_size;
        lv_txt_get_size(&txt_size, work_buf, font, 0, 0, CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
        int actual_h = txt_size.y;
        if (actual_h < line_height) actual_h = line_height;

        if (actual_h <= target_height) {
            best = mid;
            low = mid + 1;
        } else {
            if (mid == 0) break;
            high = mid - 1;
        }
    }

    if (best == 0) {
        uint32_t mid = text_len < sizeof(work_buf) - 1 ? text_len : sizeof(work_buf) - 1;
        while (mid > 0 && (((unsigned char)text[mid]) & 0xC0) == 0x80) {
            mid--;
        }
        if (mid == 0) mid = 1;
        best = mid;
    }
    return best;
}

static int flush_render_block(EpubViewer *viewer, const char *block_start, int block_len,
                              lv_font_t *font, int line_height, int *y_offset) {
    if (!viewer || !block_start || block_len <= 0 || !font || !y_offset) return 0;

    while (block_len > 0 && (block_start[block_len - 1] == '\n' || block_start[block_len - 1] == ' ')) {
        block_len--;
    }
    if (block_len <= 0) return 0;

    memcpy(viewer->reflowed_buf, block_start, block_len);
    viewer->reflowed_buf[block_len] = '\0';

    lv_point_t txt_size;
    lv_txt_get_size(&txt_size, viewer->reflowed_buf, font, 0, 0, CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
    int actual_h = txt_size.y;
    if (actual_h < line_height) actual_h = line_height;
    if (*y_offset + actual_h > CONTENT_HEIGHT) return 0;

    lv_obj_t *line_label = lv_label_create(viewer->content_container);
    lv_obj_align(line_label, LV_ALIGN_TOP_LEFT, 0, *y_offset);
    lv_obj_set_size(line_label, CONTENT_WIDTH, actual_h);
    lv_obj_set_style_text_font(line_label, font, 0);
    lv_obj_set_style_text_color(line_label, lv_color_black(), 0);
    lv_obj_set_style_pad_top(line_label, 0, 0);
    lv_label_set_long_mode(line_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(line_label, viewer->reflowed_buf);

    *y_offset += actual_h;
    return actual_h;
}

static int write_decoded_chunk(FIL *decoded_fp, const char *buf, uint32_t len,
                               uint32_t *stream_offset, char *cache_ptr, uint32_t *cache_left) {
    if (!decoded_fp || !buf || len == 0 || !stream_offset) return 0;

    UINT bw = 0;
    if (f_write(decoded_fp, buf, len, &bw) != FR_OK || bw != len) {
        return -1;
    }

    if (cache_ptr && cache_left && *cache_left > len + 1) {
        memcpy(cache_ptr, buf, len);
        cache_ptr[len] = '\0';
        *cache_left -= len;
    }

    *stream_offset += len;
    return (int)len;
}

static int build_decoded_stream(EpubViewer *viewer) {
    VIEW_LOG("[DECODE] build_decoded_stream ENTRY\n");
    FIL html_fp;
    FIL decoded_fp;
    
    VIEW_LOG("[DECODE] decoded_file_path='%s'\n", viewer->decoded_file_path);
    FRESULT fr = f_open(&html_fp, viewer->temp_file_path, FA_READ);
    VIEW_LOG("[DECODE] f_open html result=%d\n", fr);
    if (fr != FR_OK) return -1;
    
    VIEW_LOG("[DECODE] temp_file='%s'\n", viewer->temp_file_path);
    fr = f_open(&decoded_fp, viewer->decoded_file_path, FA_CREATE_ALWAYS | FA_WRITE);
    VIEW_LOG("[DECODE] f_open decoded result=%d\n", fr);
    if (fr != FR_OK) {
        f_close(&html_fp);
        return -1;
    }

    uint32_t stream_offset = 0;
    char *cache_ptr = viewer->chapter_decoded_cache;
    uint32_t cache_left = 0;
    if (viewer->use_cache_mode && cache_ptr) {
        cache_left = EPUB_CACHE_THRESHOLD + 4096;
    } else {
        cache_ptr = NULL;
    }

    VIEW_LOG("[DECODE] starting read loop, cache_mode=%d cache_ptr=%p\n", viewer->use_cache_mode, cache_ptr);
    int loop_count = 0;
    while (1) {
        UINT br = 0;
        fr = f_read(&html_fp, viewer->html_buf, EPUB_WORK_BUF_SIZE, &br);
        VIEW_LOG("[DECODE] f_read loop=%d fr=%d br=%u\n", loop_count++, fr, br);
        if (fr != FR_OK || br == 0) break;

        memset(viewer->stripped_buf, 0, EPUB_WORK_BUF_SIZE * 2);
        int stripped_len = strip_html_tags_with_styles(viewer->html_buf, (int)br, viewer->stripped_buf, EPUB_WORK_BUF_SIZE * 2);
        VIEW_LOG("[DECODE] stripped_len=%d\n", stripped_len);

        memset(viewer->decoded_buf, 0, EPUB_WORK_BUF_SIZE * 2);
        decode_html_entities(viewer->stripped_buf, viewer->decoded_buf, EPUB_WORK_BUF_SIZE * 2);
        sanitize_utf8(viewer->decoded_buf);
        fix_chinese_punctuation(viewer->decoded_buf);
        filter_unsupported_chars_ex(viewer->decoded_buf, true);

        uint32_t chunk_len = strlen(viewer->decoded_buf);
        VIEW_LOG("[DECODE] chunk_len=%u stream_off=%u\n", chunk_len, stream_offset);
        if (chunk_len > 0) {
            UINT bw = 0;
            fr = f_write(&decoded_fp, viewer->decoded_buf, chunk_len, &bw);
            VIEW_LOG("[DECODE] f_write result=%d bw=%u\n", fr, bw);
            if (fr != FR_OK || bw != chunk_len) {
                f_close(&html_fp);
                f_close(&decoded_fp);
                return -1;
            }
            if (cache_ptr && cache_left > chunk_len) {
                memcpy(cache_ptr, viewer->decoded_buf, chunk_len);
                cache_ptr += chunk_len;
                cache_left -= chunk_len;
                *cache_ptr = '\0';
            }
            stream_offset += chunk_len;
        }
    }

    VIEW_LOG("[DECODE] read loop done, stream_offset=%u\n", stream_offset);
    f_close(&html_fp);
    VIEW_LOG("[DECODE] html_fp closed\n");
    f_close(&decoded_fp);
    VIEW_LOG("[DECODE] decoded_fp closed\n");
    viewer->chapter_decoded_len = stream_offset;
    VIEW_LOG("[DECODE] DONE chapter_decoded_len=%u\n", stream_offset);
    return 0;
}

/**
 * @brief 构建页码索引（decoded temp file canonical stream）
 */
static int build_page_index(EpubViewer *viewer, int chapter_index) {
    if (!viewer || !viewer->reader) return -1;

    VIEW_LOG("Building page index for chapter %d...\n", chapter_index);
    free_page_index(viewer);
    strncpy(viewer->temp_file_path, "0:/epub_temp.html", sizeof(viewer->temp_file_path) - 1);
    strncpy(viewer->decoded_file_path, "0:/epub_temp.decoded", sizeof(viewer->decoded_file_path) - 1);

    int ret = epub_reader_extract_chapter_to_file(viewer->reader, chapter_index, viewer->temp_file_path);
    if (ret < 0) {
        VIEW_ERR("Failed to extract chapter\n");
        return -1;
    }
    uint32_t uncomp_size = (uint32_t)ret;
    viewer->chapter_uncomp_size = uncomp_size;
    VIEW_LOG("Chapter uncompressed size: %u bytes, threshold=%d\n", uncomp_size, EPUB_CACHE_THRESHOLD);

    viewer->use_cache_mode = (uncomp_size <= EPUB_CACHE_THRESHOLD);
    viewer->max_pages = EPUB_MAX_PAGES_PER_CHAPTER;
    viewer->page_char_offsets = (uint32_t*)_dma_malloc(viewer->max_pages * sizeof(uint32_t), DMAHEAP_PSRAM);
    viewer->page_start_styles = (uint8_t*)_dma_malloc(viewer->max_pages * sizeof(uint8_t), DMAHEAP_PSRAM);
    if (!viewer->page_char_offsets || !viewer->page_start_styles) return -1;
    memset(viewer->page_char_offsets, 0, viewer->max_pages * sizeof(uint32_t));
    memset(viewer->page_start_styles, 0, viewer->max_pages * sizeof(uint8_t));

    if (viewer->use_cache_mode) {
        uint32_t cache_size = (uncomp_size + 4095) & ~4095UL;
        if (cache_size < 4096) cache_size = 4096;
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

    if (build_decoded_stream(viewer) != 0) {
        VIEW_ERR("Failed to build decoded stream\n");
        return -1;
    }

    FIL decoded_fp;
    if (f_open(&decoded_fp, viewer->decoded_file_path, FA_READ) != FR_OK) return -1;
    VIEW_LOG("[IDX] scanning decoded stream, stack-safe mode\n");

    viewer->page_char_offsets[0] = 0;
    viewer->page_start_styles[0] = 0;

    uint32_t stream_offset = 0;
    int page_count = 1;
    int page_y_offset = 0;
    int current_level = 0;
    lv_font_t *current_font = FONT;
    int current_lh = LH_BODY;

    int block_cap = EPUB_WORK_BUF_SIZE * 2 - 1;
    int block_len = 0;
    uint32_t block_start_offset = 0;

    while (1) {
        UINT br = 0;
        if (f_read(&decoded_fp, viewer->decoded_buf, EPUB_WORK_BUF_SIZE * 2 - 1, &br) != FR_OK || br == 0) break;
        viewer->decoded_buf[br] = '\0';

        const char *p = viewer->decoded_buf;
        const char *end = viewer->decoded_buf + br;
        while (p < end && *p) {
            if ((unsigned char)*p == 0x02 && (p + 2) < end && *(p + 1) >= '0' && *(p + 1) <= '3' && (unsigned char)*(p + 2) == 0x03) {
                current_level = *(p + 1) - '0';
                switch (current_level) {
                    case 1: current_font = FONT_H1; current_lh = LH_H1; break;
                    case 2: current_font = FONT_H2; current_lh = LH_H2; break;
                    case 3: current_font = FONT_H3; current_lh = LH_H3; break;
                    default: current_font = FONT; current_lh = LH_BODY; break;
                }
                if (block_len == 0) {
                    block_start_offset = stream_offset;
                }
                if (block_len + 3 < block_cap) {
                    memcpy(viewer->reflowed_buf + block_len, p, 3);
                    block_len += 3;
                }
                p += 3;
                stream_offset += 3;
                continue;
            }

            if (*p == '\n') {
                if (block_len > 0) {
                    viewer->reflowed_buf[block_len] = '\0';
                    memcpy(viewer->page_text_buf, viewer->reflowed_buf, block_len + 1);
                    strip_style_markers(viewer->page_text_buf);
                    lv_point_t txt_size;
                    lv_txt_get_size(&txt_size, viewer->page_text_buf, current_font, 0, 0, CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
                    int actual_h = txt_size.y;
                    if (actual_h < current_lh) actual_h = current_lh;
                    if (page_y_offset > 0 && page_y_offset + actual_h > CONTENT_HEIGHT && page_count < viewer->max_pages) {
                        viewer->page_char_offsets[page_count] = block_start_offset;
                        viewer->page_start_styles[page_count] = (uint8_t)current_level;
                        page_count++;
                        page_y_offset = actual_h;
                    } else {
                        page_y_offset += actual_h;
                    }
                    block_len = 0;
                    VIEW_LOG("[IDX] first block measured\n");
                }
                p++;
                stream_offset++;
                block_start_offset = stream_offset;
                continue;
            }

            int char_len = 1;
            unsigned char uc = (unsigned char)*p;
            if ((uc & 0xE0) == 0xC0 && (p + 1) < end) char_len = 2;
            else if ((uc & 0xF0) == 0xE0 && (p + 2) < end) char_len = 3;
            else if ((uc & 0xF8) == 0xF0 && (p + 3) < end) char_len = 4;

            if (block_len == 0) {
                block_start_offset = stream_offset;
            }
            if (block_len + char_len < block_cap) {
                memcpy(viewer->reflowed_buf + block_len, p, char_len);
                block_len += char_len;
            }
            p += char_len;
            stream_offset += char_len;
        }
    }

    if (block_len > 0) {
        viewer->reflowed_buf[block_len] = '\0';
        memcpy(viewer->page_text_buf, viewer->reflowed_buf, block_len + 1);
        strip_style_markers(viewer->page_text_buf);
        lv_point_t txt_size;
        lv_txt_get_size(&txt_size, viewer->page_text_buf, current_font, 0, 0, CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
        int actual_h = txt_size.y;
        if (actual_h < current_lh) actual_h = current_lh;
        if (page_y_offset > 0 && page_y_offset + actual_h > CONTENT_HEIGHT && page_count < viewer->max_pages) {
            viewer->page_char_offsets[page_count] = block_start_offset;
            viewer->page_start_styles[page_count] = (uint8_t)current_level;
            page_count++;
        }
    }

    f_close(&decoded_fp);
    viewer->total_pages = page_count;
    VIEW_LOG("Page index built: %d pages (chars=%u, mode=%s)\n", viewer->total_pages,
             viewer->chapter_decoded_len, viewer->use_cache_mode ? "CACHE" : "STREAMING");
    return 0;
}


/**
 * @brief 显示当前页（canonical decoded stream）
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
    if (viewer->current_page >= viewer->total_pages) viewer->current_page = viewer->total_pages - 1;

    uint32_t page_start = viewer->page_char_offsets[viewer->current_page];
    uint32_t page_end = (viewer->current_page + 1 < viewer->total_pages)
                          ? viewer->page_char_offsets[viewer->current_page + 1]
                          : viewer->chapter_decoded_len;
    uint8_t current_level = viewer->page_start_styles ? viewer->page_start_styles[viewer->current_page] : 0;

    VIEW_LOG("Page %d/%d: char_start=%u char_end=%u mode=%s\n",
             viewer->current_page + 1, viewer->total_pages,
             page_start, page_end,
             viewer->use_cache_mode ? "CACHE" : "STREAMING");

    const char *page_text = NULL;
    uint32_t page_text_len = page_end - page_start;

    if (viewer->use_cache_mode && viewer->chapter_decoded_cache) {
        page_text = viewer->chapter_decoded_cache + page_start;
    } else {
        FIL decoded_fp;
        if (f_open(&decoded_fp, viewer->decoded_file_path, FA_READ) != FR_OK) {
            return;
        }
        if (f_lseek(&decoded_fp, page_start) != FR_OK) {
            f_close(&decoded_fp);
            return;
        }
        if (page_text_len > EPUB_WORK_BUF_SIZE * 2 - 1) page_text_len = EPUB_WORK_BUF_SIZE * 2 - 1;
        UINT br = 0;
        if (f_read(&decoded_fp, viewer->page_text_buf, page_text_len, &br) != FR_OK) {
            f_close(&decoded_fp);
            return;
        }
        f_close(&decoded_fp);
        viewer->page_text_buf[br] = '\0';
        page_text = viewer->page_text_buf;
        page_text_len = br;
    }

    if (!page_text || page_text_len == 0) {
        lv_obj_t *err = lv_label_create(viewer->content_container);
        lv_obj_align(err, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_text_font(err, FONT, 0);
        lv_obj_set_style_text_color(err, lv_color_black(), 0);
        lv_label_set_text(err, "读取结束");
        return;
    }

    uint32_t copy_len = (page_text_len < EPUB_WORK_BUF_SIZE * 2 - 1) ? page_text_len : (EPUB_WORK_BUF_SIZE * 2 - 1);
    memcpy(viewer->decoded_buf, page_text, copy_len);
    viewer->decoded_buf[copy_len] = '\0';

    lv_obj_clean(viewer->content_container);

    const char *p = viewer->decoded_buf;
    const char *block_start = p;
    int y_offset = 0;
    lv_font_t *current_font = FONT;
    int current_lh = LH_BODY;
    switch (current_level) {
        case 1: current_font = FONT_H1; current_lh = LH_H1; break;
        case 2: current_font = FONT_H2; current_lh = LH_H2; break;
        case 3: current_font = FONT_H3; current_lh = LH_H3; break;
        default: current_font = FONT; current_lh = LH_BODY; break;
    }

    while (*p) {
        if ((unsigned char)*p == 0x02 && *(p + 1) >= '0' && *(p + 1) <= '3' && (unsigned char)*(p + 2) == 0x03) {
            flush_render_block(viewer, block_start, (int)(p - block_start), current_font, current_lh, &y_offset);
            current_level = *(p + 1) - '0';
            switch (current_level) {
                case 1: current_font = FONT_H1; current_lh = LH_H1; break;
                case 2: current_font = FONT_H2; current_lh = LH_H2; break;
                case 3: current_font = FONT_H3; current_lh = LH_H3; break;
                default: current_font = FONT; current_lh = LH_BODY; break;
            }
            p += 3;
            block_start = p;
            continue;
        }
        if (*p == '\n') {
            flush_render_block(viewer, block_start, (int)(p - block_start), current_font, current_lh, &y_offset);
            p++;
            block_start = p;
            continue;
        }
        p++;
    }
    flush_render_block(viewer, block_start, (int)(p - block_start), current_font, current_lh, &y_offset);

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
    viewer->decoded_file_path[0] = '\0';

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
