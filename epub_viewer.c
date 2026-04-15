/**
 * @file epub_viewer.c
 * @brief EPUB阅读器LVGL显示界面实现 - 支持超大章节的流式解析方案
 * 
 * 核心改进：
 * - "以磁盘换内存"策略，支持无限大小章节（>256KB甚至数MB）
 * - 构建页码索引（page_offsets数组），记录每页在临时文件中的偏移量
 * - 按需加载：用户翻到哪一页，才从SD卡读取该页需要的HTML源码
 * - 内存消耗恒定：只有几KB的读取缓冲区 + 几KB的索引数组
 * 
 * 页面渲染流程：
 * 1. epub_viewer_goto_chapter() → 解压章节到临时文件 + 构建页码索引
 * 2. update_display() → 根据page_offsets[page]跳转到文件偏移 → 读取4-8KB HTML → 解析显示
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

/* 流式解析配置 - 以磁盘换内存 */
#define EPUB_MAX_PAGES_PER_CHAPTER  1000   /* 单章最多1000页 */
#define EPUB_WORK_BUF_SIZE          8192  /* 工作缓冲区大小：8KB，用于读取HTML块 */
#define EPUB_PAGE_TEXT_SIZE         4096  /* 单页纯文本缓冲区大小 */

struct EpubViewer {
    EpubReader *reader;                    /* EPUB阅读器 */
    lv_obj_t *screen;                      /* 主屏幕 */
    lv_obj_t *content_label;               /* 内容标签 */
    lv_obj_t *title_label;                /* 标题标签 */
    lv_obj_t *page_label;                 /* 页码标签 */
    lv_obj_t *toc_list;                   /* 目录列表 */
    lv_obj_t *loading_msg;                /* 加载提示 */
    
    /* 【栈溢出修复】PSRAM工作缓冲区 - 持久化分配，不占用任务栈 */
    char *work_buf;           /* 8KB */
    char *html_buf;           /* 8KB - 读取HTML块 */
    char *stripped_buf;       /* 16KB - 去除标签后 */
    char *decoded_buf;        /* 16KB - 解码HTML实体后 */
    char *reflowed_buf;       /* 16KB - 文本重排后 */
    char *page_text_buf;      /* 4KB - 单页纯文本 */
    
    /* 流式解析核心：页码-文件偏移索引 */
    uint32_t *page_offsets;               /* [page_index] = temp_file_offset，每页4字节 */
    int max_pages;                        /* page_offsets数组容量 */
    int total_pages;                      /* 总页数 */
    int current_page;                     /* 当前页（0基址） */
    char temp_file_path[64];              /* 临时文件路径，如"0:/epub_temp.html" */
    
    int current_chapter;                   /* 当前章节 */
    epub_chapter_loaded_cb chapter_cb;    /* 章节加载回调 */
};

/*====================
 *   显示配置
 *====================*/

/* 屏幕尺寸 */
#define SCREEN_WIDTH   240
#define SCREEN_HEIGHT  415

/* 显示区域 */
#define CONTENT_X      10
#define CONTENT_Y      50
#define CONTENT_WIDTH  (SCREEN_WIDTH - 20)
#define CONTENT_HEIGHT (SCREEN_HEIGHT - 100)

/* 字体配置 - 使用懒加载的阅读器字体（TTF优先，回退到系统字体） */
#define FONT get_reader_font()

/* 行高和字符宽度（16号字体） */
#define LINE_HEIGHT     22
#define CHAR_WIDTH      8
#define CHARS_PER_LINE  (CONTENT_WIDTH / CHAR_WIDTH)
#define LINES_PER_PAGE  (CONTENT_HEIGHT / LINE_HEIGHT)

/*====================
 *   内部函数声明
 *====================*/

static void strip_html_tags(const char *html, int html_len, char *output, int out_size);
static void decode_html_entities(const char *input, char *output, int out_size);
static void sanitize_utf8(char *str);

/* 替换导致 LVGL 崩溃的全角中文标点符号为半角 */
static void fix_chinese_punctuation(char *str) {
    char *p = str;
    char *out = str;
    while (*p) {
        // EF BC XX 系列 (全角 ASCII)
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBC) {
            if ((unsigned char)p[2] == 0x88) { *out++ = '('; p += 3; continue; } // （
            if ((unsigned char)p[2] == 0x89) { *out++ = ')'; p += 3; continue; } // ）
            if ((unsigned char)p[2] == 0x81) { *out++ = '!'; p += 3; continue; } // ！
            if ((unsigned char)p[2] == 0x9A) { *out++ = ':'; p += 3; continue; } // ：
            if ((unsigned char)p[2] == 0x9B) { *out++ = ';'; p += 3; continue; } // ；
            if ((unsigned char)p[2] == 0x8C) { *out++ = ','; p += 3; continue; } // ，
            if ((unsigned char)p[2] == 0x9F) { *out++ = '?'; p += 3; continue; } // ？
        }
        // E3 80 XX 系列 (CJK 标点)
        if ((unsigned char)p[0] == 0xE3 && (unsigned char)p[1] == 0x80) {
            if ((unsigned char)p[2] == 0x82) { *out++ = '.'; p += 3; continue; } // 。
            if ((unsigned char)p[2] == 0x81) { *out++ = ','; p += 3; continue; } // 、
            if ((unsigned char)p[2] == 0x8A) { *out++ = '<'; p += 3; continue; } // 《
            if ((unsigned char)p[2] == 0x8B) { *out++ = '>'; p += 3; continue; } // 》
        }
        // E2 80 XX 系列 (引号等)
        if ((unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x80) {
            if ((unsigned char)p[2] == 0x9C || (unsigned char)p[2] == 0x9D) { *out++ = '"'; p += 3; continue; } // ""
            if ((unsigned char)p[2] == 0x98 || (unsigned char)p[2] == 0x99) { *out++ = '\''; p+= 3; continue; } // ''
            if ((unsigned char)p[2] == 0x94) { *out++ = '-'; p += 3; continue; } // —
        }
        *out++ = *p++;
    }
    *out = '\0';
}

