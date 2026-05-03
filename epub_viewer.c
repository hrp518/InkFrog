#include "epub_viewer.h"
#include "epub_xhtml_parser.h"
#include "file_manager.h"
#include "lv_port_disp.h"
#include "lvgl/lvgl.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/dma_heap.h>
#include "fs/fatfs/ff.h"

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

#define EPUB_MAX_PAGES_PER_CHAPTER  1000
#define PAGE_UNIT_BYTES              2048
#define READ_WINDOW_SIZE             8192

#define SCREEN_WIDTH   240
#define SCREEN_HEIGHT  415
#define CONTENT_X      10
#define CONTENT_Y      50
#define CONTENT_WIDTH  (SCREEN_WIDTH - 20)
#define CONTENT_HEIGHT (SCREEN_HEIGHT - 121)

#define FONT       get_reader_font()
#define FONT_H1    get_reader_font_h1()
#define FONT_H2    get_reader_font_h2()
#define FONT_H3    get_reader_font_h3()

#define LH_BODY    22
#define LH_H3      26
#define LH_H2      30
#define LH_H1      34

LV_FONT_DECLARE(lv_font_misans_16);

struct EpubViewer {
    EpubReader *reader;
    lv_obj_t *screen;
    lv_obj_t *content_container;
    lv_obj_t *title_label;
    lv_obj_t *page_label;
    lv_obj_t *toc_list;
    lv_obj_t *loading_msg;

    char *whole_xhtml_buf;
    int whole_xhtml_len;
    char *decoded_text_buf;
    int decoded_text_len;

    int current_page;
    int total_pages;
    int current_chapter;
    epub_chapter_loaded_cb chapter_cb;
    epub_close_callback_t close_cb;
};

extern lv_font_t *get_reader_font(void);
extern lv_font_t *get_reader_font_h1(void);
extern lv_font_t *get_reader_font_h2(void);
extern lv_font_t *get_reader_font_h3(void);

static void update_display(EpubViewer *viewer);
static void prev_page_handler(EpubViewer *viewer);
static void next_page_handler(EpubViewer *viewer);
static void content_area_event_cb(lv_event_t *e);
static void close_viewer_cb(lv_event_t *e);
static void toc_item_cb(lv_event_t *e);
static void toc_btn_close_cb(lv_event_t *e);
static void page_prev_cb(lv_event_t *e);
static void page_next_cb(lv_event_t *e);
static void toc_btn_cb(lv_event_t *e);
static void filter_unsupported_chars_ex(char *str, bool preserve_markers);
static void filter_unsupported_chars(char *str);

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
}

static void filter_unsupported_chars(char *str) {
    filter_unsupported_chars_ex(str, false);
}

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

EpubViewer* epub_viewer_create(EpubReader *reader) {
    if (!reader) return NULL;
    EpubViewer *v = (EpubViewer*)calloc(1, sizeof(EpubViewer));
    if (!v) return NULL;
    v->reader = reader;

    v->whole_xhtml_buf = (char*)_dma_malloc(WHOLE_XHTML_BUF_SIZE, DMAHEAP_PSRAM);
    v->decoded_text_buf = (char*)_dma_malloc(DECODED_TEXT_BUF_SIZE, DMAHEAP_PSRAM);

    if (!v->whole_xhtml_buf || !v->decoded_text_buf) {
        VIEW_ERR("Buffer alloc failed (need %d + %d bytes PSRAM)\n",
                 WHOLE_XHTML_BUF_SIZE, DECODED_TEXT_BUF_SIZE);
        epub_viewer_destroy(v);
        return NULL;
    }

    VIEW_LOG("Buffers allocated: whole_xhtml=%d, decoded_text=%d\n",
             WHOLE_XHTML_BUF_SIZE, DECODED_TEXT_BUF_SIZE);
    return v;
}

