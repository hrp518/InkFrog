#include "epub_viewer.h"
#include "epub_xhtml_parser.h"
#include "file_manager.h"
#include "lv_port_disp.h"
#include "lvgl/lvgl.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
/* #include <sys/dma_heap.h>  -- 不再使用 DMA heap，改用静态分配 (.psram_bss) */
#include "fs/fatfs/ff.h"
#include "FreeRTOS.h"
#include "task.h"

/* 静态分配到 .psram_bss 段，不走 LVGL 堆，避免碎片化导致分配失败 */
static char s_whole_xhtml_buf[WHOLE_XHTML_BUF_SIZE]  __attribute__((section(".psram_bss")));
static char s_decoded_text_buf[DECODED_TEXT_BUF_SIZE] __attribute__((section(".psram_bss")));

extern void epd_mark_refresh_pending(void);
extern void epd_disable_all_animations_recursive(lv_obj_t *obj);

#define VIEWER_DEBUG 1
#if VIEWER_DEBUG
#define VIEW_LOG(fmt, ...) printf("[VIEWER] " fmt, ##__VA_ARGS__)
#define VIEW_ERR(fmt, ...) printf("[VIEWER ERR] " fmt, ##__VA_ARGS__)
#else
#define VIEW_LOG(fmt, ...)
#define VIEW_ERR(fmt, ...)
#endif

/* ========== 布局常量 ========== */

#define SCREEN_WIDTH   240
#define SCREEN_HEIGHT  415

/* 内容区：几乎全屏，顶部8px padding，底部留28px给进度条+百分比 */
#define CONTENT_X      10
#define CONTENT_Y      8
#define CONTENT_WIDTH  (SCREEN_WIDTH - 20)
#define CONTENT_HEIGHT (SCREEN_HEIGHT - 36)
/* 工具栏 */
#define TOOLBAR_HEIGHT    100
#define TOOLBAR_TRIGGER_Y 25

/* 偏移历史栈 */
#define OFFSET_HISTORY_SIZE 32

/* 字体 */
#define FONT       get_reader_font()
#define FONT_H1    get_reader_font_h1()
#define FONT_H2    get_reader_font_h2()
#define FONT_H3    get_reader_font_h3()

/* 行高 */
#define LH_BODY    22
#define LH_H3      26
#define LH_H2      30
#define LH_H1      34

/* 估算每页字节数（用于上一页回退估算） */
#define ESTIMATED_PAGE_BYTES 3000

LV_FONT_DECLARE(lv_font_misans_16);

/* ========== EpubViewer 结构体 ========== */

struct EpubViewer {
    EpubReader *reader;
    lv_obj_t *screen;
    lv_obj_t *content_container;

    /* 字节偏移定位系统 */
    int read_offset;           /* 当前渲染起始位置（decoded_text_buf偏移） */
    int page_end_offset;       /* 当前页渲染结束位置 */
    int chapter_len;           /* 当前章节解码后总长度 */
    int prev_page_start;       /* 上一页起始偏移（简单回退） */

    /* 偏移历史栈（用于连续回退多页） */
    int offset_history[OFFSET_HISTORY_SIZE];
    int history_head;
    int history_count;

    /* 阅读内容缓冲区 */
    char *whole_xhtml_buf;
    int whole_xhtml_len;
    char *decoded_text_buf;
    int decoded_text_len;

    int current_chapter;
    epub_chapter_loaded_cb chapter_cb;
    epub_close_callback_t close_cb;

    /* UI 元素 - 阅读界面 */
    lv_obj_t *pct_indicator;   /* 底部百分比文字 */
    lv_obj_t *progress_bar;    /* 底部进度条填充 */
    lv_obj_t *progress_bg;     /* 底部进度条背景 */

    /* UI 元素 - 工具栏 */
    lv_obj_t *toolbar;
    lv_obj_t *toolbar_title;
    lv_obj_t *toolbar_pct_label;
    lv_obj_t *toolbar_goto_ta; /* 工具栏内的百分比输入框 */
    bool toolbar_visible;

    /* UI 元素 - 目录 */
    lv_obj_t *toc_overlay;

    /* UI 元素 - 跳转键盘 */
    lv_obj_t *goto_screen;
    lv_obj_t *goto_ta;
    lv_obj_t *goto_kb;
};

extern lv_font_t *get_reader_font(void);
extern lv_font_t *get_reader_font_h1(void);
extern lv_font_t *get_reader_font_h2(void);
extern lv_font_t *get_reader_font_h3(void);

/* ========== 前向声明 ========== */

static void update_display(EpubViewer *viewer);
static void prev_page_handler(EpubViewer *viewer);
static void next_page_handler(EpubViewer *viewer);
static void content_area_event_cb(lv_event_t *e);
static void close_viewer_cb(lv_event_t *e);
static void toc_item_cb(lv_event_t *e);
static void toc_close_cb(lv_event_t *e);
static void toc_btn_cb(lv_event_t *e);
static void toolbar_close_cb(lv_event_t *e);
static void prev_chapter_cb(lv_event_t *e);
static void next_chapter_cb(lv_event_t *e);
static void goto_ta_clicked_cb(lv_event_t *e);
static void goto_btn_cb(lv_event_t *e);
static void goto_cancel_cb(lv_event_t *e);
static void goto_confirm_cb(lv_event_t *e);
static void filter_unsupported_chars_ex(char *str, bool preserve_markers);
static void filter_unsupported_chars(char *str);
static void toggle_toolbar(EpubViewer *viewer);
static void create_toolbar(EpubViewer *viewer);
static void update_toolbar_info(EpubViewer *viewer);
static void show_goto_keyboard(EpubViewer *viewer);
static void cleanup_goto_screen(EpubViewer *viewer);

/* ========== 偏移历史栈 ========== */

static void history_push(EpubViewer *viewer, int offset) {
    if (!viewer) return;
    viewer->offset_history[viewer->history_head] = offset;
    viewer->history_head = (viewer->history_head + 1) % OFFSET_HISTORY_SIZE;
    if (viewer->history_count < OFFSET_HISTORY_SIZE) viewer->history_count++;
}

static int history_pop(EpubViewer *viewer) {
    if (!viewer || viewer->history_count == 0) return -1;
    viewer->history_head = (viewer->history_head - 1 + OFFSET_HISTORY_SIZE) % OFFSET_HISTORY_SIZE;
    viewer->history_count--;
    return viewer->offset_history[viewer->history_head];
}

static void history_clear(EpubViewer *viewer) {
    if (!viewer) return;
    viewer->history_head = 0;
    viewer->history_count = 0;
}

/* ========== 百分比计算 ========== */

static float get_read_percentage(EpubViewer *viewer) {
    if (!viewer || viewer->chapter_len <= 0) return 0.0f;
    /* 使用page_end_offset作为当前阅读进度，因为read_offset在渲染后会被更新 */
    int pos = viewer->page_end_offset;
    if (pos <= 0) return 0.0f;
    if (pos >= viewer->chapter_len) return 100.0f;
    return (float)pos * 100.0f / (float)viewer->chapter_len;
}

/**
 * @brief 将百分比(×100整数)转换为有效的UTF-8字节偏移
 *
 * 处理逻辑:
 * 1. 百分比 → 原始字节偏移
 * 2. UTF-8对齐：跳过continuation bytes
 * 3. 换行对齐：前进到下一个换行符后（避免段落截断）
 */
static int pct_to_offset(EpubViewer *viewer, int pct_x100) {
    if (!viewer || viewer->chapter_len <= 0) return 0;

    /* 限制范围 0~10000 */
    if (pct_x100 < 0) pct_x100 = 0;
    if (pct_x100 > 10000) pct_x100 = 10000;

    int raw_offset = (int)((long long)viewer->chapter_len * pct_x100 / 10000);

    if (raw_offset <= 0) return 0;
    if (raw_offset >= viewer->chapter_len) return viewer->chapter_len - 1;

    const char *buf = viewer->decoded_text_buf;

    /* Step 1: UTF-8对齐 - 跳过continuation bytes (10xxxxxx) */
    while (raw_offset < viewer->chapter_len &&
           (buf[raw_offset] & 0xC0) == 0x80) {
        raw_offset++;
    }

    /* Step 2: 字体标记对齐 - 避免落在 0x02/0x03 标记中间 */
    if (raw_offset >= 1 && raw_offset < viewer->chapter_len &&
        (unsigned char)buf[raw_offset - 1] == 0x02 &&
        buf[raw_offset] >= '0' && buf[raw_offset] <= '3') {
        /* 在标记开头，跳过完整标记 0x02 level 0x03 */
        raw_offset += 2;
    }
    if (raw_offset < viewer->chapter_len &&
        (unsigned char)buf[raw_offset] == 0x03) {
        raw_offset++;  /* 跳过标记结尾 */
    }

    /* Step 3: 换行对齐 - 找下一个换行符，从新行开始 */
    if (raw_offset > 0 && buf[raw_offset - 1] != '\n') {
        int search_limit = raw_offset + 300;
        if (search_limit > viewer->chapter_len) search_limit = viewer->chapter_len;
        for (int i = raw_offset; i < search_limit; i++) {
            if (buf[i] == '\n') {
                raw_offset = i + 1;
                break;
            }
        }
        /* 如果300字节内没找到换行符，保持UTF-8对齐位置即可 */
    }

    if (raw_offset >= viewer->chapter_len) raw_offset = viewer->chapter_len - 1;
    return raw_offset;
}