/* 清理可能被意外截断的尾部无效 UTF-8 字符，防止 LVGL 崩溃 */
static void sanitize_utf8(char *str) {
    VIEW_LOG("sanitize_utf8: start\n");
    int len = strlen(str);
    VIEW_LOG("sanitize_utf8: len=%d\n", len);
    if (len == 0) {
        VIEW_LOG("sanitize_utf8: empty string, return\n");
        return;
    }
    
    int i = len - 1;
    /* 如果结尾是后续字节(10xxxxxx)，往前找先导字节 */
    while (i >= 0 && (str[i] & 0xC0) == 0x80) {
        VIEW_LOG("sanitize_utf8: i=%d, char=0x%02X is continuation\n", i, (unsigned char)str[i]);
        i--;
    }
    VIEW_LOG("sanitize_utf8: after loop i=%d\n", i);
    
    if (i >= 0) {
        int expected = 1;
        if ((str[i] & 0xE0) == 0xC0) expected = 2;
        else if ((str[i] & 0xF0) == 0xE0) expected = 3;
        else if ((str[i] & 0xF8) == 0xF0) expected = 4;
        VIEW_LOG("sanitize_utf8: lead byte 0x%02X, expected=%d\n", (unsigned char)str[i], expected);
        
        /* 如果实际余量不够一个完整的字符，果断抹掉它 */
        if (len - i < expected) {
            VIEW_LOG("sanitize_utf8: truncating at i=%d\n", i);
            str[i] = '\0';
        }
    }
    VIEW_LOG("sanitize_utf8: done\n");
}

/**
 * @brief 终极 UTF-8 字符防火墙
 * 作用：将文本中不在基础字库范围内的字符强制降级为 ASCII，
 * 防止 LVGL 因索引不到字模而触发内存越界死机。
 */
static void filter_unsupported_chars(char *str) {
    unsigned char *read_ptr = (unsigned char *)str;
    unsigned char *write_ptr = (unsigned char *)str;

    while (*read_ptr) {
        uint32_t unicode = 0;
        int char_len = 0;

        // 1. 手工解析 UTF-8 码点
        if (read_ptr[0] < 0x80) {
            unicode = read_ptr[0];
            char_len = 1;
        } else if ((read_ptr[0] & 0xE0) == 0xC0 && read_ptr[1]) {
            unicode = ((read_ptr[0] & 0x1F) << 6) | (read_ptr[1] & 0x3F);
            char_len = 2;
        } else if ((read_ptr[0] & 0xF0) == 0xE0 && read_ptr[1] && read_ptr[2]) {
            unicode = ((read_ptr[0] & 0x0F) << 12) | ((read_ptr[1] & 0x3F) << 6) | (read_ptr[2] & 0x3F);
            char_len = 3;
        } else if ((read_ptr[0] & 0xF8) == 0xF0 && read_ptr[1] && read_ptr[2] && read_ptr[3]) {
            unicode = ((read_ptr[0] & 0x07) << 18) | ((read_ptr[1] & 0x3F) << 12) | ((read_ptr[2] & 0x3F) << 6) | (read_ptr[3] & 0x3F);
            char_len = 4;
        } else {
            read_ptr++; // 非法 UTF-8，直接跳过 1 字节
            continue;
        }

        // 2. 白名单放行：保留换行(0x0A)、ASCII可见字符(0x20-0x7E)、基础汉字(0x4E00-0x9FA5)
        if (unicode == 0x0A || (unicode >= 0x20 && unicode <= 0x7E) || (unicode >= 0x4E00 && unicode <= 0x9FA5)) {
            for (int i = 0; i < char_len; i++) {
                *write_ptr++ = read_ptr[i];
            }
        }
        // 3. 黑名单强制降级
        else {
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
            else if (unicode == 0x2014) *write_ptr++ = '-';
            else if (unicode == 0x300A) *write_ptr++ = '<';
            else if (unicode == 0x300B) *write_ptr++ = '>';
            else *write_ptr++ = ' '; // 怪异符号变空格
        }
        read_ptr += char_len;
    }
    *write_ptr = '\0';
}

static void reflow_text(const char *input, int len, char *output, int out_size, int max_chars_per_line);
static int count_display_lines(const char *text);

/* 流式解析核心函数 */
static int build_page_index(EpubViewer *viewer, int chapter_index);
static void free_page_index(EpubViewer *viewer);
static void update_display(EpubViewer *viewer);

static void prev_page_handler(EpubViewer *viewer);
static void next_page_handler(EpubViewer *viewer);
static void toc_btn_cb(lv_event_t *e);
static void toc_item_cb(lv_event_t *e);
static void toc_btn_close_cb(lv_event_t *e);
static void close_viewer_cb(lv_event_t *e);
static void prev_chapter_cb(lv_event_t *e);
static void next_chapter_cb(lv_event_t *e);
static void page_prev_cb(lv_event_t *e);
static void page_next_cb(lv_event_t *e);

/*====================
 *   HTML处理
 *====================*/