void epub_viewer_show(EpubViewer *viewer) {
    if (!viewer) return;

    lv_font_t *ui_font = &lv_font_misans_16;

    viewer->screen = lv_obj_create(NULL);
    lv_obj_set_size(viewer->screen, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(viewer->screen, lv_color_white(), 0);
    lv_obj_set_style_pad_all(viewer->screen, 0, 0);
    lv_obj_clear_flag(viewer->screen, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC |
                      LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN);
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
    lv_obj_set_style_border_width(back_btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(back_btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_user_data(back_btn, viewer);
    lv_obj_add_event_cb(back_btn, close_viewer_cb, LV_EVENT_CLICKED, viewer);
    epd_disable_all_animations_recursive(back_btn);

    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "<");
    lv_obj_set_style_text_font(back_label, ui_font, 0);
    lv_obj_center(back_label);

    viewer->title_label = lv_label_create(header);
    lv_label_set_text(viewer->title_label, "Loading...");
    lv_obj_set_style_text_font(viewer->title_label, ui_font, 0);
    lv_obj_align(viewer->title_label, LV_ALIGN_TOP_MID, 0, 10);

    viewer->page_label = lv_label_create(header);
    lv_label_set_text(viewer->page_label, "1/?");
    lv_obj_set_style_text_font(viewer->page_label, ui_font, 0);
    lv_obj_align(viewer->page_label, LV_ALIGN_RIGHT_MID, -10, 0);

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

    lv_obj_t *nav_bar = lv_obj_create(viewer->screen);
    lv_obj_set_size(nav_bar, SCREEN_WIDTH, 50);
    lv_obj_align(nav_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(nav_bar, lv_color_white(), 0);
    epd_disable_all_animations_recursive(nav_bar);

    lv_obj_t *page_prev_btn = lv_btn_create(nav_bar);
    lv_obj_set_size(page_prev_btn, 55, 22);
    lv_obj_align(page_prev_btn, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_border_width(page_prev_btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(page_prev_btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_user_data(page_prev_btn, viewer);
    lv_obj_add_event_cb(page_prev_btn, page_prev_cb, LV_EVENT_CLICKED, viewer);
    epd_disable_all_animations_recursive(page_prev_btn);

    lv_obj_t *page_prev_label = lv_label_create(page_prev_btn);
    lv_label_set_text(page_prev_label, "Prev");
    lv_obj_set_style_text_font(page_prev_label, ui_font, 0);
    lv_obj_center(page_prev_label);

    lv_obj_t *toc_btn_obj = lv_btn_create(nav_bar);
    lv_obj_set_size(toc_btn_obj, 50, 22);
    lv_obj_align(toc_btn_obj, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_set_style_border_width(toc_btn_obj, 1, LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(toc_btn_obj, 0, LV_STATE_FOCUSED);
    lv_obj_set_user_data(toc_btn_obj, viewer);
    lv_obj_add_event_cb(toc_btn_obj, toc_btn_cb, LV_EVENT_CLICKED, viewer);
    epd_disable_all_animations_recursive(toc_btn_obj);

    lv_obj_t *toc_label = lv_label_create(toc_btn_obj);
    lv_label_set_text(toc_label, "TOC");
    lv_obj_set_style_text_font(toc_label, ui_font, 0);
    lv_obj_center(toc_label);

    lv_obj_t *page_next_btn = lv_btn_create(nav_bar);
    lv_obj_set_size(page_next_btn, 55, 22);
    lv_obj_align(page_next_btn, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_style_border_width(page_next_btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(page_next_btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_user_data(page_next_btn, viewer);
    lv_obj_add_event_cb(page_next_btn, page_next_cb, LV_EVENT_CLICKED, viewer);
    epd_disable_all_animations_recursive(page_next_btn);

    lv_obj_t *page_next_label = lv_label_create(page_next_btn);
    lv_label_set_text(page_next_label, "Next");
    lv_obj_set_style_text_font(page_next_label, ui_font, 0);
    lv_obj_center(page_next_label);

    lv_disp_load_scr(viewer->screen);
    VIEW_LOG("Viewer shown\n");
}

void epub_viewer_close(EpubViewer *viewer) {
    if (!viewer) return;
    if (viewer->screen) { lv_obj_del_async(viewer->screen); viewer->screen = NULL; }
    if (viewer->toc_list) { lv_obj_del_async(viewer->toc_list); viewer->toc_list = NULL; }
    viewer->content_container = NULL;
    viewer->title_label = NULL;
    viewer->page_label = NULL;
    if (viewer->close_cb) viewer->close_cb();
}

void epub_viewer_destroy(EpubViewer *viewer) {
    if (!viewer) return;
    epub_viewer_close(viewer);
    if (viewer->whole_xhtml_buf) { _dma_free(viewer->whole_xhtml_buf, DMAHEAP_PSRAM); viewer->whole_xhtml_buf = NULL; }
    if (viewer->decoded_text_buf) { _dma_free(viewer->decoded_text_buf, DMAHEAP_PSRAM); viewer->decoded_text_buf = NULL; }
    free(viewer);
}

bool epub_viewer_goto_chapter(EpubViewer *viewer, int chapter_index) {
    if (!viewer || !viewer->reader) return false;
    if (chapter_index < 0 || chapter_index >= viewer->reader->spine_count) return false;

    viewer->current_chapter = chapter_index;
    viewer->current_page = 0;

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

    viewer->total_pages = (viewer->decoded_text_len + PAGE_UNIT_BYTES - 1) / PAGE_UNIT_BYTES;
    if (viewer->total_pages < 1) viewer->total_pages = 1;
    if (viewer->total_pages > EPUB_MAX_PAGES_PER_CHAPTER) viewer->total_pages = EPUB_MAX_PAGES_PER_CHAPTER;

    update_display(viewer);

    if (viewer->chapter_cb) viewer->chapter_cb(chapter_index, viewer->reader->spine_count);
    return true;
}

static void update_display(EpubViewer *viewer) {
    uint32_t t0 = xTaskGetTickCount();
    if (!viewer || !viewer->content_container) return;
    if (viewer->decoded_text_len <= 0) return;

    int page = viewer->current_page;
    if (page < 0) page = 0;
    if (page >= viewer->total_pages) page = viewer->total_pages - 1;

    uint32_t offset = (uint32_t)page * PAGE_UNIT_BYTES;
    if (offset >= (uint32_t)viewer->decoded_text_len) {
        offset = (viewer->decoded_text_len > PAGE_UNIT_BYTES) ?
                  viewer->decoded_text_len - PAGE_UNIT_BYTES : 0;
    }

    uint32_t to_read = READ_WINDOW_SIZE;
    if (offset + to_read > (uint32_t)viewer->decoded_text_len) {
        to_read = viewer->decoded_text_len - offset;
    }
    if ((int)to_read <= 0) return;

    const char *p = viewer->decoded_text_buf + offset;

    printf("[UPDATE] page=%d offset=%u len=%u decoded_total=%d\n",
           page, offset, to_read, viewer->decoded_text_len);

    uint32_t t4 = xTaskGetTickCount();
    lv_obj_clean(viewer->content_container);

    const char *block_start = p;
    int y_offset = 0;
    lv_font_t *current_font = FONT;
    int current_lh = LH_BODY;
    const char *end = p + to_read;

    while (p < end && y_offset < CONTENT_HEIGHT) {
        if ((unsigned char)*p == 0x02 && (p + 2) < end &&
            *(p + 1) >= '0' && *(p + 1) <= '3' && (unsigned char)*(p + 2) == 0x03) {
            int blen = (int)(p - block_start);
            if (blen > 0 && y_offset < CONTENT_HEIGHT) {
                int clen = blen > 500 ? 500 : blen;
                char buf[502];
                memcpy(buf, block_start, clen);
                buf[clen] = '\0';
                lv_obj_t *label = lv_label_create(viewer->content_container);
                lv_obj_set_style_text_font(label, current_font, 0);
                lv_obj_set_style_text_color(label, lv_color_black(), 0);
                lv_obj_set_width(label, CONTENT_WIDTH);
                lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, y_offset);
                lv_label_set_text(label, buf);
                y_offset += current_lh;
            }
            int level = *(p + 1) - '0';
            switch (level) {
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
            int blen = (int)(p - block_start);
            if (blen > 0 && y_offset < CONTENT_HEIGHT) {
                int clen = blen > 500 ? 500 : blen;
                char buf[502];
                memcpy(buf, block_start, clen);
                buf[clen] = '\0';
                lv_obj_t *label = lv_label_create(viewer->content_container);
                lv_obj_set_style_text_font(label, current_font, 0);
                lv_obj_set_style_text_color(label, lv_color_black(), 0);
                lv_obj_set_width(label, CONTENT_WIDTH);
                lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, y_offset);
                lv_label_set_text(label, buf);
                y_offset += current_lh;
            }
            p++;
            block_start = p;
            continue;
        }
        p++;
    }
    {
        int blen = (int)(p - block_start);
        if (blen > 0 && y_offset < CONTENT_HEIGHT) {
            int clen = blen > 500 ? 500 : blen;
            char buf[502];
            memcpy(buf, block_start, clen);
            buf[clen] = '\0';
            lv_obj_t *label = lv_label_create(viewer->content_container);
            lv_obj_set_style_text_font(label, current_font, 0);
            lv_obj_set_style_text_color(label, lv_color_black(), 0);
            lv_obj_set_width(label, CONTENT_WIDTH);
            lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, y_offset);
            lv_label_set_text(label, buf);
        }
    }

    char page_str[32];
    snprintf(page_str, sizeof(page_str), "%d/%d", page + 1, viewer->total_pages);
    lv_label_set_text(viewer->page_label, page_str);

    epd_mark_refresh_pending();
    uint32_t t5 = xTaskGetTickCount();
    printf("[UPDATE] done: %u ticks, page=%d/%d y=%d\n",
           t5 - t0, page + 1, viewer->total_pages, y_offset);
}

static void prev_page_handler(EpubViewer *viewer) {
    if (!viewer) return;
    if (viewer->current_page > 0) {
        viewer->current_page--;
        update_display(viewer);
    } else if (viewer->current_chapter > 0) {
        epub_viewer_goto_chapter(viewer, viewer->current_chapter - 1);
        if (viewer->total_pages > 0) {
            viewer->current_page = viewer->total_pages - 1;
            update_display(viewer);
        }
    }
}

static void next_page_handler(EpubViewer *viewer) {
    if (!viewer) return;
    if (viewer->current_page < viewer->total_pages - 1) {
        viewer->current_page++;
        update_display(viewer);
    } else if (viewer->reader && viewer->current_chapter < viewer->reader->spine_count - 1) {
        epub_viewer_goto_chapter(viewer, viewer->current_chapter + 1);
    }
}

bool epub_viewer_prev_page(EpubViewer *viewer) {
    if (!viewer || viewer->current_page <= 0) return false;
    viewer->current_page--;
    update_display(viewer);
    return true;
}

bool epub_viewer_next_page(EpubViewer *viewer) {
    if (!viewer || viewer->current_page >= viewer->total_pages - 1) return false;
    viewer->current_page++;
    update_display(viewer);
    return true;
}

static void content_area_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *screen = lv_event_get_target(e);
    EpubViewer *viewer = (EpubViewer*)lv_obj_get_user_data(screen);
    if (!viewer) return;
    lv_point_t point;
    lv_indev_get_point(lv_indev_get_act(), &point);
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_RELEASED) {
        int x = point.x, y = point.y;
        if (x < CONTENT_X || x > CONTENT_X + CONTENT_WIDTH || y < CONTENT_Y || y > CONTENT_Y + CONTENT_HEIGHT) return;
        if (x < 80) prev_page_handler(viewer);
        else if (x > 160) next_page_handler(viewer);
        else epub_viewer_show_toc(viewer);
    }
}

static void close_viewer_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    EpubViewer *viewer = (EpubViewer*)lv_obj_get_user_data(btn);
    if (viewer) epub_viewer_close(viewer);
}

static void page_prev_cb(lv_event_t *e) {
    EpubViewer *viewer = (EpubViewer*)lv_obj_get_user_data(lv_event_get_target(e));
    if (viewer) prev_page_handler(viewer);
}

static void page_next_cb(lv_event_t *e) {
    EpubViewer *viewer = (EpubViewer*)lv_obj_get_user_data(lv_event_get_target(e));
    if (viewer) next_page_handler(viewer);
}

static void toc_btn_cb(lv_event_t *e) {
    EpubViewer *viewer = (EpubViewer*)lv_obj_get_user_data(lv_event_get_target(e));
    if (viewer) epub_viewer_show_toc(viewer);
}

void epub_viewer_show_toc(EpubViewer *viewer) {
    if (!viewer || !viewer->reader || !viewer->screen) return;
    if (viewer->toc_list) { lv_obj_del_async(viewer->toc_list); viewer->toc_list = NULL; return; }

    lv_font_t *ui_font = &lv_font_misans_16;

    viewer->toc_list = lv_obj_create(viewer->screen);
    lv_obj_set_size(viewer->toc_list, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_align(viewer->toc_list, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(viewer->toc_list, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(viewer->toc_list, LV_OPA_COVER, 0);

    lv_obj_t *close_btn = lv_btn_create(viewer->toc_list);
    lv_obj_set_size(close_btn, 50, 30);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -10, 5);
    lv_obj_set_style_border_width(close_btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(close_btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_user_data(close_btn, viewer);
    lv_obj_add_event_cb(close_btn, toc_btn_close_cb, LV_EVENT_CLICKED, viewer);

    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "X");
    lv_obj_set_style_text_font(close_label, ui_font, 0);
    lv_obj_center(close_label);

    int toc_count = epub_reader_get_toc_count(viewer->reader);
    if (toc_count > 0) {
        lv_obj_t *list = lv_list_create(viewer->toc_list);
        lv_obj_set_size(list, SCREEN_WIDTH - 20, SCREEN_HEIGHT - 60);
        lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_set_style_pad_row(list, 2, 0);
        for (int i = 0; i < toc_count && i < 20; i++) {
            EpubTocEntry *toc = epub_reader_get_toc(viewer->reader, i);
            if (toc) {
                lv_obj_t *btn = lv_list_add_btn(list, NULL, toc->title);
                lv_obj_set_style_text_font(btn, ui_font, 0);
                lv_obj_set_user_data(btn, viewer);
                lv_obj_add_event_cb(btn, toc_item_cb, LV_EVENT_CLICKED, NULL);
            }
        }
    }
    epd_mark_refresh_pending();
}

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
    if (viewer->toc_list) { lv_obj_del_async(viewer->toc_list); viewer->toc_list = NULL; }
}

static void toc_btn_close_cb(lv_event_t *e) {
    EpubViewer *viewer = (EpubViewer*)lv_obj_get_user_data(lv_event_get_target(e));
    if (viewer && viewer->toc_list) { lv_obj_del_async(viewer->toc_list); viewer->toc_list = NULL; epd_mark_refresh_pending(); }
}

int epub_viewer_get_current_chapter(EpubViewer *viewer) { return viewer ? viewer->current_chapter : 0; }
int epub_viewer_get_current_page(EpubViewer *viewer) { return viewer ? viewer->current_page + 1 : 1; }
int epub_viewer_get_total_pages(EpubViewer *viewer) { return viewer ? viewer->total_pages : 1; }
void epub_viewer_set_chapter_loaded_cb(EpubViewer *viewer, epub_chapter_loaded_cb cb) { if (viewer) viewer->chapter_cb = cb; }
void epub_viewer_set_close_cb(EpubViewer *viewer, epub_close_callback_t cb) { if (viewer) viewer->close_cb = cb; }

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
            if (i + 3 < len && strncasecmp(html + i, "&lt;", 4) == 0) { output[out_pos++] = '<'; i += 4; continue; }
            if (i + 3 < len && strncasecmp(html + i, "&gt;", 4) == 0) { output[out_pos++] = '>'; i += 4; continue; }
            if (i + 4 < len && strncasecmp(html + i, "&amp;", 5) == 0) { output[out_pos++] = '&'; i += 5; continue; }
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