/* ========== 字符过滤 ========== */

static void filter_unsupported_chars_ex(char *str, bool preserve_markers) {
    unsigned char *read_ptr = (unsigned char *)str;
    unsigned char *write_ptr = (unsigned char *)str;

    while (*read_ptr) {
        if (read_ptr[0] == 0x02 && read_ptr[1] >= '0' && read_ptr[1] <= '3' && read_ptr[2] == 0x03) {
            if (preserve_markers) {
                *write_ptr++ = *read_ptr++;
                *write_ptr++ = *read_ptr++;
                *write_ptr++ = *read_ptr++;
            } else {
                read_ptr += 3;
            }
            continue;
        }
        if (!preserve_markers && read_ptr[0] == 0x03) {
            read_ptr++;
            continue;
        }

        uint32_t unicode = 0;
        int char_len = 0;

        if (read_ptr[0] < 0x80) {
            unicode = read_ptr[0]; char_len = 1;
        } else if ((read_ptr[0] & 0xE0) == 0xC0 && (read_ptr[1] & 0xC0) == 0x80) {
            unicode = ((read_ptr[0] & 0x1F) << 6) | (read_ptr[1] & 0x3F); char_len = 2;
        } else if ((read_ptr[0] & 0xF0) == 0xE0 && (read_ptr[1] & 0xC0) == 0x80 && (read_ptr[2] & 0xC0) == 0x80) {
            unicode = ((read_ptr[0] & 0x0F) << 12) | ((read_ptr[1] & 0x3F) << 6) | (read_ptr[2] & 0x3F); char_len = 3;
        } else if ((read_ptr[0] & 0xF8) == 0xF0 && (read_ptr[1] & 0xC0) == 0x80 && (read_ptr[2] & 0xC0) == 0x80 && (read_ptr[3] & 0xC0) == 0x80) {
            unicode = ((read_ptr[0] & 0x07) << 18) | ((read_ptr[1] & 0x3F) << 12) | ((read_ptr[2] & 0x3F) << 6) | (read_ptr[3] & 0x3F); char_len = 4;
        } else {
            read_ptr++;
            continue;
        }

        if (unicode == 0x0A || unicode == 0xE000 ||
            (unicode >= 0x20 && unicode <= 0x7E) ||
            (unicode >= 0x4E00 && unicode <= 0x9FA5) ||
            (unicode >= 0x3000 && unicode <= 0x303F)) {
            for (int i = 0; i < char_len; i++) *write_ptr++ = read_ptr[i];
        } else {
            if (unicode == 0xFF08) *write_ptr++ = '(';
            else if (unicode == 0xFF09) *write_ptr++ = ')';
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

static void filter_unsupported_chars(char *str) {
    filter_unsupported_chars_ex(str, false);
}

/* ========== XHTML解码回调 ========== */

static void decode_start_cb(const char *name, const char **atts, void *user_data) {
    EpubViewer *v = (EpubViewer *)user_data;
    if (!v) return;
    int remaining = DECODED_TEXT_BUF_SIZE - v->decoded_text_len - 10;
    if (remaining < 3) return;

    if (strcmp(name, "h1") == 0) {
        v->decoded_text_buf[v->decoded_text_len++] = 0x02;
        v->decoded_text_buf[v->decoded_text_len++] = '1';
        v->decoded_text_buf[v->decoded_text_len++] = 0x03;
    } else if (strcmp(name, "h2") == 0) {
        v->decoded_text_buf[v->decoded_text_len++] = 0x02;
        v->decoded_text_buf[v->decoded_text_len++] = '2';
        v->decoded_text_buf[v->decoded_text_len++] = 0x03;
    } else if (strcmp(name, "h3") == 0) {
        v->decoded_text_buf[v->decoded_text_len++] = 0x02;
        v->decoded_text_buf[v->decoded_text_len++] = '3';
        v->decoded_text_buf[v->decoded_text_len++] = 0x03;
    } else if (strcmp(name, "p") == 0 || strcmp(name, "div") == 0 || strcmp(name, "body") == 0) {
        v->decoded_text_buf[v->decoded_text_len++] = 0x02;
        v->decoded_text_buf[v->decoded_text_len++] = '0';
        v->decoded_text_buf[v->decoded_text_len++] = 0x03;
    }
}

static void decode_end_cb(const char *name, void *user_data) {
    EpubViewer *v = (EpubViewer *)user_data;
    if (!v) return;
    int remaining = DECODED_TEXT_BUF_SIZE - v->decoded_text_len - 10;
    if (remaining < 1) return;

    if (strcmp(name, "p") == 0 || strcmp(name, "div") == 0 ||
        strcmp(name, "h1") == 0 || strcmp(name, "h2") == 0 || strcmp(name, "h3") == 0) {
        v->decoded_text_buf[v->decoded_text_len++] = '\n';
    }
    if (strcmp(name, "li") == 0 || strcmp(name, "td") == 0 || strcmp(name, "br") == 0) {
        v->decoded_text_buf[v->decoded_text_len++] = '\n';
    }
}

static void decode_char_cb(const char *data, int len, void *user_data) {
    EpubViewer *v = (EpubViewer *)user_data;
    if (!v || len <= 0) return;

    if (v->decoded_text_len + len >= DECODED_TEXT_BUF_SIZE - 10) {
        len = DECODED_TEXT_BUF_SIZE - 10 - v->decoded_text_len;
        if (len <= 0) return;
    }

    memcpy(v->decoded_text_buf + v->decoded_text_len, data, len);
    v->decoded_text_len += len;
}

/* ========== UTF-8 工具函数 ========== */

static int utf8_char_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static uint32_t utf8_to_unicode(const char *p) {
    const unsigned char *s = (const unsigned char *)p;
    if (s[0] < 0x80) return s[0];
    if ((s[0] & 0xE0) == 0xC0) return ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    if ((s[0] & 0xF0) == 0xE0) return ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    if ((s[0] & 0xF8) == 0xF0) return ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    return s[0];
}

/* Get character advance width in pixels for line wrapping.
 * Uses LVGL font API for accurate metrics instead of hardcoded values. */
static int get_char_adv_width(uint32_t unicode, const lv_font_t *font) {
    lv_coord_t w = lv_font_get_glyph_width(font, unicode, 0);
    if (w > 0) return (int)w;
    /* Fallback for characters not in font */
    if (unicode >= 0x4E00 && unicode <= 0x9FFF) return 22;
    if (unicode >= 0x3000 && unicode <= 0x303F) return 22;
    if (unicode >= 0xFF00 && unicode <= 0xFFEF) return 22;
    return 11;
}

/* 渲染保护标志：update_display期间阻止REFR_TIMER并发渲染 */
volatile int g_rendering_in_progress = 0;

/* Debug counters for render cycle tracking */
static int g_render_label_count = 0;
static int g_render_char_count = 0;

/* Create a single-line label (no LVGL wrapping) at given y offset */
static void create_line_label(EpubViewer *viewer, const char *text, int len,
                               lv_font_t *font, int *y_offset) {
    if (len <= 0 || *y_offset >= CONTENT_HEIGHT) return;
    char buf[80];
    if (len > 79) len = 79;
    memcpy(buf, text, len);
    buf[len] = '\0';
    lv_obj_t *label = lv_label_create(viewer->content_container);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, *y_offset);
    lv_label_set_text(label, buf);
    g_render_label_count++;
    /* Count UTF-8 characters in this label */
    for (int i = 0; i < len; ) {
        unsigned char c = (unsigned char)buf[i];
        if (c < 0x80) i++;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else i++;
        g_render_char_count++;
    }
}

/* ========== 核心渲染函数 ========== */

static void update_display(EpubViewer *viewer) {
    uint32_t t0 = xTaskGetTickCount();
    if (!viewer || !viewer->content_container) return;
    if (viewer->decoded_text_len <= 0) return;

    int start_offset = viewer->read_offset;
    if (start_offset < 0) start_offset = 0;
    if (start_offset >= viewer->chapter_len) {
        start_offset = (viewer->chapter_len > ESTIMATED_PAGE_BYTES) ?
                        viewer->chapter_len - ESTIMATED_PAGE_BYTES : 0;
    }

    const char *p = viewer->decoded_text_buf + start_offset;
    const char *end = viewer->decoded_text_buf + viewer->chapter_len;

    printf("[UPDATE] offset=%d chapter_len=%d\n", start_offset, viewer->chapter_len);

    /* 调试：重置dsc统计，标记布局阶段开始 */
    lv_tiny_ttf_reset_dsc_stats();
    lv_tiny_ttf_set_dsc_phase(1);

    /* 设置渲染保护标志，防止REFR_TIMER在update_display期间并发渲染 */
    g_rendering_in_progress = 1;

    /* 隐藏容器以避免每个label创建时都产生invalid area，
     * 否则lv_refr_now()会对每个invalid area重新渲染所有label(O(n²)) */
    lv_obj_add_flag(viewer->content_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(viewer->content_container);
    /* lv_obj_clean后LVGL不会立即更新coords，强制重新设置尺寸和位置，
     * 否则容器尺寸可能停留在1x1（首页）或错误值（翻页后），
     * 导致所有子label被裁剪到错误区域内 */
    lv_obj_set_size(viewer->content_container, CONTENT_WIDTH, CONTENT_HEIGHT);
    lv_obj_align(viewer->content_container, LV_ALIGN_TOP_LEFT, CONTENT_X, CONTENT_Y);

    /* 调试：记录容器位置信息 */
    printf("[DISP_DBG] container: x=%d y=%d w=%d h=%d scr_h=%d CONTENT_HEIGHT=%d\n",
           lv_obj_get_x(viewer->content_container),
           lv_obj_get_y(viewer->content_container),
           lv_obj_get_width(viewer->content_container),
           lv_obj_get_height(viewer->content_container),
           SCREEN_HEIGHT, CONTENT_HEIGHT);

    int y_offset = 0;
    lv_font_t *current_font = FONT;
    int current_lh = LH_BODY;

    /* Line-by-line rendering */
    char line_buf[80];
    int line_pos = 0;
    int line_width = 0;

    int line_num = 0;
    while (p < end && y_offset + current_lh <= CONTENT_HEIGHT) {
        /* Check for font marker: 0x02 level 0x03 */
        if ((unsigned char)*p == 0x02 && (p + 2) < end &&
            *(p + 1) >= '0' && *(p + 1) <= '3' && (unsigned char)*(p + 2) == 0x03) {
            /* Flush current line */
            if (line_pos > 0) {
                printf("[LINE_DBG] #%d chars=%d x_wid=%d y=%d lh=%d\n",
                       line_num, line_pos, line_width, y_offset, current_lh);
                line_num++;
                create_line_label(viewer, line_buf, line_pos, current_font, &y_offset);
                y_offset += current_lh;
                line_pos = 0;
                line_width = 0;
            }
            int level = *(p + 1) - '0';
            switch (level) {
                case 1: current_font = FONT_H1; current_lh = LH_H1; break;
                case 2: current_font = FONT_H2; current_lh = LH_H2; break;
                case 3: current_font = FONT_H3; current_lh = LH_H3; break;
                default: current_font = FONT; current_lh = LH_BODY; break;
            }
            p += 3;
            continue;
        }

        /* Handle newline */
        if (*p == '\n') {
            if (line_pos > 0) {
                printf("[LINE_DBG] #%d chars=%d x_wid=%d y=%d lh=%d\n",
                       line_num, line_pos, line_width, y_offset, current_lh);
                line_num++;
                create_line_label(viewer, line_buf, line_pos, current_font, &y_offset);
                y_offset += current_lh;
                line_pos = 0;
                line_width = 0;
            }
            p++;
            continue;
        }

        /* Get next character */
        int clen = utf8_char_len((unsigned char)*p);
        if (p + clen > end) break;

        uint32_t unicode = utf8_to_unicode(p);
        int adv = get_char_adv_width(unicode, current_font);

        /* If adding this char exceeds line width, flush current line first */
        if (line_width + adv > CONTENT_WIDTH && line_pos > 0) {
            printf("[LINE_DBG] #%d chars=%d x_wid=%d y=%d lh=%d WRAP\n",
                   line_num, line_pos, line_width, y_offset, current_lh);
            line_num++;
            create_line_label(viewer, line_buf, line_pos, current_font, &y_offset);
            y_offset += current_lh;
            line_pos = 0;
            line_width = 0;
            if (y_offset + current_lh > CONTENT_HEIGHT) {
                p += clen;
                break;
            }
        }

        /* Add character to line buffer */
        for (int i = 0; i < clen && line_pos < 78; i++) {
            line_buf[line_pos++] = p[i];
        }
        line_width += adv;
        p += clen;
    }

    /* Flush remaining text */
    if (line_pos > 0 && y_offset + current_lh <= CONTENT_HEIGHT) {
        printf("[LINE_DBG] #%d chars=%d x_wid=%d y=%d lh=%d LAST\n",
               line_num, line_pos, line_width, y_offset, current_lh);
        create_line_label(viewer, line_buf, line_pos, current_font, &y_offset);
    } else if (line_pos > 0) {
        printf("[LINE_DBG] SKIP chars=%d y=%d+%d>%d\n",
               line_pos, y_offset, current_lh, CONTENT_HEIGHT);
    }

    /* 记录本页结束位置（下一页的起始） */
    viewer->page_end_offset = (int)(p - viewer->decoded_text_buf);

    /* [PAGE_TEXT] 调试输出：每页解码后的文字内容 */
    {
        int txt_len = viewer->page_end_offset - start_offset;
        if (txt_len > 0) {
            const char *txt_start = viewer->decoded_text_buf + start_offset;
            /* 先打印页面范围 */
            printf("[PAGE_TEXT] === offset=%d~%d (%d bytes) ===\n",
                   start_offset, viewer->page_end_offset, txt_len);
            /* 逐行输出，以\n为分隔符，每行最多80字节 */
            const char *line_start = txt_start;
            const char *txt_end = txt_start + txt_len;
            int line_no = 0;
            while (line_start < txt_end) {
                /* 找到行尾 */
                const char *line_end = line_start;
                while (line_end < txt_end && *line_end != '\n') line_end++;
                int llen = (int)(line_end - line_start);
                if (llen > 80) llen = 80; /* 限制每行输出长度 */
                /* 输出hex和可打印字符 */
                char hex_buf[241], ascii_buf[81]; /* 80*3+1=241, was 161 causing stack overflow */
                int hx = 0;
                for (int i = 0; i < llen; i++) {
                    unsigned char c = (unsigned char)line_start[i];
                    hx += sprintf(hex_buf + hx, "%02X ", c);
                    ascii_buf[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
                }
                ascii_buf[llen] = '\0';
                hex_buf[hx] = '\0';
                printf("[PAGE_TXT:%d] %s | %s\n", line_no, ascii_buf, hex_buf);
                line_no++;
                line_start = line_end + 1; /* 跳过\n */
            }
            printf("[PAGE_TEXT] === end ===\n");
        }
    }

    /* 更新百分比指示器 */
    float pct = get_read_percentage(viewer);
    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%.2f%%", pct);
    if (viewer->pct_indicator) lv_label_set_text(viewer->pct_indicator, pct_str);

    /* 更新进度条 */
    if (viewer->progress_bar) {
        int bar_max = SCREEN_WIDTH - 20;
        int bar_width = (int)((float)bar_max * pct / 100.0f);
        if (bar_width < 0) bar_width = 0;
        if (bar_width > bar_max) bar_width = bar_max;
        lv_obj_set_width(viewer->progress_bar, bar_width);
    }

    /* 更新工具栏信息 */
    update_toolbar_info(viewer);

    printf("[RENDER_DBG] page_labels=%d page_chars=%d offset_span=%d\n",
           g_render_label_count, g_render_char_count,
           viewer->page_end_offset - start_offset);
    g_render_label_count = 0;
    g_render_char_count = 0;
    printf("[RENDER_DBG] total_lines=%d CONTENT_WIDTH=%d CONTENT_HEIGHT=%d\n",
           line_num, CONTENT_WIDTH, CONTENT_HEIGHT);
    /* 恢复容器可见，此时只产生一次invalid area（容器整体），避免O(n²)重渲染 */
    lv_obj_clear_flag(viewer->content_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(viewer->content_container);

    /* 调试：打印布局阶段dsc统计 */
    {
        int layout_total, layout_hits, layout_misses, layout_non_cjk;
        int layout_unique_miss = lv_tiny_ttf_get_dsc_stats(&layout_total, &layout_hits, &layout_misses, &layout_non_cjk);
        printf("[DSC_STATS] layout phase: total=%d hits=%d misses=%d non_cjk=%d unique_miss=%d\n",
               layout_total, layout_hits, layout_misses, layout_non_cjk, layout_unique_miss);
        if(layout_unique_miss > 0) {
            extern uint32_t *lv_tiny_ttf_get_miss_uniques(void);
            /* 打印前20个miss的unicode */
            printf("[DSC_MISS_CHARS]");
            /* 直接使用stats API获取miss列表 */
        }
    }

    /* 标记渲染阶段 */
    lv_tiny_ttf_reset_dsc_stats();
    lv_tiny_ttf_set_dsc_phase(2);

    /* 强制LVGL同步完成所有渲染（包括字形渲染），确保framebuffer内容完整
     * 然后再触发EPD刷新，避免EPD DU读到不完整的画面 */
    uint32_t t_refr_start = xTaskGetTickCount();
    lv_tiny_ttf_bitmap_page_start();
    lv_refr_now(NULL);
    lv_tiny_ttf_bitmap_page_end();
    g_rendering_in_progress = 0;
    uint32_t t_refr_end = xTaskGetTickCount();

    /* 调试：打印渲染阶段dsc统计 */
    {
        int render_total, render_hits, render_misses, render_non_cjk;
        int render_unique_miss = lv_tiny_ttf_get_dsc_stats(&render_total, &render_hits, &render_misses, &render_non_cjk);
        printf("[DSC_STATS] render phase: total=%d hits=%d misses=%d non_cjk=%d unique_miss=%d\n",
               render_total, render_hits, render_misses, render_non_cjk, render_unique_miss);
    }

    epd_mark_refresh_pending();
    printf("[RENDER_DBG] layout=%dms refr_now=%dms total=%dms offset=%d->%d pct=%.2f%% y=%d\n",
           (t_refr_start - t0), (t_refr_end - t_refr_start), (t_refr_end - t0),
           start_offset, viewer->page_end_offset, pct, y_offset);
}

/* ========== 创建/销毁 ========== */

EpubViewer* epub_viewer_create(EpubReader *reader) {
    if (!reader) return NULL;
    EpubViewer *v = (EpubViewer*)calloc(1, sizeof(EpubViewer));
    if (!v) return NULL;
    v->reader = reader;

    /* 直接指向静态数组（.psram_bss段） */
    v->whole_xhtml_buf = s_whole_xhtml_buf;
    v->decoded_text_buf = s_decoded_text_buf;

    VIEW_LOG("Buffers static (.psram_bss): whole_xhtml=%d, decoded_text=%d\n",
             WHOLE_XHTML_BUF_SIZE, DECODED_TEXT_BUF_SIZE);
    return v;
}

/* ========== 工具栏 ========== */

static void update_toolbar_info(EpubViewer *viewer) {
    if (!viewer || !viewer->toolbar) return;

    float pct = get_read_percentage(viewer);

    /* 更新标题 */
    if (viewer->toolbar_title && viewer->reader) {
        const char *title = epub_reader_get_title(viewer->reader);
        if (title && title[0]) {
            lv_label_set_text(viewer->toolbar_title, title);
        } else {
            lv_label_set_text(viewer->toolbar_title, "EPUB");
        }
    }

    /* 更新百分比 */
    if (viewer->toolbar_pct_label) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f%%", pct);
        lv_label_set_text(viewer->toolbar_pct_label, buf);
    }
}

static void create_toolbar(EpubViewer *viewer) {
    if (!viewer) return;
    lv_font_t *ui_font = (lv_font_t*)&lv_font_misans_16;

    /* 工具栏容器，初始隐藏在屏幕上方 */
    viewer->toolbar = lv_obj_create(viewer->screen);
    lv_obj_set_size(viewer->toolbar, SCREEN_WIDTH, TOOLBAR_HEIGHT);
    lv_obj_set_pos(viewer->toolbar, 0, -TOOLBAR_HEIGHT);
    lv_obj_set_style_bg_color(viewer->toolbar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(viewer->toolbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(viewer->toolbar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(viewer->toolbar, 2, 0);
    lv_obj_set_style_border_color(viewer->toolbar, lv_color_black(), 0);
    lv_obj_set_style_pad_all(viewer->toolbar, 4, 0);
    lv_obj_clear_flag(viewer->toolbar, LV_OBJ_FLAG_SCROLLABLE);
    epd_disable_all_animations_recursive(viewer->toolbar);
    viewer->toolbar_visible = false;

    /* 第1行：标题 + 百分比 + 关闭按钮 */
    viewer->toolbar_title = lv_label_create(viewer->toolbar);
    lv_label_set_text(viewer->toolbar_title, "EPUB");
    lv_obj_set_style_text_font(viewer->toolbar_title, ui_font, 0);
    lv_obj_set_style_text_color(viewer->toolbar_title, lv_color_black(), 0);
    lv_obj_set_pos(viewer->toolbar_title, 4, 4);
    lv_obj_set_size(viewer->toolbar_title, 130, 16);

    viewer->toolbar_pct_label = lv_label_create(viewer->toolbar);
    lv_label_set_text(viewer->toolbar_pct_label, "0.00%");
    lv_obj_set_style_text_font(viewer->toolbar_pct_label, ui_font, 0);
    lv_obj_set_style_text_color(viewer->toolbar_pct_label, lv_color_make(0x66, 0x66, 0x66), 0);
    lv_obj_set_pos(viewer->toolbar_pct_label, 140, 4);

    lv_obj_t *close_btn = lv_btn_create(viewer->toolbar);
    lv_obj_set_size(close_btn, 22, 22);
    lv_obj_set_pos(close_btn, 212, 2);
    lv_obj_set_style_bg_color(close_btn, lv_color_white(), 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_border_color(close_btn, lv_color_make(0x99, 0x99, 0x99), 0);
    lv_obj_set_style_radius(close_btn, 2, 0);
    lv_obj_set_style_transition(close_btn, NULL, LV_PART_MAIN);
    lv_obj_add_event_cb(close_btn, toolbar_close_cb, LV_EVENT_CLICKED, viewer);
    epd_disable_all_animations_recursive(close_btn);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "X");
    lv_obj_set_style_text_font(close_lbl, ui_font, 0);
    lv_obj_center(close_lbl);

    /* 第2行：按钮组（返回书架/目录/上一章/下一章） */
    int btn_w = 52, btn_h = 26, btn_y = 28, gap = 4;
    int btn_x = 4;

    /* 返回书架 */
    lv_obj_t *btn_back = lv_btn_create(viewer->toolbar);
    lv_obj_set_size(btn_back, btn_w, btn_h);
    lv_obj_set_pos(btn_back, btn_x, btn_y);
    lv_obj_set_style_bg_color(btn_back, lv_color_white(), 0);
    lv_obj_set_style_border_width(btn_back, 1, 0);
    lv_obj_set_style_border_color(btn_back, lv_color_black(), 0);
    lv_obj_set_style_radius(btn_back, 2, 0);
    lv_obj_set_style_transition(btn_back, NULL, LV_PART_MAIN);
    epd_disable_all_animations_recursive(btn_back);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Home");
    lv_obj_set_style_text_font(lbl_back, ui_font, 0);
    lv_obj_center(lbl_back);
    lv_obj_add_event_cb(btn_back, close_viewer_cb, LV_EVENT_CLICKED, viewer);
    btn_x += btn_w + gap;

    /* 目录 */
    lv_obj_t *btn_toc = lv_btn_create(viewer->toolbar);
    lv_obj_set_size(btn_toc, btn_w, btn_h);
    lv_obj_set_pos(btn_toc, btn_x, btn_y);
    lv_obj_set_style_bg_color(btn_toc, lv_color_white(), 0);
    lv_obj_set_style_border_width(btn_toc, 1, 0);
    lv_obj_set_style_border_color(btn_toc, lv_color_black(), 0);
    lv_obj_set_style_radius(btn_toc, 2, 0);
    lv_obj_set_style_transition(btn_toc, NULL, LV_PART_MAIN);
    epd_disable_all_animations_recursive(btn_toc);
    lv_obj_t *lbl_toc = lv_label_create(btn_toc);
    lv_label_set_text(lbl_toc, "TOC");
    lv_obj_set_style_text_font(lbl_toc, ui_font, 0);
    lv_obj_center(lbl_toc);
    lv_obj_add_event_cb(btn_toc, toc_btn_cb, LV_EVENT_CLICKED, viewer);
    btn_x += btn_w + gap;

    /* 上一章 */
    lv_obj_t *btn_prev = lv_btn_create(viewer->toolbar);
    lv_obj_set_size(btn_prev, btn_w, btn_h);
    lv_obj_set_pos(btn_prev, btn_x, btn_y);
    lv_obj_set_style_bg_color(btn_prev, lv_color_white(), 0);
    lv_obj_set_style_border_width(btn_prev, 1, 0);
    lv_obj_set_style_border_color(btn_prev, lv_color_black(), 0);
    lv_obj_set_style_radius(btn_prev, 2, 0);
    lv_obj_set_style_transition(btn_prev, NULL, LV_PART_MAIN);
    epd_disable_all_animations_recursive(btn_prev);
    lv_obj_t *lbl_prev = lv_label_create(btn_prev);
    lv_label_set_text(lbl_prev, "<<");
    lv_obj_set_style_text_font(lbl_prev, ui_font, 0);
    lv_obj_center(lbl_prev);
    lv_obj_add_event_cb(btn_prev, prev_chapter_cb, LV_EVENT_CLICKED, viewer);
    btn_x += btn_w + gap;

    /* 下一章 */
    lv_obj_t *btn_next = lv_btn_create(viewer->toolbar);
    lv_obj_set_size(btn_next, btn_w, btn_h);
    lv_obj_set_pos(btn_next, btn_x, btn_y);
    lv_obj_set_style_bg_color(btn_next, lv_color_white(), 0);
    lv_obj_set_style_border_width(btn_next, 1, 0);
    lv_obj_set_style_border_color(btn_next, lv_color_black(), 0);
    lv_obj_set_style_radius(btn_next, 2, 0);
    lv_obj_set_style_transition(btn_next, NULL, LV_PART_MAIN);
    epd_disable_all_animations_recursive(btn_next);
    lv_obj_t *lbl_next = lv_label_create(btn_next);
    lv_label_set_text(lbl_next, ">>");
    lv_obj_set_style_text_font(lbl_next, ui_font, 0);
    lv_obj_center(lbl_next);
    lv_obj_add_event_cb(btn_next, next_chapter_cb, LV_EVENT_CLICKED, viewer);

    /* 第3行：百分比跳转 */
    lv_obj_t *goto_label = lv_label_create(viewer->toolbar);
    lv_label_set_text(goto_label, "Jump");
    lv_obj_set_style_text_font(goto_label, ui_font, 0);
    lv_obj_set_style_text_color(goto_label, lv_color_black(), 0);
    lv_obj_set_pos(goto_label, 4, 62);

    /* 百分比输入框 */
    viewer->toolbar_goto_ta = lv_textarea_create(viewer->toolbar);
    lv_obj_set_size(viewer->toolbar_goto_ta, 70, 24);
    lv_obj_set_pos(viewer->toolbar_goto_ta, 22, 60);
    lv_textarea_set_text(viewer->toolbar_goto_ta, "0.00");
    lv_textarea_set_accepted_chars(viewer->toolbar_goto_ta, "0123456789.");
    lv_textarea_set_max_length(viewer->toolbar_goto_ta, 6);
    lv_obj_set_style_text_font(viewer->toolbar_goto_ta, ui_font, 0);
    lv_obj_set_style_border_width(viewer->toolbar_goto_ta, 1, 0);
    lv_obj_set_style_border_color(viewer->toolbar_goto_ta, lv_color_make(0x99, 0x99, 0x99), 0);
    epd_disable_all_animations_recursive(viewer->toolbar_goto_ta);
    /* 点击输入框弹出数字键盘 */
    lv_obj_add_event_cb(viewer->toolbar_goto_ta, goto_ta_clicked_cb, LV_EVENT_CLICKED, viewer);

    lv_obj_t *pct_sign = lv_label_create(viewer->toolbar);
    lv_label_set_text(pct_sign, "%");
    lv_obj_set_style_text_font(pct_sign, ui_font, 0);
    lv_obj_set_style_text_color(pct_sign, lv_color_black(), 0);
    lv_obj_set_pos(pct_sign, 96, 64);

    /* 前往按钮 */
    lv_obj_t *goto_btn = lv_btn_create(viewer->toolbar);
    lv_obj_set_size(goto_btn, 50, 24);
    lv_obj_set_pos(goto_btn, 112, 60);
    lv_obj_set_style_bg_color(goto_btn, lv_color_white(), 0);
    lv_obj_set_style_border_width(goto_btn, 1, 0);
    lv_obj_set_style_border_color(goto_btn, lv_color_black(), 0);
    lv_obj_set_style_radius(goto_btn, 2, 0);
    lv_obj_set_style_transition(goto_btn, NULL, LV_PART_MAIN);
    epd_disable_all_animations_recursive(goto_btn);
    lv_obj_t *goto_lbl = lv_label_create(goto_btn);
    lv_label_set_text(goto_lbl, "Go");
    lv_obj_set_style_text_font(goto_lbl, ui_font, 0);
    lv_obj_center(goto_lbl);
    lv_obj_add_event_cb(goto_btn, goto_btn_cb, LV_EVENT_CLICKED, viewer);

    /* 章节信息 */
    lv_obj_t *chap_label = lv_label_create(viewer->toolbar);
    lv_label_set_text(chap_label, "Ch.1");
    lv_obj_set_style_text_font(chap_label, ui_font, 0);
    lv_obj_set_style_text_color(chap_label, lv_color_make(0x99, 0x99, 0x99), 0);
    lv_obj_set_pos(chap_label, 170, 64);
    /* 用user_data存储章节label以便后续更新 - 暂时不做，简单方案 */
    (void)chap_label;
}

static void toggle_toolbar(EpubViewer *viewer) {
    if (!viewer || !viewer->toolbar) return;
    viewer->toolbar_visible = !viewer->toolbar_visible;

    if (viewer->toolbar_visible) {
        lv_obj_set_pos(viewer->toolbar, 0, 0);
    } else {
        lv_obj_set_pos(viewer->toolbar, 0, -TOOLBAR_HEIGHT);
    }
    epd_mark_refresh_pending();
}

/* ========== 跳转键盘界面 ========== */

static void show_goto_keyboard(EpubViewer *viewer) {
    if (!viewer) return;
    lv_font_t *ui_font = (lv_font_t*)&lv_font_misans_16;

    viewer->goto_screen = lv_obj_create(NULL);
    lv_obj_set_size(viewer->goto_screen, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(viewer->goto_screen, lv_color_white(), 0);
    lv_obj_set_style_pad_all(viewer->goto_screen, 0, 0);
    epd_disable_all_animations_recursive(viewer->goto_screen);

    /* 标题 */
    lv_obj_t *title = lv_label_create(viewer->goto_screen);
    lv_label_set_text(title, "Jump to");
    lv_obj_set_style_text_font(title, ui_font, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* 输入框 */
    viewer->goto_ta = lv_textarea_create(viewer->goto_screen);
    lv_obj_set_size(viewer->goto_ta, 140, 36);
    lv_obj_align(viewer->goto_ta, LV_ALIGN_TOP_MID, 0, 30);
    lv_textarea_set_text(viewer->goto_ta, "");
    lv_textarea_set_placeholder_text(viewer->goto_ta, "0~100");
    lv_textarea_set_accepted_chars(viewer->goto_ta, "0123456789.");
    lv_textarea_set_max_length(viewer->goto_ta, 6);
    lv_obj_set_style_text_font(viewer->goto_ta, ui_font, 0);
    epd_disable_all_animations_recursive(viewer->goto_ta);

    /* 百分号 */
    lv_obj_t *pct_sign = lv_label_create(viewer->goto_screen);
    lv_label_set_text(pct_sign, "%");
    lv_obj_set_style_text_font(pct_sign, ui_font, 0);
    lv_obj_set_style_text_color(pct_sign, lv_color_black(), 0);
    lv_obj_align_to(pct_sign, viewer->goto_ta, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    /* 数字键盘 */
    viewer->goto_kb = lv_keyboard_create(viewer->goto_screen);
    lv_obj_set_size(viewer->goto_kb, 230, 150);
    lv_obj_align(viewer->goto_kb, LV_ALIGN_BOTTOM_MID, 0, -35);
    lv_keyboard_set_mode(viewer->goto_kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(viewer->goto_kb, viewer->goto_ta);
    epd_disable_all_animations_recursive(viewer->goto_kb);

    /* 取消按钮 */
    lv_obj_t *btn_cancel = lv_btn_create(viewer->goto_screen);
    lv_obj_set_size(btn_cancel, 80, 28);
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_LEFT, 10, -4);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_white(), 0);
    lv_obj_set_style_border_width(btn_cancel, 1, 0);
    lv_obj_set_style_border_color(btn_cancel, lv_color_black(), 0);
    lv_obj_set_style_radius(btn_cancel, 2, 0);
    lv_obj_set_style_transition(btn_cancel, NULL, LV_PART_MAIN);
    epd_disable_all_animations_recursive(btn_cancel);
    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_set_style_text_font(lbl_cancel, ui_font, 0);
    lv_obj_center(lbl_cancel);
    lv_obj_add_event_cb(btn_cancel, goto_cancel_cb, LV_EVENT_CLICKED, viewer);

    /* 确认按钮 */
    lv_obj_t *btn_confirm = lv_btn_create(viewer->goto_screen);
    lv_obj_set_size(btn_confirm, 80, 28);
    lv_obj_align(btn_confirm, LV_ALIGN_BOTTOM_RIGHT, -10, -4);
    lv_obj_set_style_bg_color(btn_confirm, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn_confirm, 1, 0);
    lv_obj_set_style_border_color(btn_confirm, lv_color_black(), 0);
    lv_obj_set_style_radius(btn_confirm, 2, 0);
    lv_obj_set_style_transition(btn_confirm, NULL, LV_PART_MAIN);
    epd_disable_all_animations_recursive(btn_confirm);
    lv_obj_t *lbl_confirm = lv_label_create(btn_confirm);
    lv_label_set_text(lbl_confirm, "Go");
    lv_obj_set_style_text_font(lbl_confirm, ui_font, 0);
    lv_obj_set_style_text_color(lbl_confirm, lv_color_white(), 0);
    lv_obj_center(lbl_confirm);
    lv_obj_add_event_cb(btn_confirm, goto_confirm_cb, LV_EVENT_CLICKED, viewer);

    lv_disp_load_scr(viewer->goto_screen);
    epd_mark_refresh_pending();
}

static void cleanup_goto_screen(EpubViewer *viewer) {
    if (!viewer) return;
    if (viewer->goto_kb) { lv_obj_del(viewer->goto_kb); viewer->goto_kb = NULL; }
    if (viewer->goto_ta) { lv_obj_del(viewer->goto_ta); viewer->goto_ta = NULL; }
    if (viewer->goto_screen) {
        lv_obj_del(viewer->goto_screen);
        viewer->goto_screen = NULL;
    }
    /* 重新加载阅读界面 */
    if (viewer->screen) lv_disp_load_scr(viewer->screen);
}

/* ========== 显示/关闭/销毁 ========== */

void epub_viewer_show(EpubViewer *viewer) {
    if (!viewer) return;

    lv_font_t *ui_font = (lv_font_t*)&lv_font_misans_16;

    /* 创建全屏（无固定Header/NavBar） */
    viewer->screen = lv_obj_create(NULL);
    lv_obj_set_size(viewer->screen, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(viewer->screen, lv_color_white(), 0);
    lv_obj_set_style_pad_all(viewer->screen, 0, 0);
    lv_obj_clear_flag(viewer->screen, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC |
                      LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN);
    epd_disable_all_animations_recursive(viewer->screen);

    /* 内容区：几乎全屏 */
    viewer->content_container = lv_obj_create(viewer->screen);
    lv_obj_set_size(viewer->content_container, CONTENT_WIDTH, CONTENT_HEIGHT);
    lv_obj_align(viewer->content_container, LV_ALIGN_TOP_LEFT, CONTENT_X, CONTENT_Y);
    lv_obj_set_style_bg_color(viewer->content_container, lv_color_white(), 0);
    lv_obj_set_style_border_width(viewer->content_container, 0, 0);
    lv_obj_set_style_pad_all(viewer->content_container, 0, 0);
    lv_obj_clear_flag(viewer->content_container, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC |
                      LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN);
    /* 让content_container的事件冒泡到screen，使screen上注册的触摸回调能正常触发 */
    lv_obj_add_flag(viewer->content_container, LV_OBJ_FLAG_EVENT_BUBBLE);
    epd_disable_all_animations_recursive(viewer->content_container);

    /* 进度条背景 */
    viewer->progress_bg = lv_obj_create(viewer->screen);
    lv_obj_set_size(viewer->progress_bg, SCREEN_WIDTH - 20, 2);
    lv_obj_align(viewer->progress_bg, LV_ALIGN_BOTTOM_LEFT, 10, -16);
    lv_obj_set_style_bg_color(viewer->progress_bg, lv_color_make(0xEE, 0xEE, 0xEE), 0);
    lv_obj_set_style_bg_opa(viewer->progress_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(viewer->progress_bg, 0, 0);
    lv_obj_set_style_radius(viewer->progress_bg, 0, 0);
    lv_obj_set_style_pad_all(viewer->progress_bg, 0, 0);
    lv_obj_clear_flag(viewer->progress_bg, LV_OBJ_FLAG_SCROLLABLE);
    epd_disable_all_animations_recursive(viewer->progress_bg);

    /* 进度条填充 */
    viewer->progress_bar = lv_obj_create(viewer->progress_bg);
    lv_obj_set_size(viewer->progress_bar, 0, 2);
    lv_obj_set_pos(viewer->progress_bar, 0, 0);
    lv_obj_set_style_bg_color(viewer->progress_bar, lv_color_make(0x99, 0x99, 0x99), 0);
    lv_obj_set_style_bg_opa(viewer->progress_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(viewer->progress_bar, 0, 0);
    lv_obj_set_style_radius(viewer->progress_bar, 0, 0);
    lv_obj_set_style_pad_all(viewer->progress_bar, 0, 0);
    epd_disable_all_animations_recursive(viewer->progress_bar);

    /* 底部百分比文字 */
    viewer->pct_indicator = lv_label_create(viewer->screen);
    lv_label_set_text(viewer->pct_indicator, "0.00%");
    lv_obj_set_style_text_font(viewer->pct_indicator, ui_font, 0);
    lv_obj_set_style_text_color(viewer->pct_indicator, lv_color_make(0x99, 0x99, 0x99), 0);
    lv_obj_align(viewer->pct_indicator, LV_ALIGN_BOTTOM_MID, 0, -2);

    /* 创建隐藏式工具栏 */
    create_toolbar(viewer);

    /* 全屏触控事件 - 使用RELEASED事件（释放后处理避免阻塞；不受长按阈值影响） */
    lv_obj_set_user_data(viewer->screen, viewer);
    lv_obj_add_event_cb(viewer->screen, content_area_event_cb, LV_EVENT_RELEASED, viewer);

    lv_disp_load_scr(viewer->screen);
    VIEW_LOG("Viewer shown (new UI)\n");
}

void epub_viewer_close(EpubViewer *viewer) {
    if (!viewer) return;
    /* 先清理跳转键盘 */
    if (viewer->goto_screen) {
        if (viewer->goto_kb) { lv_obj_del(viewer->goto_kb); viewer->goto_kb = NULL; }
        if (viewer->goto_ta) { lv_obj_del(viewer->goto_ta); viewer->goto_ta = NULL; }
        lv_obj_del(viewer->goto_screen);
        viewer->goto_screen = NULL;
    }
    if (viewer->toc_overlay) { lv_obj_del_async(viewer->toc_overlay); viewer->toc_overlay = NULL; }
    if (viewer->screen) { lv_obj_del_async(viewer->screen); viewer->screen = NULL; }
    viewer->content_container = NULL;
    viewer->pct_indicator = NULL;
    viewer->progress_bar = NULL;
    viewer->progress_bg = NULL;
    viewer->toolbar = NULL;
    if (viewer->close_cb) viewer->close_cb();
}

void epub_viewer_destroy(EpubViewer *viewer) {
    if (!viewer) return;
    epub_viewer_close(viewer);
    /* 静态数组（.psram_bss段），不需要free */
    viewer->whole_xhtml_buf = NULL;
    viewer->decoded_text_buf = NULL;
    free(viewer);
}

/* ========== 章节加载 ========== */

bool epub_viewer_goto_chapter(EpubViewer *viewer, int chapter_index) {
    if (!viewer || !viewer->reader) return false;
    if (chapter_index < 0 || chapter_index >= viewer->reader->spine_count) return false;

    viewer->current_chapter = chapter_index;

    VIEW_LOG("Goto chapter %d...\n", chapter_index);

    viewer->whole_xhtml_len = epub_reader_read_chapter_full(
        viewer->reader, chapter_index,
        viewer->whole_xhtml_buf, WHOLE_XHTML_BUF_SIZE);

    if (viewer->whole_xhtml_len < 0) {
        if (viewer->whole_xhtml_len == -2) {
            VIEW_ERR("Chapter %d too large (>1MB)\n", chapter_index);
        } else {
            VIEW_ERR("Failed to read chapter %d\n", chapter_index);
        }
        return false;
    }

    VIEW_LOG("Chapter %d: loaded %d bytes of XHTML\n", chapter_index, viewer->whole_xhtml_len);

    viewer->decoded_text_len = 0;
    viewer->decoded_text_buf[0] = '\0';

    XhtmlParser *xp = xhtml_parser_create();
    if (!xp) {
        VIEW_ERR("Failed to create XhtmlParser\n");
        return false;
    }

    xhtml_parser_set_callbacks(xp, decode_start_cb, decode_end_cb, decode_char_cb, viewer);

    uint32_t t0 = xTaskGetTickCount();
    bool parse_ok = xhtml_parser_parse(xp, viewer->whole_xhtml_buf, viewer->whole_xhtml_len);
    uint32_t t1 = xTaskGetTickCount();
    xhtml_parser_destroy(xp);

    if (!parse_ok) {
        VIEW_ERR("Expat parse failed for chapter %d\n", chapter_index);
        return false;
    }

    if (viewer->decoded_text_len > 0) {
        viewer->decoded_text_buf[viewer->decoded_text_len] = '\0';
        filter_unsupported_chars_ex(viewer->decoded_text_buf, true);
        viewer->decoded_text_len = strlen(viewer->decoded_text_buf);
    }

    VIEW_LOG("Chapter %d: parsed in %u ms, decoded=%d chars\n",
             chapter_index, t1 - t0, viewer->decoded_text_len);

    /* 设置百分比定位参数 */
    viewer->chapter_len = viewer->decoded_text_len;
    viewer->read_offset = 0;
    viewer->page_end_offset = 0;
    viewer->prev_page_start = 0;
    history_clear(viewer);

    update_display(viewer);

    if (viewer->chapter_cb) viewer->chapter_cb(chapter_index, viewer->reader->spine_count);
    return true;
}

/* ========== 翻页逻辑 ========== */

static void next_page_handler(EpubViewer *viewer) {
    if (!viewer) return;

    /* 如果已经渲染到章节末尾 */
    if (viewer->page_end_offset >= viewer->chapter_len) {
        /* 尝试下一章 */
        if (viewer->reader && viewer->current_chapter < viewer->reader->spine_count - 1) {
            epub_viewer_goto_chapter(viewer, viewer->current_chapter + 1);
        }
        return;
    }

    /* 如果页首和页尾相同（空页面或极短内容），也尝试翻页 */
    if (viewer->read_offset == viewer->page_end_offset && viewer->page_end_offset < viewer->chapter_len) {
        /* 强制前进至少一个字符 */
        viewer->page_end_offset++;
    }

    /* 压栈当前起始位置 */
    history_push(viewer, viewer->read_offset);

    /* 从上页结束位置开始渲染 */
    viewer->read_offset = viewer->page_end_offset;
    update_display(viewer);
}

static void prev_page_handler(EpubViewer *viewer) {
    if (!viewer) return;

    /* 从历史栈弹出 */
    int prev_offset = history_pop(viewer);
    if (prev_offset >= 0 && prev_offset < viewer->read_offset) {
        viewer->read_offset = prev_offset;
        update_display(viewer);
        return;
    }

    /* 没有历史，估算回退 */
    if (viewer->read_offset > 0) {
        int back = viewer->read_offset - ESTIMATED_PAGE_BYTES;
        if (back < 0) back = 0;

        const char *buf = viewer->decoded_text_buf;

        /* UTF-8对齐 */
        while (back < viewer->chapter_len && (buf[back] & 0xC0) == 0x80) back++;

        /* 换行对齐 */
        if (back > 0 && buf[back - 1] != '\n') {
            int limit = back + 300;
            if (limit > viewer->chapter_len) limit = viewer->chapter_len;
            for (int i = back; i < limit; i++) {
                if (buf[i] == '\n') { back = i + 1; break; }
            }
        }

        viewer->read_offset = back;
        update_display(viewer);
    } else {
        /* 已在章节开头，尝试上一章 */
        if (viewer->reader && viewer->current_chapter > 0) {
            epub_viewer_goto_chapter(viewer, viewer->current_chapter - 1);
            /* 跳到上一章末尾附近 */
            int back = viewer->chapter_len - ESTIMATED_PAGE_BYTES;
            if (back < 0) back = 0;
            const char *buf = viewer->decoded_text_buf;
            while (back < viewer->chapter_len && (buf[back] & 0xC0) == 0x80) back++;
            if (back > 0 && buf[back - 1] != '\n') {
                int limit = back + 300;
                if (limit > viewer->chapter_len) limit = viewer->chapter_len;
                for (int i = back; i < limit; i++) {
                    if (buf[i] == '\n') { back = i + 1; break; }
                }
            }
            viewer->read_offset = back;
            update_display(viewer);
        }
    }
}

/* ========== 触控处理 ========== */

static void content_area_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    /* 使用RELEASED而非CLICKED：触摸持续>400ms时LVGL不发CLICKED事件（长按检测），
     * PRESSED虽然也触发但会阻塞释放信号处理，RELEASED在手指抬起时触发最合适 */
    if (code != LV_EVENT_RELEASED) return;

    EpubViewer *viewer = (EpubViewer *)lv_event_get_user_data(e);
    if (!viewer) return;

    lv_point_t point;
    lv_indev_get_point(lv_indev_get_act(), &point);
    int x = point.x, y = point.y;

    /* 目录打开时忽略 */
    if (viewer->toc_overlay) return;

    /* 跳转键盘打开时忽略 */
    if (viewer->goto_screen) return;

    /* 点击工具栏区域时让工具栏自己处理 */
    if (viewer->toolbar_visible && y < TOOLBAR_HEIGHT) return;

    /* 顶部25px → 弹出/关闭工具栏 */
    if (y < TOOLBAR_TRIGGER_Y) {
        toggle_toolbar(viewer);
        return;
    }

    /* 工具栏打开时，点击其他区域 → 关闭工具栏 */
    if (viewer->toolbar_visible) {
        toggle_toolbar(viewer);
        return;
    }

    /* 左1/3 → 上一页 */
    if (x < SCREEN_WIDTH / 3) {
        prev_page_handler(viewer);
    }
    /* 右1/3 → 下一页 */
    else if (x > SCREEN_WIDTH * 2 / 3) {
        next_page_handler(viewer);
    }
    /* 中间1/3 → 无操作 */
}

/* ========== 工具栏回调 ========== */

static void toolbar_close_cb(lv_event_t *e) {
    EpubViewer *viewer = (EpubViewer *)lv_event_get_user_data(e);
    if (viewer && viewer->toolbar_visible) toggle_toolbar(viewer);
}

static void close_viewer_cb(lv_event_t *e) {
    EpubViewer *viewer = (EpubViewer *)lv_event_get_user_data(e);
    if (viewer) {
        if (viewer->toolbar_visible) toggle_toolbar(viewer);
        epub_viewer_close(viewer);
    }
}

static void toc_btn_cb(lv_event_t *e) {
    EpubViewer *viewer = (EpubViewer *)lv_event_get_user_data(e);
    if (!viewer) return;
    /* 先关闭工具栏 */
    if (viewer->toolbar_visible) toggle_toolbar(viewer);
    epub_viewer_show_toc(viewer);
}

static void prev_chapter_cb(lv_event_t *e) {
    EpubViewer *viewer = (EpubViewer *)lv_event_get_user_data(e);
    if (!viewer) return;
    if (viewer->toolbar_visible) toggle_toolbar(viewer);
    if (viewer->reader && viewer->current_chapter > 0) {
        epub_viewer_goto_chapter(viewer, viewer->current_chapter - 1);
    }
}

static void next_chapter_cb(lv_event_t *e) {
    EpubViewer *viewer = (EpubViewer *)lv_event_get_user_data(e);
    if (!viewer) return;
    if (viewer->toolbar_visible) toggle_toolbar(viewer);
    if (viewer->reader && viewer->current_chapter < viewer->reader->spine_count - 1) {
        epub_viewer_goto_chapter(viewer, viewer->current_chapter + 1);
    }
}

/* ========== 跳转回调 ========== */

static void goto_ta_clicked_cb(lv_event_t *e) {
    EpubViewer *viewer = (EpubViewer *)lv_event_get_user_data(e);
    if (!viewer) return;
    /* 关闭工具栏并弹出键盘 */
    if (viewer->toolbar_visible) toggle_toolbar(viewer);
    show_goto_keyboard(viewer);
}

static void goto_btn_cb(lv_event_t *e) {
    EpubViewer *viewer = (EpubViewer *)lv_event_get_user_data(e);
    if (!viewer || !viewer->toolbar_goto_ta) return;

    const char *text = lv_textarea_get_text(viewer->toolbar_goto_ta);
    float pct = 0.0f;

    /* 解析百分比 */
    if (text && text[0]) {
        pct = (float)atof(text);
    }

    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;

    int pct_x100 = (int)(pct * 100 + 0.5f);
    int new_offset = pct_to_offset(viewer, pct_x100);

    /* 关闭工具栏 */
    if (viewer->toolbar_visible) toggle_toolbar(viewer);

    /* 清空历史 */
    history_clear(viewer);

    /* 跳转 */
    viewer->read_offset = new_offset;
    update_display(viewer);
}

/* goto键盘界面的回调 - 需要在函数外能访问viewer */
static void goto_cancel_cb(lv_event_t *e) {
    EpubViewer *viewer = (EpubViewer *)lv_event_get_user_data(e);
    cleanup_goto_screen(viewer);
    epd_mark_refresh_pending();
}

static void goto_confirm_cb(lv_event_t *e) {
    EpubViewer *viewer = (EpubViewer *)lv_event_get_user_data(e);
    if (!viewer || !viewer->goto_ta) return;

    const char *text = lv_textarea_get_text(viewer->goto_ta);
    float pct = 0.0f;

    if (text && text[0]) {
        pct = (float)atof(text);
    }

    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;

    int pct_x100 = (int)(pct * 100 + 0.5f);
    int new_offset = pct_to_offset(viewer, pct_x100);

    /* 清理键盘界面 */
    cleanup_goto_screen(viewer);

    /* 清空历史 */
    history_clear(viewer);

    /* 关闭工具栏 */
    if (viewer->toolbar_visible) toggle_toolbar(viewer);

    /* 跳转 */
    viewer->read_offset = new_offset;
    update_display(viewer);
}

/* ========== 目录 ========== */

void epub_viewer_show_toc(EpubViewer *viewer) {
    if (!viewer || !viewer->reader || !viewer->screen) return;

    /* 如果目录已打开，关闭它 */
    if (viewer->toc_overlay) {
        lv_obj_del_async(viewer->toc_overlay);
        viewer->toc_overlay = NULL;
        epd_mark_refresh_pending();
        return;
    }

    lv_font_t *ui_font = (lv_font_t*)&lv_font_misans_16;

    /* 全屏覆盖 */
    viewer->toc_overlay = lv_obj_create(viewer->screen);
    lv_obj_set_size(viewer->toc_overlay, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_align(viewer->toc_overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(viewer->toc_overlay, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(viewer->toc_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(viewer->toc_overlay, 0, 0);
    lv_obj_clear_flag(viewer->toc_overlay, LV_OBJ_FLAG_SCROLLABLE);
    epd_disable_all_animations_recursive(viewer->toc_overlay);

    /* 标题行 + 关闭按钮 */
    lv_obj_t *header = lv_obj_create(viewer->toc_overlay);
    lv_obj_set_size(header, SCREEN_WIDTH, 30);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_white(), 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, 2, 0);
    lv_obj_set_style_border_color(header, lv_color_black(), 0);
    lv_obj_set_style_pad_all(header, 4, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    epd_disable_all_animations_recursive(header);

    lv_obj_t *toc_title = lv_label_create(header);
    lv_label_set_text(toc_title, "TOC");
    lv_obj_set_style_text_font(toc_title, ui_font, 0);
    lv_obj_set_style_text_color(toc_title, lv_color_black(), 0);
    lv_obj_align(toc_title, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *toc_close_btn = lv_btn_create(header);
    lv_obj_set_size(toc_close_btn, 22, 22);
    lv_obj_align(toc_close_btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(toc_close_btn, lv_color_white(), 0);
    lv_obj_set_style_border_width(toc_close_btn, 1, 0);
    lv_obj_set_style_border_color(toc_close_btn, lv_color_make(0x99, 0x99, 0x99), 0);
    lv_obj_set_style_radius(toc_close_btn, 2, 0);
    lv_obj_set_style_transition(toc_close_btn, NULL, LV_PART_MAIN);
    epd_disable_all_animations_recursive(toc_close_btn);
    lv_obj_t *toc_close_lbl = lv_label_create(toc_close_btn);
    lv_label_set_text(toc_close_lbl, "X");
    lv_obj_set_style_text_font(toc_close_lbl, ui_font, 0);
    lv_obj_center(toc_close_lbl);
    lv_obj_add_event_cb(toc_close_btn, toc_close_cb, LV_EVENT_CLICKED, viewer);

    /* 目录列表 */
    int toc_count = epub_reader_get_toc_count(viewer->reader);
    if (toc_count > 0) {
        lv_obj_t *list = lv_list_create(viewer->toc_overlay);
        lv_obj_set_size(list, SCREEN_WIDTH, SCREEN_HEIGHT - 34);
        lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, 32);
        lv_obj_set_style_pad_row(list, 2, 0);
        epd_disable_all_animations_recursive(list);

        for (int i = 0; i < toc_count && i < 20; i++) {
            EpubTocEntry *toc = epub_reader_get_toc(viewer->reader, i);
            if (toc) {
                /* 使用带标记的按钮文本 */
                char item_text[128];
                bool is_current = false;

                /* 判断是否当前章节 */
                if (viewer->reader) {
                    int ch = epub_reader_jump_to_toc(viewer->reader, i);
                    if (ch == viewer->current_chapter) is_current = true;
                }

                if (is_current) {
                    snprintf(item_text, sizeof(item_text), "> %s", toc->title);
                } else {
                    snprintf(item_text, sizeof(item_text), "  %s", toc->title);
                }

                lv_obj_t *btn = lv_list_add_btn(list, NULL, item_text);
                lv_obj_set_style_text_font(btn, ui_font, 0);
                if (is_current) {
                    lv_obj_set_style_text_color(btn, lv_color_black(), 0);
                } else {
                    lv_obj_set_style_text_color(btn, lv_color_make(0x44, 0x44, 0x44), 0);
                }
                /* 将索引编码到user_data */
                lv_obj_set_user_data(btn, (void*)(long)(i + 1)); /* +1 区分NULL */
                lv_obj_add_event_cb(btn, toc_item_cb, LV_EVENT_CLICKED, viewer);
            }
        }
    }

    epd_mark_refresh_pending();
}

static void toc_item_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    EpubViewer *viewer = (EpubViewer *)lv_event_get_user_data(e);
    if (!viewer) return;

    long btn_idx = (long)lv_obj_get_user_data(btn);
    int toc_idx = (int)(btn_idx - 1);  /* -1 还原 */
    if (toc_idx < 0) return;

    if (viewer->reader) {
        int chapter = epub_reader_jump_to_toc(viewer->reader, toc_idx);
        if (chapter >= 0) epub_viewer_goto_chapter(viewer, chapter);
    }

    if (viewer->toc_overlay) {
        lv_obj_del_async(viewer->toc_overlay);
        viewer->toc_overlay = NULL;
        epd_mark_refresh_pending();
    }
}

static void toc_close_cb(lv_event_t *e) {
    EpubViewer *viewer = (EpubViewer *)lv_event_get_user_data(e);
    if (viewer && viewer->toc_overlay) {
        lv_obj_del_async(viewer->toc_overlay);
        viewer->toc_overlay = NULL;
        epd_mark_refresh_pending();
    }
}

/* ========== 公共接口 ========== */

bool epub_viewer_prev_page(EpubViewer *viewer) {
    if (!viewer) return false;
    if (viewer->read_offset <= 0 && viewer->history_count == 0) return false;
    prev_page_handler(viewer);
    return true;
}

bool epub_viewer_next_page(EpubViewer *viewer) {
    if (!viewer) return false;
    if (viewer->page_end_offset >= viewer->chapter_len) return false;
    next_page_handler(viewer);
    return true;
}

int epub_viewer_get_current_chapter(EpubViewer *viewer) { return viewer ? viewer->current_chapter : 0; }

int epub_viewer_get_current_page(EpubViewer *viewer) {
    /* 兼容旧接口，返回1 */
    (void)viewer;
    return 1;
}

int epub_viewer_get_total_pages(EpubViewer *viewer) {
    /* 兼容旧接口，返回1 */
    (void)viewer;
    return 1;
}

void epub_viewer_set_chapter_loaded_cb(EpubViewer *viewer, epub_chapter_loaded_cb cb) {
    if (viewer) viewer->chapter_cb = cb;
}

void epub_viewer_set_close_cb(EpubViewer *viewer, epub_close_callback_t cb) {
    if (viewer) viewer->close_cb = cb;
}

/* ========== HTML工具函数（保留） ========== */

int epub_strip_html_tags(const char *html, int len, char *output, int out_size) {
    if (!html || !output || out_size <= 0) return 0;
    int out_pos = 0, i = 0;
    bool in_tag = false;
    while (i < len && out_pos < out_size - 1) {
        if (html[i] == '<') { in_tag = true; i++; continue; }
        if (html[i] == '>') { in_tag = false; i++; continue; }
        if (in_tag) { i++; continue; }
        if (html[i] == '&') {
            if (i + 5 < len && strncasecmp(html + i, "&nbsp;", 6) == 0) { output[out_pos++] = ' '; i += 6; continue; }
            if (i + 3 < len && strncasecmp(html + i, "<", 4) == 0) { output[out_pos++] = '<'; i += 4; continue; }
            if (i + 3 < len && strncasecmp(html + i, ">", 4) == 0) { output[out_pos++] = '>'; i += 4; continue; }
            if (i + 4 < len && strncasecmp(html + i, "&", 5) == 0) { output[out_pos++] = '&'; i += 5; continue; }
        }
        output[out_pos++] = html[i++];
    }
    output[out_pos] = '\0';
    return out_pos;
}

int epub_decode_html_entities(const char *text, char *output, int out_size) {
    if (!text || !output || out_size <= 0) return 0;
    strncpy(output, text, out_size - 1);
    output[out_size - 1] = '\0';
    return strlen(output);
}