/* 去除HTML标签，保留纯文本 - 防止换行堆叠版 */
static void strip_html_tags(const char *html, int html_len, char *output, int out_size) {
    int out_pos = 0;
    int in_tag = 0;
    const char *p = html;
    const char *end = html + html_len;
    
    while (p < end && out_pos < out_size - 1) {
        if (*p == '<') {
            in_tag = 1;
            /* 块级标签处理：只在前面不是换行时才加换行，防止空行堆叠 */
            if (strncmp(p, "<p", 2) == 0 || strncmp(p, "<br", 3) == 0 ||
                strncmp(p, "<div", 4) == 0 || strncmp(p, "<h", 2) == 0) {
                if (out_pos > 0 && output[out_pos - 1] != '\n') {
                    output[out_pos++] = '\n';
                }
            }
        } else if (*p == '>') {
            in_tag = 0;
        } else if (!in_tag) {
            /* 关键：过滤掉原始 HTML 中的 \r \n \t，这些应该由标签决定 */
            if (*p != '\r' && *p != '\n' && *p != '\t') {
                output[out_pos++] = *p;
            } else {
                /* 如果是原始换行，转为一个空格，避免破坏 UTF-8 或产生连续换行 */
                if (out_pos > 0 && output[out_pos - 1] != '\n' && output[out_pos - 1] != ' ') {
                    output[out_pos++] = ' ';
                }
            }
        }
        p++;
    }
    output[out_pos] = '\0';
}

/* 解码HTML实体 */
static void decode_html_entities(const char *input, char *output, int out_size) {
    int out_pos = 0;
    const char *p = input;
    
    while (*p && out_pos < out_size - 1) {
        if (*p == '&') {
            /* 检查常见实体 - 使用单个字符比较避免引号问题 */
            if (p[1] == 'n' && p[2] == 'b' && p[3] == 's' && p[4] == 'p' && p[5] == ';') {
                output[out_pos++] = ' ';
                p += 6;
            } else if (p[1] == 'l' && p[2] == 't' && p[3] == ';') {
                output[out_pos++] = '<';
                p += 4;
            } else if (p[1] == 'g' && p[2] == 't' && p[3] == ';') {
                output[out_pos++] = '>';
                p += 4;
            } else if (p[1] == 'a' && p[2] == 'm' && p[3] == 'p' && p[4] == ';') {
                output[out_pos++] = '&';
                p += 5;
            } else if (p[1] == 'q' && p[2] == 'u' && p[3] == 'o' && p[4] == 't' && p[5] == ';') {
                output[out_pos++] = '"';
                p += 6;
            } else if (p[1] == '#' && p[2] >= '0' && p[2] <= '9') {
                /* HTML数字实体 &#xxx; */
                p += 2;
                int code = 0;
                while (*p >= '0' && *p <= '9') {
                    code = code * 10 + (*p - '0');
                    p++;
                }
                if (*p == ';') p++;
                if (code > 0 && code < 0xFFFF) {
                    /* 简化处理：只处理ASCII和部分CJK */
                    if (code < 128) {
                        output[out_pos++] = (char)code;
                    } else if (code >= 0x4E00 && code <= 0x9FFF) {
                        /* 中文Unicode范围 */
                        /* UTF-8编码 */
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

/* 文本重排 - 按指定宽度分行 */
static void reflow_text(const char *input, int len, char *output, int out_size, int max_chars_per_line) {
    int out_pos = 0;
    int char_count = 0;
    int in_whitespace = 0;
    int last_space = -1;
    const char *p = input;
    const char *end = input + len;
    int first_line = 1;  /* 标记是否为首行 */
    
    while (*p && p < end && out_pos < out_size - 1) {
        /* 跳过多余的空白 */
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            if (!in_whitespace) {
                /* 单词边界 */
                last_space = out_pos;
            }
            in_whitespace = 1;
            
            /* 换行处理 */
            if (*p == '\n') {
                if (out_pos > 0 && output[out_pos - 1] != '\n') {
                    output[out_pos++] = '\n';
                }
                char_count = 0;
                first_line = 0;
                in_whitespace = 0;
            }
            p++;
            continue;
        }
        
        in_whitespace = 0;
        
        /* 首行缩进 */
        if (char_count == 0 && first_line) {
            /* 前两格缩进 */
            if (out_pos < out_size - 2) {
                output[out_pos++] = ' ';
                output[out_pos++] = ' ';
                char_count += 2;
            }
            first_line = 0;
        }
        
        /* 行尾换行判断 */
        if (char_count >= max_chars_per_line && last_space >= 0) {
            /* 在单词边界换行 */
            output[last_space] = '\n';
            out_pos = last_space + 1;
            char_count = out_pos > 0 ? 0 : 0;
            
            /* 从最后一个空格后继续 */
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            last_space = -1;
            continue;
        }
        
        output[out_pos++] = *p++;
        char_count++;
    }
    
    /* 确保以换行符结尾 */
    if (out_pos > 0 && output[out_pos - 1] != '\n') {
        output[out_pos++] = '\n';
    }
    
    output[out_pos] = '\0';
}

/* 统计显示行数 */
static int count_display_lines(const char *text) {
    int lines = 0;
    const char *p = text;
    
    while (*p) {
        if (*p == '\n') {
            lines++;
        }
        p++;
    }
    
    return lines > 0 ? lines : 1;
}

/*====================
 *   流式解析核心实现
 *====================*/

/**
 * @brief 释放页码索引内存
 */
static void free_page_index(EpubViewer *viewer) {
    if (viewer->page_offsets) {
        _dma_free(viewer->page_offsets, 0);
        viewer->page_offsets = NULL;
    }
    viewer->max_pages = 0;
    viewer->total_pages = 0;
}

/**
 * @brief 构建页码索引（精确HTML字节映射版本）
 *
 * 遍历整个临时文件，逐字节解析HTML并模拟排版。
 * 每排满一页(LINES_PER_PAGE行)，就记录当前位于HTML文件的精确字节位置。
 * 为了防止截断HTML标签，强制规定分页点只能落在标签之外。
 *
 * @return 0成功，-1失败
 */
static int build_page_index(EpubViewer *viewer, int chapter_index) {
    if (!viewer || !viewer->reader) return -1;
    
    VIEW_LOG("Building page index for chapter %d...\n", chapter_index);
    free_page_index(viewer);
    strncpy(viewer->temp_file_path, "0:/epub_temp.html", sizeof(viewer->temp_file_path) - 1);
    
    if (epub_reader_extract_chapter_to_file(viewer->reader, chapter_index, viewer->temp_file_path) != 0) {
        VIEW_ERR("Failed to extract chapter\n");
        return -1;
    }
    
    viewer->max_pages = EPUB_MAX_PAGES_PER_CHAPTER;
    viewer->page_offsets = (uint32_t*)_dma_malloc(viewer->max_pages * sizeof(uint32_t), DMAHEAP_PSRAM);
    if (!viewer->page_offsets) return -1;
    memset(viewer->page_offsets, 0, viewer->max_pages * sizeof(uint32_t));
    
    FIL temp_fp;
    if (f_open(&temp_fp, viewer->temp_file_path, FA_READ) != FR_OK) return -1;
    
    uint32_t current_html_offset = 0;
    int page_count = 1;
    viewer->page_offsets[0] = 0;
    
    int in_tag = 0;
    int line_count = 0;
    int char_count = 0;
    int chars_per_line = CHARS_PER_LINE;
    
    /* 逐块读取，但逐字节计算精确偏移量 */
    while (1) {
        UINT br = 0;
        f_read(&temp_fp, viewer->html_buf, EPUB_WORK_BUF_SIZE, &br);
        if (br == 0) break;
        
        for (UINT i = 0; i < br; i++) {
            char c = viewer->html_buf[i];
            
            if (c == '<') {
                in_tag = 1;
                /* 遇到块级标签产生换行 */
                if (i + 2 < br && (strncmp(&viewer->html_buf[i], "<p", 2) == 0 ||
                                   strncmp(&viewer->html_buf[i], "<d", 2) == 0 ||
                                   strncmp(&viewer->html_buf[i], "<b", 2) == 0 ||
                                   strncmp(&viewer->html_buf[i], "<h", 2) == 0 ||
                                   strncmp(&viewer->html_buf[i], "</p>", 4) == 0)) {
                    if (char_count > 0) {
                        line_count++;
                        char_count = 0;
                    }
                }
            } else if (c == '>') {
                in_tag = 0;
            } else if (!in_tag) {
                /* 忽略 HTML 源码中多余的换行符 */
                if ((c == '\n' || c == '\r' || c == '\t') && char_count == 0) {
                    continue;
                }
                char_count++;
                if (char_count >= chars_per_line) {
                    line_count++;
                    char_count = 0;
                }
            }
            
            /* 【核心逻辑】：如果满一页，并且当前不在 HTML 标签内部，则安全切断，记录精确偏移量！ */
            if (line_count >= LINES_PER_PAGE && !in_tag) {
                
                /* 【新增】：确保我们不在 UTF-8 字符的中间截断！
                 * UTF-8 后续字节特征是 10xxxxxx (即 & 0xC0 == 0x80)
                 * 如果下一个字节是后续字节，说明当前字符还没完，不能切！ */
                if (i + 1 < br && (viewer->html_buf[i + 1] & 0xC0) == 0x80) {
                    continue;
                }

                if (page_count < viewer->max_pages) {
                    /* 记录下一页在 HTML 文件中的精确绝对位置 */
                    viewer->page_offsets[page_count] = current_html_offset + i + 1;
                    page_count++;
                }
                line_count = 0;
                char_count = 0;
            }
        }
        current_html_offset += br;
    }
    f_close(&temp_fp);
    
    viewer->total_pages = page_count;
    VIEW_LOG("Page index built: %d pages\n", viewer->total_pages);
    return 0;
}

/*====================
 *   界面更新（精确HTML字节映射版本）
 *====================*/

/**
 * @brief 按需加载并显示当前页（终极排雷版）
 */
static void update_display(EpubViewer *viewer) {
    VIEW_LOG("=== update_display START ===\n");
    if (!viewer || !viewer->content_label) return;
    if (!viewer->page_offsets || viewer->total_pages == 0) {
        lv_label_set_text(viewer->content_label, "加载中...");
        return;
    }
    
    if (viewer->current_page < 0) viewer->current_page = 0;
    if (viewer->current_page >= viewer->total_pages) {
        viewer->current_page = viewer->total_pages - 1;
    }
    
    uint32_t start_offset = viewer->page_offsets[viewer->current_page];
    uint32_t end_offset;
    if (viewer->current_page + 1 < viewer->total_pages) {
        end_offset = viewer->page_offsets[viewer->current_page + 1];
    } else {
        end_offset = 0xFFFFFFFF;
    }
    
    VIEW_LOG("DBG 1: Offset ready (start:%u, end:%u)\n", start_offset, end_offset);
    
    FIL temp_fp;
    if (f_open(&temp_fp, viewer->temp_file_path, FA_READ) != FR_OK) {
        lv_label_set_text(viewer->content_label, "文件打开失败");
        return;
    }
    
    if (f_lseek(&temp_fp, start_offset) != FR_OK) {
        f_close(&temp_fp);
        return;
    }
    
    uint32_t read_size = end_offset - start_offset;
    if (end_offset == 0xFFFFFFFF || read_size > (EPUB_WORK_BUF_SIZE - 1)) {
        read_size = EPUB_WORK_BUF_SIZE - 1;
    }
    
    memset(viewer->html_buf, 0, EPUB_WORK_BUF_SIZE);
    UINT br = 0;
    f_read(&temp_fp, viewer->html_buf, read_size, &br);
    f_close(&temp_fp);
    
    VIEW_LOG("DBG 2: File read done (br:%u)\n", br);
    
    if (br == 0 && read_size > 0) {
        lv_label_set_text(viewer->content_label, "读取结束");
        return;
    }
    
    memset(viewer->stripped_buf, 0, EPUB_WORK_BUF_SIZE * 2);
    strip_html_tags(viewer->html_buf, br, viewer->stripped_buf, EPUB_WORK_BUF_SIZE * 2);
    VIEW_LOG("DBG 3: strip_html_tags done\n");
    
    memset(viewer->decoded_buf, 0, EPUB_WORK_BUF_SIZE * 2);
    decode_html_entities(viewer->stripped_buf, viewer->decoded_buf, EPUB_WORK_BUF_SIZE * 2);
    VIEW_LOG("DBG 4: decode_html_entities done\n");
    
    memset(viewer->page_text_buf, 0, EPUB_PAGE_TEXT_SIZE);
    int decoded_len = strlen(viewer->decoded_buf);
    if (decoded_len >= EPUB_PAGE_TEXT_SIZE) {
        decoded_len = EPUB_PAGE_TEXT_SIZE - 1;
    }
    
    int copy_start = 0;
    if (viewer->current_page + 1 < viewer->total_pages) {
        if (decoded_len > 0) {
            unsigned char last_byte = (unsigned char)viewer->decoded_buf[decoded_len - 1];
            while (decoded_len > 0 && (last_byte >= 0x80 && last_byte <= 0xBF)) {
                decoded_len--;
                if (decoded_len > 0) {
                    last_byte = (unsigned char)viewer->decoded_buf[decoded_len - 1];
                }
            }
        }
    }
    
    memcpy(viewer->page_text_buf, viewer->decoded_buf + copy_start, decoded_len);
    VIEW_LOG("DBG 5: text copied to page_text_buf\n");
    
    sanitize_utf8(viewer->page_text_buf);
    VIEW_LOG("DBG 6: sanitize_utf8 done, len=%u\n", (unsigned)strlen(viewer->page_text_buf));
    
    // 强杀全角标点保护 LVGL 渲染器！
    fix_chinese_punctuation(viewer->page_text_buf);
    VIEW_LOG("DBG 6.1: fix_chinese_punctuation done\n");
    
    /* 修剪首尾空行 */
    char *clean_text = viewer->page_text_buf;
    while (*clean_text == '\n' || *clean_text == ' ' || *clean_text == '\r') {
        clean_text++;
    }
    VIEW_LOG("DBG 7: final text len=%u\n", (unsigned)strlen(clean_text));
    
    /* 终极 UTF-8 防火墙：强制降级所有超出字库范围的字符 */
    filter_unsupported_chars(clean_text);
    VIEW_LOG("DBG 7.1: filter_unsupported_chars done, final len=%u\n", (unsigned)strlen(clean_text));
    
    /* ======== 字库探针 + 安全换行排版 ======== */

    lv_font_t *font = get_reader_font();
    lv_obj_set_style_text_font(viewer->content_label, font, 0);

    int in_idx = 0;
    int out_idx = 0;
    int line_width_count = 0;
    int max_line_width = 26;

    memset(viewer->reflowed_buf, 0, EPUB_WORK_BUF_SIZE * 2);
    lv_font_glyph_dsc_t dummy_dsc;
    memset(&dummy_dsc, 0, sizeof(lv_font_glyph_dsc_t));

    while (clean_text[in_idx] != '\0' && out_idx < (EPUB_WORK_BUF_SIZE * 2 - 5)) {
        unsigned char head = (unsigned char)clean_text[in_idx];
        int char_bytes = 1;
        uint32_t unicode = head;

        if ((head & 0x80) == 0) { char_bytes = 1; }
        else if ((head & 0xE0) == 0xC0) {
            char_bytes = 2;
            unicode = ((head & 0x1F) << 6) | (clean_text[in_idx+1] & 0x3F);
        } else if ((head & 0xF0) == 0xE0) {
            char_bytes = 3;
            unicode = ((head & 0x0F) << 12) | ((clean_text[in_idx+1] & 0x3F) << 6) | (clean_text[in_idx+2] & 0x3F);
        } else if ((head & 0xF8) == 0xF0) {
            char_bytes = 4;
            unicode = ((head & 0x07) << 18) | ((clean_text[in_idx+1] & 0x3F) << 12) | ((clean_text[in_idx+2] & 0x3F) << 6) | (clean_text[in_idx+3] & 0x3F);
        }

        bool glyph_exists = true;
        if (head != '\n' && unicode > 0x7E) {
            if (lv_font_get_glyph_dsc(font, &dummy_dsc, unicode, 0) == false) {
                glyph_exists = false;
            }
        }

        if (glyph_exists) {
            for (int i = 0; i < char_bytes && clean_text[in_idx] != '\0'; i++) {
                viewer->reflowed_buf[out_idx++] = clean_text[in_idx++];
            }
            if (head != '\n') line_width_count += (char_bytes > 1 ? 2 : 1);
        } else {
            viewer->reflowed_buf[out_idx++] = ' ';
            in_idx += char_bytes;
            line_width_count += 1;
        }

        if (head == '\n') {
            line_width_count = 0;
        } else if (line_width_count >= max_line_width) {
            viewer->reflowed_buf[out_idx++] = '\n';
            line_width_count = 0;
        }
    }
    viewer->reflowed_buf[out_idx] = '\0';

    VIEW_LOG("DBG 8: Text safe! Rendering...\n");
    
    // 最终上屏
    lv_label_set_long_mode(viewer->content_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(viewer->content_label, viewer->reflowed_buf);
    
    VIEW_LOG("DBG 9: FULL SURVIVAL!!!\n");
    
    /* 更新页码与标题 */
    char page_str[64];
    snprintf(page_str, sizeof(page_str), "%d/%d", viewer->current_page + 1, viewer->total_pages);
    lv_label_set_text(viewer->page_label, page_str);
    
    char title_str[64];
    snprintf(title_str, sizeof(title_str), "第%d章", viewer->current_chapter + 1);
    lv_label_set_text(viewer->title_label, title_str);
    
    epd_mark_refresh_pending();
    VIEW_LOG("=== update_display END ===\n");
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
        /* 切换到上一章 */
        viewer->current_chapter--;
        if (viewer->chapter_cb) {
            viewer->chapter_cb(viewer->current_chapter, viewer->reader ? viewer->reader->spine_count : 0);
        }
    }
}

static void next_page_handler(EpubViewer *viewer) {
    if (!viewer) return;
    
    if (viewer->current_page < viewer->total_pages - 1) {
        viewer->current_page++;
        update_display(viewer);
    } else if (viewer->reader && viewer->current_chapter < viewer->reader->spine_count - 1) {
        /* 切换到下一章 */
        viewer->current_chapter++;
        if (viewer->chapter_cb) {
            viewer->chapter_cb(viewer->current_chapter, viewer->reader->spine_count);
        }
    }
}

/*====================
 *   触摸事件处理
 *====================*/

/* 按键区域定义 */
#define BTN_PREV_X     0
#define BTN_PREV_Y     200
#define BTN_PREV_W     80
#define BTN_PREV_H     100

#define BTN_NEXT_X     160
#define BTN_NEXT_Y     200
#define BTN_NEXT_W     80
#define BTN_NEXT_H     100

#define BTN_TOC_X      80
#define BTN_TOC_Y      200
#define BTN_TOC_W      80
#define BTN_TOC_H      100

/* 触摸事件处理 */
static void content_area_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *screen = lv_event_get_target(e);
    EpubViewer *viewer = (EpubViewer*)lv_obj_get_user_data(screen);

    if (!viewer) return;
    
    /* 获取触摸点 */
    lv_point_t point;
    lv_indev_get_point(lv_indev_get_act(), &point);
    
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_RELEASED) {
        int x = point.x;
        int y = point.y;
        
        /* 过滤非内容区域 */
        if (x < CONTENT_X || x > CONTENT_X + CONTENT_WIDTH ||
            y < CONTENT_Y || y > CONTENT_Y + CONTENT_HEIGHT) {
            return;
        }
        
        /* 根据触摸位置决定操作 */
        if (x < 80) {
            /* 左侧 - 上一页 */
            prev_page_handler(viewer);
        } else if (x > 160) {
            /* 右侧 - 下一页 */
            next_page_handler(viewer);
        } else {
            /* 中间 - 显示/隐藏目录 */
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

    /* 按钮是 lv_list 内部对象的子节点，遍历其兄弟找索引 */
    lv_obj_t *parent = lv_obj_get_parent(btn);
    uint32_t btn_idx = 0;
    uint32_t child_cnt = lv_obj_get_child_cnt(parent);
    for (uint32_t i = 0; i < child_cnt; i++) {
        if (lv_obj_get_child(parent, i) == btn) {
            btn_idx = i;
            break;
        }
    }

    if (viewer->reader) {
        int chapter = epub_reader_jump_to_toc(viewer->reader, btn_idx);
        if (chapter >= 0) {
            epub_viewer_goto_chapter(viewer, chapter);
        }
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
 *   其他回调
 *====================*/

static void close_viewer_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    EpubViewer *viewer = (EpubViewer*)lv_obj_get_user_data(btn);
    if (viewer) {
        epub_viewer_close(viewer);
    }
}

static void prev_chapter_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    EpubViewer *viewer = (EpubViewer*)lv_obj_get_user_data(btn);
    if (viewer && viewer->current_chapter > 0) {
        epub_viewer_goto_chapter(viewer, viewer->current_chapter - 1);
    }
}

static void next_chapter_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    EpubViewer *viewer = (EpubViewer*)lv_obj_get_user_data(btn);
    if (viewer && viewer->reader && viewer->current_chapter < viewer->reader->spine_count - 1) {
        epub_viewer_goto_chapter(viewer, viewer->current_chapter + 1);
    }
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

/*====================
 *   公共API实现
 *====================*/

EpubViewer* epub_viewer_create(EpubReader *reader) {
    EpubViewer *viewer;
    viewer = (EpubViewer*)_dma_malloc(sizeof(EpubViewer), DMAHEAP_PSRAM);
    if (!viewer) {
        VIEW_LOG("Alloc failed\n");
        return NULL;
    }
    
    memset(viewer, 0, sizeof(EpubViewer));
    viewer->reader = reader;
    viewer->current_chapter = 0;
    viewer->current_page = 0;
    viewer->total_pages = 1;
    
    /* 【栈溢出修复】分配多个PSRAM缓冲区，避免函数内大型局部数组 */
    viewer->work_buf = (char*)_dma_malloc(EPUB_WORK_BUF_SIZE, DMAHEAP_PSRAM);
    viewer->html_buf = (char*)_dma_malloc(EPUB_WORK_BUF_SIZE, DMAHEAP_PSRAM);
    viewer->stripped_buf = (char*)_dma_malloc(EPUB_WORK_BUF_SIZE * 2, DMAHEAP_PSRAM);
    viewer->decoded_buf = (char*)_dma_malloc(EPUB_WORK_BUF_SIZE * 2, DMAHEAP_PSRAM);
    viewer->reflowed_buf = (char*)_dma_malloc(EPUB_WORK_BUF_SIZE * 2, DMAHEAP_PSRAM);
    viewer->page_text_buf = (char*)_dma_malloc(EPUB_PAGE_TEXT_SIZE, DMAHEAP_PSRAM);
    
    if (!viewer->work_buf || !viewer->html_buf || !viewer->stripped_buf ||
        !viewer->decoded_buf || !viewer->reflowed_buf || !viewer->page_text_buf) {
        VIEW_LOG("PSRAM buffer alloc failed\n");
        if (viewer->work_buf) _dma_free(viewer->work_buf, 0);
        if (viewer->html_buf) _dma_free(viewer->html_buf, 0);
        if (viewer->stripped_buf) _dma_free(viewer->stripped_buf, 0);
        if (viewer->decoded_buf) _dma_free(viewer->decoded_buf, 0);
        if (viewer->reflowed_buf) _dma_free(viewer->reflowed_buf, 0);
        if (viewer->page_text_buf) _dma_free(viewer->page_text_buf, 0);
        _dma_free(viewer, 0);
        return NULL;
    }
    
    memset(viewer->work_buf, 0, EPUB_WORK_BUF_SIZE);
    memset(viewer->html_buf, 0, EPUB_WORK_BUF_SIZE);
    memset(viewer->stripped_buf, 0, EPUB_WORK_BUF_SIZE * 2);
    memset(viewer->decoded_buf, 0, EPUB_WORK_BUF_SIZE * 2);
    memset(viewer->reflowed_buf, 0, EPUB_WORK_BUF_SIZE * 2);
    memset(viewer->page_text_buf, 0, EPUB_PAGE_TEXT_SIZE);
    
    VIEW_LOG("Using PSRAM buffers (work:8KB, html:8KB, stripped:16KB, decoded:16KB, reflowed:16KB, page_text:4KB)\n");
    
    /* 设置临时文件默认路径 */
    strncpy(viewer->temp_file_path, "0:/epub_temp.html", sizeof(viewer->temp_file_path) - 1);
    
    return viewer;
}

void epub_viewer_show(EpubViewer *viewer) {
    if (!viewer) return;
    
    viewer->screen = lv_obj_create(NULL);
    lv_obj_set_size(viewer->screen, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(viewer->screen, lv_color_white(), 0);
    lv_obj_clear_flag(viewer->screen, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN);
    epd_disable_all_animations_recursive(viewer->screen);
    
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
    lv_obj_set_style_border_width(back_btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(back_btn, 1, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(back_btn, 0, LV_STATE_FOCUSED);
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
    
    viewer->content_label = lv_label_create(viewer->screen);
    lv_obj_set_size(viewer->content_label, CONTENT_WIDTH, CONTENT_HEIGHT);
    lv_obj_align(viewer->content_label, LV_ALIGN_TOP_LEFT, CONTENT_X, CONTENT_Y);
    lv_obj_set_style_text_font(viewer->content_label, FONT, 0);
    lv_obj_set_style_text_color(viewer->content_label, lv_color_black(), 0);
    lv_label_set_long_mode(viewer->content_label, LV_LABEL_LONG_CLIP);
    
    lv_obj_set_user_data(viewer->screen, viewer);
    lv_obj_add_event_cb(viewer->screen, content_area_event_cb, LV_EVENT_CLICKED, viewer);
    
    lv_obj_t *nav_bar = lv_obj_create(viewer->screen);
    lv_obj_set_size(nav_bar, SCREEN_WIDTH, 50);
    lv_obj_align(nav_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(nav_bar, lv_color_white(), 0);
    epd_disable_all_animations_recursive(nav_bar);

    /* 第一行：翻页键 */
    lv_obj_t *page_prev_btn = lv_btn_create(nav_bar);
    lv_obj_set_size(page_prev_btn, 55, 22);
    lv_obj_align(page_prev_btn, LV_ALIGN_TOP_LEFT, 5, 2);
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

    lv_obj_t *page_next_btn = lv_btn_create(nav_bar);
    lv_obj_set_size(page_next_btn, 55, 22);
    lv_obj_align(page_next_btn, LV_ALIGN_TOP_RIGHT, -5, 2);
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

    /* 第二行：章导航键 */
    lv_obj_t *prev_btn = lv_btn_create(nav_bar);
    lv_obj_set_size(prev_btn, 70, 22);
    lv_obj_align(prev_btn, LV_ALIGN_BOTTOM_LEFT, 5, 0);
    lv_obj_set_style_border_width(prev_btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(prev_btn, 1, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(prev_btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_user_data(prev_btn, viewer);
    lv_obj_add_event_cb(prev_btn, prev_chapter_cb, LV_EVENT_CLICKED, viewer);
    epd_disable_all_animations_recursive(prev_btn);

    lv_obj_t *prev_label = lv_label_create(prev_btn);
    lv_label_set_text(prev_label, "上一章");
    lv_obj_set_style_text_font(prev_label, FONT, 0);
    lv_obj_center(prev_label);

    lv_obj_t *toc_btn_obj = lv_btn_create(nav_bar);
    lv_obj_set_size(toc_btn_obj, 50, 22);
    lv_obj_align(toc_btn_obj, LV_ALIGN_BOTTOM_MID, 0, 0);
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

    lv_obj_t *next_btn = lv_btn_create(nav_bar);
    lv_obj_set_size(next_btn, 70, 22);
    lv_obj_align(next_btn, LV_ALIGN_BOTTOM_RIGHT, -5, 0);
    lv_obj_set_style_border_width(next_btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(next_btn, 1, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(next_btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_user_data(next_btn, viewer);
    lv_obj_add_event_cb(next_btn, next_chapter_cb, LV_EVENT_CLICKED, viewer);
    epd_disable_all_animations_recursive(next_btn);

    lv_obj_t *next_label = lv_label_create(next_btn);
    lv_label_set_text(next_label, "下一章");
    lv_obj_set_style_text_font(next_label, FONT, 0);
    lv_obj_center(next_label);
    
    lv_disp_load_scr(viewer->screen);
    
    VIEW_LOG("Viewer shown\n");
}

static void toc_btn_cb(lv_event_t *e) {
    EpubViewer *viewer = (EpubViewer*)lv_event_get_user_data(e);
    if (viewer) {
        epub_viewer_show_toc(viewer);
    }
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
    
    viewer->content_label = NULL;
    viewer->title_label = NULL;
    viewer->page_label = NULL;
}

void epub_viewer_destroy(EpubViewer *viewer) {
    if (viewer) {
        epub_viewer_close(viewer);
        /* 释放页码索引 */
        free_page_index(viewer);
        /* 【栈溢出修复】释放所有PSRAM缓冲区 */
        if (viewer->work_buf) _dma_free(viewer->work_buf, 0);
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

/**
 * @brief 跳转到指定章节
 * 
 * 核心变化：不再将整个章节加载到内存，
 * 而是：1. 流式解压到SD卡临时文件  2. 构建页码索引  3. 显示第一页
 */
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
    
    /* 构建页码索引（流式解析核心） */
    if (build_page_index(viewer, chapter_index) != 0) {
        VIEW_ERR("Failed to build page index for chapter %d\n", chapter_index);
        /* 降级处理：显示错误信息 */
        lv_label_set_text(viewer->content_label, "章节加载失败");
        viewer->total_pages = 1;
        viewer->current_page = 0;
        return false;
    }
    
    /* 更新显示 */
    update_display(viewer);
    
    VIEW_LOG("Chapter %d ready: %d pages\n", chapter_index, viewer->total_pages);
    
    return true;
}

void epub_viewer_show_toc(EpubViewer *viewer) {
    if (!viewer || !viewer->reader || !viewer->screen) return;
    
    /* 关闭已有目录 */
    if (viewer->toc_list) {
        lv_obj_del_async(viewer->toc_list);
        viewer->toc_list = NULL;
        return;
    }
    
    /* 创建目录覆盖层 */
    viewer->toc_list = lv_obj_create(viewer->screen);
    lv_obj_set_size(viewer->toc_list, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_align(viewer->toc_list, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(viewer->toc_list, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(viewer->toc_list, LV_OPA_COVER, 0);
    
    /* 目录标题 */
    lv_obj_t *toc_title = lv_label_create(viewer->toc_list);
    lv_label_set_text(toc_title, "目录");
    lv_obj_set_style_text_font(toc_title, FONT, 0);
    lv_obj_align(toc_title, LV_ALIGN_TOP_MID, 0, 10);
    
    /* 关闭按钮 */
    lv_obj_t *close_btn = lv_btn_create(viewer->toc_list);
    lv_obj_set_size(close_btn, 50, 30);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -10, 5);
    lv_obj_set_user_data(close_btn, viewer);
    lv_obj_add_event_cb(close_btn, toc_btn_close_cb, LV_EVENT_CLICKED, viewer);
    
    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "关闭");
    lv_obj_set_style_text_font(close_label, FONT, 0);
    lv_obj_center(close_label);
    
    /* 创建目录列表 */
    int toc_count = epub_reader_get_toc_count(viewer->reader);
    if (toc_count > 0) {
        /* 使用列表控件 */
        lv_obj_t *list = lv_list_create(viewer->toc_list);
        lv_obj_set_size(list, SCREEN_WIDTH - 20, SCREEN_HEIGHT - 100);
        lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_set_style_pad_row(list, 2, 0);
        
        /* 添加目录项 */
        for (int i = 0; i < toc_count && i < 20; i++) {
            EpubTocEntry *toc = epub_reader_get_toc(viewer->reader, i);
            if (toc) {
                char btn_text[140];
                snprintf(btn_text, sizeof(btn_text), "%s", toc->title);
                
                lv_obj_t *btn = lv_list_add_btn(list, NULL, btn_text);
                lv_obj_set_style_text_font(btn, FONT, 0);
                lv_obj_set_user_data(btn, viewer);
                lv_obj_add_event_cb(btn, toc_item_cb, LV_EVENT_CLICKED, NULL);
            }
        }
    } else {
        /* 无目录时显示章节列表 */
        lv_obj_t *info = lv_label_create(viewer->toc_list);
        lv_label_set_text(info, "无目录信息\n\n显示章节列表");
        lv_obj_set_style_text_font(info, FONT, 0);
        lv_obj_align(info, LV_ALIGN_CENTER, 0, 0);
    }
    
    VIEW_LOG("TOC shown, %d entries\n", toc_count);
    epd_mark_refresh_pending(); // 【新增】
}

int epub_viewer_get_current_chapter(EpubViewer *viewer) {
    return viewer ? viewer->current_chapter : 0;
}

int epub_viewer_get_current_page(EpubViewer *viewer) {
    return viewer ? viewer->current_page + 1 : 1;
}

int epub_viewer_get_total_pages(EpubViewer *viewer) {
    return viewer ? viewer->total_pages : 1;
}

void epub_viewer_set_chapter_loaded_cb(EpubViewer *viewer, epub_chapter_loaded_cb cb) {
    if (viewer) {
        viewer->chapter_cb = cb;
    }
}

/*====================
 *   HTML处理API
 *====================*/

int epub_strip_html_tags(const char *html, int len, char *output, int out_size) {
    strip_html_tags(html, len, output, out_size);
    return strlen(output);
}

int epub_decode_html_entities(const char *text, char *output, int out_size) {
    decode_html_entities(text, output, out_size);
    return strlen(output);
}
