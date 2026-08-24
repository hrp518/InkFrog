/**
 * @file txt_viewer.c
 * @brief TXT 阅读器实现（与 epub_viewer 同构）
 *
 * 渲染核心：update_display() 镜像 epub_viewer.c:556 的逐行排版算法，
 * 但取字符来源从「静态 decoded_text_buf」改为「原始字节窗口 + gbk_decode_next」。
 *
 * 偏移空间 = 原始文件字节。所有 read_offset/page_end_offset/history/书签/百分比
 * 都基于原始字节，跨 UTF-8/GBK/UTF-16 编码统一（UTF-16 额外做 2 字节码元对齐）。
 *
 * 解耦：
 *   - 不含任何 EPUB/XHTML 解析代码。
 *   - 字体经 file_manager.c 公共 API（get_reader_font* / file_manager_set_reader_font_size）。
 *   - 书签经 settings_storage.c。
 *   - 编码经 gbk.c。
 */

#include "txt_viewer.h"
#include "gbk.h"
#include "file_manager.h"
#include "settings_storage.h"
#include "lv_port_disp.h"
#include "lvgl/lvgl.h"
#include "fs/fatfs/ff.h"
#include "loading.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

extern void epd_mark_refresh_pending(void);
extern void epd_disable_all_animations_recursive(lv_obj_t *obj);

#define TV_DEBUG 1
#if TV_DEBUG
#define TV_LOG(fmt, ...) printf("[TV] " fmt, ##__VA_ARGS__)
#define TV_ERR(fmt, ...) printf("[TV ERR] " fmt, ##__VA_ARGS__)
#else
#define TV_LOG(fmt, ...)
#define TV_ERR(fmt, ...)
#endif

/* ========== 布局常量（与 epub_viewer 完全一致，保证视觉同构） ========== */

#define SCREEN_WIDTH   240
#define SCREEN_HEIGHT  415

#define CONTENT_X      10
#define CONTENT_Y      14
#define CONTENT_WIDTH  (SCREEN_WIDTH - 20)   /* 220 */
#define CONTENT_HEIGHT (SCREEN_HEIGHT - 18)  /* 397 */

#define TOOLBAR_HEIGHT    130
#define TOOLBAR_TRIGGER_Y 130

/* ========== 字号挡位（与 epub_viewer 一致） ========== */

#define FONT_SIZE_COUNT 5
typedef struct {
    int font_size;   /* 正文字体像素高度 */
    int lh_body;     /* 正文行高 */
} FontSizeConfig;

/* TXT 只用正文字号（无 H1/H2/H3），挡位表与 epub 的 lh_body 一致 */
static const FontSizeConfig g_font_sizes[FONT_SIZE_COUNT] = {
    { 14, 19 },
    { 16, 22 },
    { 18, 25 },
    { 20, 28 },
    { 22, 31 },
};

/* 当前字号挡位（默认 3 = 18px）。注意：epub_viewer 也有同名 static，
 * 同一时刻只有一个 viewer 在运行，最终都走 file_manager_set_reader_font_size()
 * 操作同一份全局 TTF 字体状态，互不干扰。 */
static int g_font_size_index = 2;

#define OFFSET_HISTORY_SIZE 32
#define ESTIMATED_PAGE_BYTES 3000   /* 上一页回退估算（原始字节） */

/* 流式窗口大小：远大于单页文本量(~3KB)，保证一页不会跨窗口。
 * 放 .psram_bss 与 epub 静态缓冲同段，二者不会同时存活。 */
#define RAW_WINDOW_SIZE (32 * 1024)
static uint8_t s_raw_window[RAW_WINDOW_SIZE] __attribute__((section(".psram_bss")));

/* 字体（直接复用 file_manager 全局 TTF） */
#define FONT get_reader_font()

LV_FONT_DECLARE(lv_font_misans_16);

/* ========== TxtViewer 结构体 ========== */

struct TxtViewer {
    lv_obj_t *screen;
    lv_obj_t *content_container;

    char filepath[SETTINGS_MAX_PATH];   /* 书签 key */
    char filename[64];                  /* 工具栏标题（不含路径） */

    /* 文件状态 */
    FIL fp;                  /* 整个阅读期间保持打开，seek + 窗口读 */
    bool fp_open;
    TxtEncoding encoding;
    int bom_size;            /* 跳过 BOM 后有效内容起始字节 */
    int file_size;           /* 文件总字节数（含 BOM） */
    int content_size;        /* 有效内容字节数 = file_size - bom_size */

    /* 窗口状态 */
    int window_start;        /* s_raw_window[0] 对应的原始文件偏移 */
    int window_len;          /* 窗口有效字节数 */

    /* 字节偏移定位（全部原始字节空间） */
    int read_offset;         /* 当前页起始（原始偏移） */
    int page_end_offset;     /* 当前页结束（原始偏移） */

    /* 偏移历史栈 */
    int offset_history[OFFSET_HISTORY_SIZE];
    int history_head;
    int history_count;

    txt_close_callback_t close_cb;

    /* UI 元素 - 工具栏 */
    lv_obj_t *toolbar;
    lv_obj_t *toolbar_title;
    lv_obj_t *toolbar_pct_label;
    lv_obj_t *toolbar_goto_ta;
    bool toolbar_visible;

    /* UI 元素 - 跳转键盘 */
    lv_obj_t *goto_screen;
    lv_obj_t *goto_ta;
    lv_obj_t *goto_kb;
};

extern lv_font_t *get_reader_font(void);

/* ========== 前向声明 ========== */

static void update_display(TxtViewer *viewer);
static void prev_page_handler(TxtViewer *viewer);
static void next_page_handler(TxtViewer *viewer);
static void content_area_event_cb(lv_event_t *e);
static void close_viewer_cb(lv_event_t *e);
static void font_size_btn_cb(lv_event_t *e);
static void toolbar_close_cb(lv_event_t *e);
static void goto_ta_clicked_cb(lv_event_t *e);
static void goto_cancel_cb(lv_event_t *e);
static void goto_confirm_cb(lv_event_t *e);
static void toggle_toolbar(TxtViewer *viewer);
static void create_toolbar(TxtViewer *viewer);
static void update_toolbar_info(TxtViewer *viewer);
static void show_goto_keyboard(TxtViewer *viewer);
static void cleanup_goto_screen(TxtViewer *viewer);
static int  fill_window(TxtViewer *viewer, int offset);

/* ========== 偏移历史栈 ========== */

static void history_push(TxtViewer *v, int offset) {
    if (!v) return;
    v->offset_history[v->history_head] = offset;
    v->history_head = (v->history_head + 1) % OFFSET_HISTORY_SIZE;
    if (v->history_count < OFFSET_HISTORY_SIZE) v->history_count++;
}

static int history_pop(TxtViewer *v) {
    if (!v || v->history_count == 0) return -1;
    v->history_head = (v->history_head - 1 + OFFSET_HISTORY_SIZE) % OFFSET_HISTORY_SIZE;
    v->history_count--;
    return v->offset_history[v->history_head];
}

static void history_clear(TxtViewer *v) {
    if (!v) return;
    v->history_head = 0;
    v->history_count = 0;
}

/* ========== 百分比 ========== */

static int get_read_percentage(TxtViewer *v) {
    if (!v || v->content_size <= 0) return 0;
    /* 有效进度 = (read_offset - bom_size) / content_size */
    int pos = v->read_offset - v->bom_size;
    if (pos <= 0) return 0;
    if (pos >= v->content_size) return 100;
    return (int)((long long)pos * 100 / (long long)v->content_size);
}

/* ========== 编码相关辅助（UTF-16 码元对齐 / 换行扫描） ========== */

/* UTF-16 的字符偏移必须与 bom_size 同奇偶（2 字节码元对齐），
 * 奇数偏移会落到字符中间。单字节编码原样返回。 */
static int align_char_boundary(TxtViewer *v, int offset) {
    if (v->encoding != TXT_ENC_UTF16LE && v->encoding != TXT_ENC_UTF16BE) return offset;
    if (((offset - v->bom_size) & 1) != 0) offset++;
    return offset;
}

/**
 * @brief 从窗口相对下标 rel 起向前找换行，返回行首的窗口相对下标；找不到返回 -1
 *
 * UTF-8/GBK：原始字节 0x0A 即换行（不会出现在多字节序列内部）。
 * UTF-16LE/BE：换行为 0A 00 / 00 0A 双字节，且只在与 bom_size 同奇偶的
 * 码元起点上认定——「上」(U+4E0A) 之类汉字的 0x0A 低位字节不是换行。
 */
static int scan_next_line_start(TxtViewer *v, int rel) {
    const uint8_t *buf = s_raw_window;
    int win_end = v->window_len;
    int limit = rel + 300;
    if (limit > win_end) limit = win_end;

    if (v->encoding == TXT_ENC_UTF16LE) {
        for (int i = rel; i < limit && i + 1 < win_end; i++) {
            if (((v->window_start + i - v->bom_size) & 1) == 0 &&
                buf[i] == 0x0A && buf[i + 1] == 0x00) {
                return i + 2;
            }
        }
    } else if (v->encoding == TXT_ENC_UTF16BE) {
        for (int i = rel; i < limit; i++) {
            if (i >= 1 && ((v->window_start + i - v->bom_size) & 1) == 1 &&
                buf[i] == 0x0A && buf[i - 1] == 0x00) {
                return i + 1;
            }
        }
    } else {
        for (int i = rel; i < limit; i++) {
            if (buf[i] == '\n') return i + 1;
        }
    }
    return -1;
}

/**
 * @brief 百分比(0-100) → 原始字节偏移，并向前到下一行行首对齐
 */
static int pct_to_offset(TxtViewer *v, int pct) {
    if (!v || v->content_size <= 0) return v->bom_size;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    int raw_off = v->bom_size + (int)((long long)v->content_size * pct / 100);
    if (raw_off <= v->bom_size) return v->bom_size;
    if (raw_off >= v->file_size) raw_off = v->file_size - 1;

    /* 用窗口读取后扫描换行 */
    if (fill_window(v, raw_off) <= 0) return align_char_boundary(v, v->bom_size);

    int idx = scan_next_line_start(v, raw_off - v->window_start);
    if (idx >= 0) {
        int aligned = v->window_start + idx;
        if (aligned >= v->file_size) aligned = v->file_size - 1;
        return align_char_boundary(v, aligned);
    }
    /* 300 字节内无换行，保持当前位置（UTF-16 仍做码元对齐） */
    return align_char_boundary(v, raw_off);
}

/* ========== 窗口读取 ========== */

/**
 * @brief 确保 offset 位于窗口内（不在则 seek+读）
 * @return 窗口有效字节数；<=0 表示读失败
 */
static int fill_window(TxtViewer *v, int offset) {
    if (!v || !v->fp_open) return 0;
    if (offset < 0) offset = 0;
    if (offset >= v->file_size) offset = (v->file_size > 0) ? v->file_size - 1 : 0;

    /* 已在窗口内：无需读 */
    if (offset >= v->window_start && offset < v->window_start + v->window_len
        && v->window_len > 0) {
        return v->window_len;
    }

    FRESULT res = f_lseek(&v->fp, offset);
    if (res != FR_OK) {
        TV_ERR("f_lseek to %d failed: %d\n", offset, res);
        return -1;
    }
    UINT br = 0;
    res = f_read(&v->fp, s_raw_window, RAW_WINDOW_SIZE, &br);
    if (res != FR_OK) {
        TV_ERR("f_read at %d failed: %d\n", offset, res);
        return -1;
    }
    v->window_start = offset;
    v->window_len = (int)br;
    return v->window_len;
}

/* ========== UTF-8 辅助（取字符宽度） ========== */

/* CJK 快速宽度缓存：同字号下所有 CJK advance width 相同，只查一次。
 * 与 epub_viewer.c 同样的优化手法。 */
static int g_cjk_adv_width = -1;
static lv_font_t *g_cjk_adv_font = NULL;

static int get_char_adv_width(uint32_t unicode, const lv_font_t *font) {
    if (unicode >= 0x4E00 && unicode <= 0x9FFF) {
        if (g_cjk_adv_font == font && g_cjk_adv_width > 0) return g_cjk_adv_width;
        lv_coord_t w = lv_font_get_glyph_width(font, unicode, 0);
        if (w > 0) { g_cjk_adv_width = (int)w; g_cjk_adv_font = (lv_font_t*)font; return g_cjk_adv_width; }
        return 22;
    }
    if (unicode >= 0x3000 && unicode <= 0x303F) {
        if (g_cjk_adv_font == font && g_cjk_adv_width > 0) return g_cjk_adv_width;
        lv_coord_t w = lv_font_get_glyph_width(font, 0x4E00, 0);
        if (w > 0) { g_cjk_adv_width = (int)w; g_cjk_adv_font = (lv_font_t*)font; return g_cjk_adv_width; }
        return 22;
    }
    lv_coord_t w = lv_font_get_glyph_width(font, unicode, 0);
    if (w > 0) return (int)w;
    if (unicode >= 0xFF00 && unicode <= 0xFFEF) return 22;
    return 11;
}

/* 计算字形位图超出字体行框顶部的像素数（防首行裁切，与 epub_viewer 同） */
static int get_glyph_top_overflow(const lv_font_t *font) {
    if (!font) return 4;
    lv_font_glyph_dsc_t g;
    if (!lv_font_get_glyph_dsc(font, &g, 0x4E2D, 0) || g.box_h == 0) return 4;
    int asc = (int)lv_font_get_line_height(font) - (int)font->base_line;
    int above = (int)g.box_h + (int)g.ofs_y - asc;
    return above > 0 ? above : 0;
}

/* 渲染保护标志：update_display 期间阻止 REFR_TIMER 并发（与 epub_viewer 同名 extern 互补） */
extern volatile int g_rendering_in_progress;

/* ========== 单行 label 创建（镜像 epub_viewer） ========== */

/* 诊断：本次 update_display 内 label 创建累计耗时（区分"建 label 对象"与"dsc 查询"） */
static uint32_t g_tv_label_ms = 0;

static void create_line_label(TxtViewer *v, const char *text, int len,
                              lv_font_t *font, int *y_offset) {
    if (len <= 0 || *y_offset >= CONTENT_HEIGHT) return;
    uint32_t t0 = xTaskGetTickCount();
    char buf[80];
    if (len > 79) len = 79;
    memcpy(buf, text, len);
    buf[len] = '\0';
    lv_obj_t *label = lv_label_create(v->content_container);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, *y_offset);
    lv_label_set_text(label, buf);
    g_tv_label_ms += xTaskGetTickCount() - t0;
}

/* ========== 核心渲染 ========== */

static void update_display(TxtViewer *v) {
    if (!v || !v->content_container) return;
    if (v->content_size <= 0) return;

    /* 诊断：加载时间构成（fill=读窗, layout=排版, labels=建 label 对象, refr=渲染） */
    uint32_t t0 = xTaskGetTickCount();
    uint32_t t_fill = t0;
    uint32_t t_layout = t0;
    g_tv_label_ms = 0;

    int start_offset = v->read_offset;
    if (start_offset < v->bom_size) start_offset = v->bom_size;
    if (start_offset >= v->file_size) {
        start_offset = (v->content_size > ESTIMATED_PAGE_BYTES)
                       ? v->file_size - ESTIMATED_PAGE_BYTES
                       : v->bom_size;
    }

    /* 读窗口（多数翻页命中已读窗口，免 IO） */
    if (fill_window(v, start_offset) <= 0) {
        TV_ERR("fill_window failed at %d\n", start_offset);
        return;
    }
    t_fill = xTaskGetTickCount();

    /* 隐藏容器 → clean → 重设尺寸：避免每个 label 创建产生 invalid area（O(n²)） */
    lv_tiny_ttf_reset_dsc_stats();
    lv_tiny_ttf_reset_dsc_l2_cache();
    lv_tiny_ttf_set_dsc_phase(1);
    g_rendering_in_progress = 1;

    lv_obj_add_flag(v->content_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(v->content_container);
    lv_obj_set_size(v->content_container, CONTENT_WIDTH, CONTENT_HEIGHT);
    lv_obj_align(v->content_container, LV_ALIGN_TOP_LEFT, CONTENT_X, CONTENT_Y);

    const FontSizeConfig *fsz = &g_font_sizes[g_font_size_index];
    int y_offset = get_glyph_top_overflow(FONT);
    int current_lh = fsz->lh_body;
    lv_font_t *current_font = FONT;

    /* 逐字符排版：从窗口解码下一个字符（UTF-8/GBK 通用） */
    const uint8_t *win = s_raw_window;
    int rel = start_offset - v->window_start;
    const uint8_t *p = win + rel;
    const uint8_t *win_end = win + v->window_len;

    char line_buf[80];
    int line_pos = 0;
    int line_width = 0;

    while (p < win_end && y_offset + current_lh <= CONTENT_HEIGHT) {
        /* 先解码下一个字符（unicode + 占用原始字节数），换行判断走解码值：
         * UTF-16 的换行是 0A 00 / 00 0A 双字节，原始单字节 0x0A 可能只是
         * 某汉字的低位字节（如「上」U+4E0A），不能按原始字节判断。 */
        uint32_t unicode = 0;
        int adv_raw = gbk_decode_next(v->encoding, p, win_end, &unicode);
        if (adv_raw <= 0) break;

        /* 换行（解码值，各编码统一） */
        if (unicode == '\n') {
            if (line_pos > 0) {
                create_line_label(v, line_buf, line_pos, current_font, &y_offset);
                y_offset += current_lh;
                line_pos = 0;
                line_width = 0;
                if (y_offset + current_lh > CONTENT_HEIGHT) { p += adv_raw; break; }
            }
            p += adv_raw;
            continue;
        }

        /* 跳过 \r（CRLF / 老 Mac 风格）与 NUL */
        if (unicode == '\r' || unicode == 0x00) {
            p += adv_raw;
            continue;
        }

        /* 空白类字符归一化为半角空格（与 epub filter_unsupported_chars 一致） */
        int render_adv;
        if (unicode == 0x09 || unicode == 0x00A0 || unicode == 0x3000 ||
            unicode == 0x202F || unicode == 0x205F || unicode == 0xFEFF ||
            (unicode >= 0x2000 && unicode <= 0x200D)) {
            unicode = ' ';
        }

        render_adv = get_char_adv_width(unicode, current_font);

        /* 超过行宽则先换行 */
        if (line_width + render_adv > CONTENT_WIDTH && line_pos > 0) {
            create_line_label(v, line_buf, line_pos, current_font, &y_offset);
            y_offset += current_lh;
            line_pos = 0;
            line_width = 0;
            if (y_offset + current_lh > CONTENT_HEIGHT) {
                /* 页面已满：不前进 p，让当前字符成为下一页起始
                 * （比 epub_viewer 多前进一个字符更准确，避免丢字） */
                break;
            }
        }

        /* 把该字符的 UTF-8 编码写入 line_buf（label 用 UTF-8）。
         * 先算字节数再判余量，防止 4 字节字符在 line_pos=77/78 时越界写 */
        int nb = 1;
        if (unicode >= 0x80) nb = (unicode < 0x800) ? 2 : ((unicode < 0x10000) ? 3 : 4);
        if (line_pos + nb <= 78) {
            if (unicode < 0x80) {
                line_buf[line_pos++] = (char)unicode;
            } else if (unicode < 0x800) {
                line_buf[line_pos++] = (char)(0xC0 | (unicode >> 6));
                line_buf[line_pos++] = (char)(0x80 | (unicode & 0x3F));
            } else if (unicode < 0x10000) {
                line_buf[line_pos++] = (char)(0xE0 | (unicode >> 12));
                line_buf[line_pos++] = (char)(0x80 | ((unicode >> 6) & 0x3F));
                line_buf[line_pos++] = (char)(0x80 | (unicode & 0x3F));
            } else {
                line_buf[line_pos++] = (char)(0xF0 | (unicode >> 18));
                line_buf[line_pos++] = (char)(0x80 | ((unicode >> 12) & 0x3F));
                line_buf[line_pos++] = (char)(0x80 | ((unicode >> 6) & 0x3F));
                line_buf[line_pos++] = (char)(0x80 | (unicode & 0x3F));
            }
        }
        line_width += render_adv;
        p += adv_raw;
    }

    /* flush 残留行 */
    if (line_pos > 0 && y_offset + current_lh <= CONTENT_HEIGHT) {
        create_line_label(v, line_buf, line_pos, current_font, &y_offset);
    }

    /* 记录本页结束的原始字节偏移 */
    v->page_end_offset = v->window_start + (int)(p - win);
    if (v->page_end_offset > v->file_size) v->page_end_offset = v->file_size;

    /* 更新工具栏信息 */
    update_toolbar_info(v);

    /* 布局完成：打印 dsc 统计与路径耗时构成（对齐 epub_viewer 的 DSC_STATS） */
    t_layout = xTaskGetTickCount();
    {
        int lt, lh, lm, lnc;
        int l_unique = lv_tiny_ttf_get_dsc_stats(&lt, &lh, &lm, &lnc);
        uint32_t l2ms, l2c, mms, mc, sms, sc;
        lv_tiny_ttf_get_dsc_timing(&l2ms, &l2c, &mms, &mc, &sms, &sc);
        printf("[DSC_STATS] layout phase: total=%d hits=%d misses=%d non_cjk=%d unique_miss=%d\n",
               lt, lh, lm, lnc, l_unique);
        printf("[DSC_TIME] layout: l2=%ums/%u metrics=%ums/%u stbtt=%ums/%u (ms/calls)\n",
               l2ms, l2c, mms, mc, sms, sc);
    }

    /* 恢复容器可见 + 同步渲染 + EPD 刷新 */
    lv_obj_clear_flag(v->content_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(v->content_container);

    lv_tiny_ttf_reset_dsc_stats();
    lv_tiny_ttf_reset_dsc_l2_cache();
    lv_tiny_ttf_set_dsc_phase(2);

    lv_tiny_ttf_bitmap_page_start();
    g_rendering_in_progress = 0;
    lv_refr_now(NULL);
    lv_tiny_ttf_bitmap_page_end();

    /* 调试：渲染阶段 dsc 统计 + 总体耗时构成 */
    {
        int rt, rh, rm, rnc;
        int r_unique = lv_tiny_ttf_get_dsc_stats(&rt, &rh, &rm, &rnc);
        printf("[DSC_STATS] render phase: total=%d hits=%d misses=%d non_cjk=%d unique_miss=%d l2_hits=%d\n",
               rt, rh, rm, rnc, r_unique, lv_tiny_ttf_get_l2_hits());
    }
    uint32_t t_refr = xTaskGetTickCount();
    printf("[TV_DBG] fill=%ums layout=%ums labels=%ums refr=%ums total=%ums offset=%d->%d pct=%d%%\n",
           (unsigned)(t_fill - t0), (unsigned)(t_layout - t_fill), (unsigned)g_tv_label_ms,
           (unsigned)(t_refr - t_layout), (unsigned)(t_refr - t0),
           start_offset, v->page_end_offset, txt_viewer_get_overall_pct(v));

    epd_mark_refresh_pending();

    /* 自动保存书签（chapter 恒 0，offset 为原始字节偏移） */
    if (v->filepath[0] != '\0') {
        int pct = txt_viewer_get_overall_pct(v);
        settings_save_bookmark(v->filepath, 0, v->read_offset, pct);
    }
}

/* ========== 创建/销毁 ========== */

TxtViewer* txt_viewer_create(const char *filepath) {
    if (!filepath || !filepath[0]) return NULL;

    TxtViewer *v = (TxtViewer*)calloc(1, sizeof(TxtViewer));
    if (!v) return NULL;

    strncpy(v->filepath, filepath, sizeof(v->filepath) - 1);
    v->filepath[sizeof(v->filepath) - 1] = '\0';

    const char *fname = strrchr(filepath, '/');
    if (fname) fname++; else fname = filepath;
    strncpy(v->filename, fname, sizeof(v->filename) - 1);
    v->filename[sizeof(v->filename) - 1] = '\0';

    /* 打开文件 + 检测编码 + 取大小 */
    FRESULT res = f_open(&v->fp, filepath, FA_READ);
    if (res != FR_OK) {
        TV_ERR("open %s failed: %d\n", filepath, res);
        free(v);
        return NULL;
    }
    v->fp_open = true;
    v->file_size = (int)f_size(&v->fp);
    v->encoding = gbk_detect_encoding(&v->fp);
    v->bom_size = gbk_bom_size(v->encoding);
    v->content_size = v->file_size - v->bom_size;
    if (v->content_size < 0) v->content_size = 0;

    /* 初始窗口：从 BOM 后开始 */
    v->read_offset = v->bom_size;
    v->page_end_offset = v->bom_size;
    v->window_start = -1;
    v->window_len = 0;

    TV_LOG("opened %s  enc=%d bom=%d size=%d content=%d\n",
           v->filename, (int)v->encoding, v->bom_size, v->file_size, v->content_size);

    return v;
}

void txt_viewer_show(TxtViewer *v) {
    if (!v) return;

    lv_font_t *ui_font = (lv_font_t*)&lv_font_misans_16;

    v->screen = lv_obj_create(NULL);
    lv_obj_set_size(v->screen, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(v->screen, lv_color_white(), 0);
    lv_obj_set_style_pad_all(v->screen, 0, 0);
    lv_obj_clear_flag(v->screen, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC |
                      LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN);
    epd_disable_all_animations_recursive(v->screen);

    v->content_container = lv_obj_create(v->screen);
    lv_obj_set_size(v->content_container, CONTENT_WIDTH, CONTENT_HEIGHT);
    lv_obj_align(v->content_container, LV_ALIGN_TOP_LEFT, CONTENT_X, CONTENT_Y);
    lv_obj_set_style_bg_color(v->content_container, lv_color_white(), 0);
    lv_obj_set_style_border_width(v->content_container, 0, 0);
    lv_obj_set_style_pad_all(v->content_container, 0, 0);
    lv_obj_add_flag(v->content_container, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(v->content_container, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC |
                      LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN);
    /* CLICKABLE + EVENT_BUBBLE：与 epub_viewer 同，缺 CLICKABLE 触摸事件被忽略 */
    lv_obj_add_flag(v->content_container, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    epd_disable_all_animations_recursive(v->content_container);

    create_toolbar(v);

    /* 全屏触控（RELEASED：抬手触发，不受长按阈值影响） */
    lv_obj_set_user_data(v->screen, v);
    lv_obj_add_event_cb(v->screen, content_area_event_cb, LV_EVENT_RELEASED, v);

    lv_disp_load_scr(v->screen);
    TV_LOG("shown\n");
}

void txt_viewer_close(TxtViewer *v) {
    if (!v) return;
    if (v->goto_screen) {
        if (v->goto_kb) { lv_obj_del(v->goto_kb); v->goto_kb = NULL; }
        if (v->goto_ta) { lv_obj_del(v->goto_ta); v->goto_ta = NULL; }
        lv_obj_del(v->goto_screen);
        v->goto_screen = NULL;
    }
    if (v->screen) { lv_obj_del_async(v->screen); v->screen = NULL; }
    v->content_container = NULL;
    v->toolbar = NULL;
    txt_close_callback_t cb = v->close_cb;
    v->close_cb = NULL;
    if (cb) cb();
}

void txt_viewer_destroy(TxtViewer *v) {
    if (!v) return;
    txt_viewer_close(v);
    if (v->fp_open) {
        f_close(&v->fp);
        v->fp_open = false;
    }
    free(v);
}

/* ========== 工具栏 ========== */

static void update_toolbar_info(TxtViewer *v) {
    if (!v || !v->toolbar) return;
    int pct = get_read_percentage(v);

    if (v->toolbar_title) {
        lv_label_set_text(v->toolbar_title, v->filename[0] ? v->filename : "TXT");
    }
    if (v->toolbar_pct_label) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(v->toolbar_pct_label, buf);
    }
}

static void create_toolbar(TxtViewer *v) {
    if (!v) return;
    lv_font_t *ui_font = (lv_font_t*)&lv_font_misans_16;

    v->toolbar = lv_obj_create(v->screen);
    lv_obj_set_size(v->toolbar, SCREEN_WIDTH, TOOLBAR_HEIGHT);
    lv_obj_set_pos(v->toolbar, 0, -TOOLBAR_HEIGHT);
    lv_obj_add_flag(v->toolbar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(v->toolbar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(v->toolbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(v->toolbar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(v->toolbar, 2, 0);
    lv_obj_set_style_border_color(v->toolbar, lv_color_black(), 0);
    lv_obj_set_style_pad_all(v->toolbar, 4, 0);
    lv_obj_clear_flag(v->toolbar, LV_OBJ_FLAG_SCROLLABLE);
    epd_disable_all_animations_recursive(v->toolbar);
    v->toolbar_visible = false;

    /* 第1行：标题 + 百分比 + X 关闭 */
    v->toolbar_title = lv_label_create(v->toolbar);
    lv_label_set_text(v->toolbar_title, v->filename[0] ? v->filename : "TXT");
    lv_obj_set_style_text_font(v->toolbar_title, ui_font, 0);
    lv_obj_set_style_text_color(v->toolbar_title, lv_color_black(), 0);
    lv_obj_set_pos(v->toolbar_title, 70, 4);
    lv_obj_set_size(v->toolbar_title, 95, 16);
    lv_label_set_long_mode(v->toolbar_title, LV_LABEL_LONG_DOT);

    v->toolbar_pct_label = lv_label_create(v->toolbar);
    lv_label_set_text(v->toolbar_pct_label, "0%");
    lv_obj_set_style_text_font(v->toolbar_pct_label, ui_font, 0);
    lv_obj_set_style_text_color(v->toolbar_pct_label, lv_color_make(0x66, 0x66, 0x66), 0);
    lv_obj_set_pos(v->toolbar_pct_label, 172, 4);

    lv_obj_t *close_btn = lv_btn_create(v->toolbar);
    lv_obj_set_size(close_btn, 60, 30);
    lv_obj_set_pos(close_btn, 2, 2);
    lv_obj_set_style_bg_color(close_btn, lv_color_white(), 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_border_color(close_btn, lv_color_make(0x99, 0x99, 0x99), 0);
    lv_obj_set_style_radius(close_btn, 2, 0);
    lv_obj_set_style_transition(close_btn, NULL, LV_PART_MAIN);
    lv_obj_add_event_cb(close_btn, toolbar_close_cb, LV_EVENT_CLICKED, v);
    epd_disable_all_animations_recursive(close_btn);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "X");
    lv_obj_set_style_text_font(close_lbl, ui_font, 0);
    lv_obj_center(close_lbl);

    /* 第2行：Home（返回）+ Jump 百分比输入框（TXT 无目录）
     * Home 高度 = 首页功能磁贴(84px)的一半 = 42px */
    int btn_w = 112, btn_h = 42, btn_y = 28, gap = 8;
    int btn_x = 4;

    /* Home */
    lv_obj_t *btn_back = lv_btn_create(v->toolbar);
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
    lv_obj_add_event_cb(btn_back, close_viewer_cb, LV_EVENT_CLICKED, v);
    btn_x += btn_w + gap;

    /* Jump 输入框（点击弹数字键盘） */
    lv_obj_t *goto_label = lv_label_create(v->toolbar);
    lv_label_set_text(goto_label, "Jump");
    lv_obj_set_style_text_font(goto_label, ui_font, 0);
    lv_obj_set_pos(goto_label, btn_x, btn_y + 5);

    v->toolbar_goto_ta = lv_textarea_create(v->toolbar);
    lv_obj_set_size(v->toolbar_goto_ta, 70, 24);
    lv_obj_set_pos(v->toolbar_goto_ta, btn_x + 38, btn_y - 1);
    lv_textarea_set_text(v->toolbar_goto_ta, "0");
    lv_textarea_set_accepted_chars(v->toolbar_goto_ta, "0123456789.");
    lv_textarea_set_max_length(v->toolbar_goto_ta, 6);
    lv_obj_set_style_text_font(v->toolbar_goto_ta, ui_font, 0);
    lv_obj_set_style_border_width(v->toolbar_goto_ta, 1, 0);
    lv_obj_set_style_border_color(v->toolbar_goto_ta, lv_color_make(0x99, 0x99, 0x99), 0);
    epd_disable_all_animations_recursive(v->toolbar_goto_ta);
    lv_obj_add_event_cb(v->toolbar_goto_ta, goto_ta_clicked_cb, LV_EVENT_CLICKED, v);

    lv_obj_t *pct_sign = lv_label_create(v->toolbar);
    lv_label_set_text(pct_sign, "%");
    lv_obj_set_style_text_font(pct_sign, ui_font, 0);
    lv_obj_set_pos(pct_sign, btn_x + 112, btn_y + 5);

    /* 第3行：5 挡字号按钮 */
    static const char *font_labels[FONT_SIZE_COUNT] = {"S", "s", "M", "L", "XL"};
    int fs_btn_w = 38, fs_btn_h = 24, fs_btn_y = 76;   /* Home 加高后下移避免重叠 */
    int fs_total_w = FONT_SIZE_COUNT * fs_btn_w + (FONT_SIZE_COUNT - 1) * 4;
    int fs_start_x = (SCREEN_WIDTH - fs_total_w) / 2;

    for (int i = 0; i < FONT_SIZE_COUNT; i++) {
        lv_obj_t *fbtn = lv_btn_create(v->toolbar);
        lv_obj_set_size(fbtn, fs_btn_w, fs_btn_h);
        lv_obj_set_pos(fbtn, fs_start_x + i * (fs_btn_w + 4), fs_btn_y);
        lv_obj_set_style_radius(fbtn, 2, 0);
        lv_obj_set_style_transition(fbtn, NULL, LV_PART_MAIN);
        epd_disable_all_animations_recursive(fbtn);
        if (i == g_font_size_index) {
            lv_obj_set_style_bg_color(fbtn, lv_color_black(), 0);
            lv_obj_set_style_border_width(fbtn, 1, 0);
            lv_obj_set_style_border_color(fbtn, lv_color_black(), 0);
        } else {
            lv_obj_set_style_bg_color(fbtn, lv_color_white(), 0);
            lv_obj_set_style_border_width(fbtn, 1, 0);
            lv_obj_set_style_border_color(fbtn, lv_color_make(0x99, 0x99, 0x99), 0);
        }
        lv_obj_set_user_data(fbtn, (void*)(long)(100 + i));
        lv_obj_add_event_cb(fbtn, font_size_btn_cb, LV_EVENT_CLICKED, v);
        lv_obj_t *flbl = lv_label_create(fbtn);
        lv_label_set_text(flbl, font_labels[i]);
        lv_obj_set_style_text_font(flbl, ui_font, 0);
        if (i == g_font_size_index) lv_obj_set_style_text_color(flbl, lv_color_white(), 0);
        lv_obj_center(flbl);
    }

    epd_mark_refresh_pending();
}

static void toggle_toolbar(TxtViewer *v) {
    if (!v || !v->toolbar) return;
    v->toolbar_visible = !v->toolbar_visible;
    if (v->toolbar_visible) {
        lv_obj_set_pos(v->toolbar, 0, 0);
        lv_obj_clear_flag(v->toolbar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(v->toolbar);
        lv_obj_invalidate(v->toolbar);
    } else {
        lv_obj_add_flag(v->toolbar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(v->toolbar, 0, -TOOLBAR_HEIGHT);
    }
    epd_mark_refresh_pending();
}

/* ========== 跳转键盘（复刻 epub_viewer） ========== */

/* 默认 lv_keyboard_def_event_cb 只认识 LV_SYMBOL_* 符号串, 文本 "Del"/"OK" 和
 * 裸 "\x11"/"\x12" 都会被当文本打进 textarea 再被 accepted_chars 过滤 → 死键。
 * 用 goto_kb_event_cb 自定义回调处理; 确认只留底部 Go 按钮。 */
static const char * const kb_num_map[] = {
    "1", "2", "3", "Del", "\n",
    "4", "5", "6", ".",   "\n",
    "7", "8", "9", "0",   ""
};

static const lv_btnmatrix_ctrl_t kb_num_ctrl[] = {
    1, 1, 1, LV_KEYBOARD_CTRL_BTN_FLAGS | 1,
    1, 1, 1, 1,
    1, 1, 1, 1
};

static void goto_kb_event_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    lv_keyboard_t *keyboard = (lv_keyboard_t *)kb;
    uint16_t btn_id = lv_btnmatrix_get_selected_btn(kb);
    if (btn_id == LV_BTNMATRIX_BTN_NONE) return;
    const char *txt = lv_btnmatrix_get_btn_text(kb, btn_id);
    if (!txt) return;

    if (strcmp(txt, "Del") == 0) {
        if (keyboard->ta) {
            lv_textarea_del_char(keyboard->ta);
        }
        return;
    }
    if (keyboard->ta) {
        lv_textarea_add_text(keyboard->ta, txt);
    }
}

static void show_goto_keyboard(TxtViewer *v) {
    if (!v) return;
    lv_font_t *ui_font = (lv_font_t*)&lv_font_misans_16;

    v->goto_screen = lv_obj_create(NULL);
    lv_obj_set_size(v->goto_screen, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(v->goto_screen, lv_color_white(), 0);
    lv_obj_set_style_pad_all(v->goto_screen, 0, 0);
    epd_disable_all_animations_recursive(v->goto_screen);

    lv_obj_t *title = lv_label_create(v->goto_screen);
    lv_label_set_text(title, "Jump to");
    lv_obj_set_style_text_font(title, ui_font, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    v->goto_ta = lv_textarea_create(v->goto_screen);
    lv_obj_set_size(v->goto_ta, 140, 36);
    lv_obj_align(v->goto_ta, LV_ALIGN_TOP_MID, 0, 30);
    lv_textarea_set_text(v->goto_ta, "");
    lv_textarea_set_placeholder_text(v->goto_ta, "0~100");
    lv_textarea_set_accepted_chars(v->goto_ta, "0123456789.");
    lv_textarea_set_max_length(v->goto_ta, 6);
    lv_obj_set_style_text_font(v->goto_ta, ui_font, 0);
    epd_disable_all_animations_recursive(v->goto_ta);

    lv_obj_t *pct_sign = lv_label_create(v->goto_screen);
    lv_label_set_text(pct_sign, "%");
    lv_obj_set_style_text_font(pct_sign, ui_font, 0);
    lv_obj_set_style_text_color(pct_sign, lv_color_black(), 0);
    lv_obj_align_to(pct_sign, v->goto_ta, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    v->goto_kb = lv_keyboard_create(v->goto_screen);
    lv_obj_set_size(v->goto_kb, 230, 150);
    lv_obj_align(v->goto_kb, LV_ALIGN_BOTTOM_MID, 0, -35);
    lv_keyboard_set_map(v->goto_kb, LV_KEYBOARD_MODE_NUMBER, kb_num_map, kb_num_ctrl);
    lv_keyboard_set_mode(v->goto_kb, LV_KEYBOARD_MODE_NUMBER); /* 关键: 必须切到数字模式, 否则显示默认完整键盘 */
    lv_keyboard_set_textarea(v->goto_kb, v->goto_ta);
    /* 移除默认 def_event_cb (不认识 "Del" 会当文本敲), 换成自定义处理 */
    lv_obj_remove_event_cb(v->goto_kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(v->goto_kb, goto_kb_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    epd_disable_all_animations_recursive(v->goto_kb);

    lv_obj_t *btn_cancel = lv_btn_create(v->goto_screen);
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
    lv_obj_add_event_cb(btn_cancel, goto_cancel_cb, LV_EVENT_CLICKED, v);

    lv_obj_t *btn_confirm = lv_btn_create(v->goto_screen);
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
    lv_obj_add_event_cb(btn_confirm, goto_confirm_cb, LV_EVENT_CLICKED, v);

    lv_disp_load_scr(v->goto_screen);
    epd_mark_refresh_pending();
}

static void cleanup_goto_screen(TxtViewer *v) {
    if (!v) return;
    if (v->screen) lv_disp_load_scr(v->screen);
    if (v->goto_kb) { lv_obj_del(v->goto_kb); v->goto_kb = NULL; }
    if (v->goto_ta) { lv_obj_del(v->goto_ta); v->goto_ta = NULL; }
    if (v->goto_screen) {
        lv_obj_del(v->goto_screen);
        v->goto_screen = NULL;
    }
}

/* ========== 翻页 ========== */

static void next_page_handler(TxtViewer *v) {
    if (!v) return;
    /* 已到文件末尾 */
    if (v->page_end_offset >= v->file_size) return;

    /* 防止空页死循环（UTF-16 按 2 字节码元前进，保持奇偶对齐） */
    if (v->read_offset == v->page_end_offset && v->page_end_offset < v->file_size) {
        v->page_end_offset +=
            (v->encoding == TXT_ENC_UTF16LE || v->encoding == TXT_ENC_UTF16BE) ? 2 : 1;
    }

    history_push(v, v->read_offset);
    v->read_offset = v->page_end_offset;
    update_display(v);
}

static void prev_page_handler(TxtViewer *v) {
    if (!v) return;

    int prev_offset = history_pop(v);
    if (prev_offset >= v->bom_size && prev_offset < v->read_offset) {
        v->read_offset = prev_offset;
        update_display(v);
        return;
    }

    /* 无历史：估算回退并换行对齐 */
    if (v->read_offset > v->bom_size) {
        int back = v->read_offset - ESTIMATED_PAGE_BYTES;
        if (back < v->bom_size) back = v->bom_size;

        if (fill_window(v, back) <= 0) return;

        /* 换行对齐：找下一行行首（编码感知，UTF-16 码元对齐） */
        int idx = scan_next_line_start(v, back - v->window_start);
        if (idx >= 0) back = v->window_start + idx;

        v->read_offset = align_char_boundary(v, back);
        update_display(v);
    }
}

/* ========== 触控 ========== */

static void content_area_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_RELEASED) return;

    TxtViewer *v = (TxtViewer *)lv_event_get_user_data(e);
    if (!v) return;

    lv_point_t point;
    lv_indev_get_point(lv_indev_get_act(), &point);
    int x = point.x, y = point.y;

    /* 跳转键盘打开时忽略 */
    if (v->goto_screen) return;
    /* 工具栏区域让工具栏自己处理 */
    if (v->toolbar_visible && y < TOOLBAR_HEIGHT) return;

    /* 顶部 → 弹出/关闭工具栏 */
    if (y < TOOLBAR_TRIGGER_Y) {
        toggle_toolbar(v);
        return;
    }
    /* 工具栏打开时点其他区域 → 关闭工具栏 */
    if (v->toolbar_visible) {
        toggle_toolbar(v);
        return;
    }

    /* 左 1/3 → 上一页；中间 + 右 2/3 → 下一页 */
    if (x < SCREEN_WIDTH / 3) {
        prev_page_handler(v);
    } else {
        next_page_handler(v);
    }
}

/* ========== 工具栏回调 ========== */

static void font_size_btn_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    int raw = (int)(long)lv_obj_get_user_data(btn);
    int idx = raw - 100;
    if (idx < 0 || idx >= FONT_SIZE_COUNT) return;

    TxtViewer *v = (TxtViewer *)lv_event_get_user_data(e);
    if (!v) return;
    if (g_font_size_index == idx) return;

    g_font_size_index = idx;
    file_manager_set_reader_font_size(g_font_sizes[idx].font_size);

    /* 重置 CJK 宽度缓存 */
    g_cjk_adv_width = -1;
    g_cjk_adv_font = NULL;

    /* 更新挡位按钮高亮（必须在 update_display 之前，否则 EPD 刷旧样式） */
    if (v->toolbar) {
        uint32_t ci, total = lv_obj_get_child_cnt(v->toolbar);
        for (ci = 0; ci < total; ci++) {
            lv_obj_t *child = lv_obj_get_child(v->toolbar, ci);
            if (child && lv_obj_check_type(child, &lv_btn_class)) {
                int child_idx = (int)(long)lv_obj_get_user_data(child);
                if (child_idx >= 100 && child_idx < 100 + FONT_SIZE_COUNT) {
                    int gear = child_idx - 100;
                    if (gear == idx) {
                        lv_obj_set_style_bg_color(child, lv_color_black(), 0);
                        lv_obj_set_style_border_color(child, lv_color_black(), 0);
                        lv_obj_t *lbl = lv_obj_get_child(child, 0);
                        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
                    } else {
                        lv_obj_set_style_bg_color(child, lv_color_white(), 0);
                        lv_obj_set_style_border_color(child, lv_color_make(0x99, 0x99, 0x99), 0);
                        lv_obj_t *lbl = lv_obj_get_child(child, 0);
                        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
                    }
                }
            }
        }
        lv_obj_invalidate(v->toolbar);
    }

    update_display(v);
}

static void toolbar_close_cb(lv_event_t *e) {
    TxtViewer *v = (TxtViewer *)lv_event_get_user_data(e);
    if (v && v->toolbar_visible) toggle_toolbar(v);
}

static void close_viewer_cb(lv_event_t *e) {
    TxtViewer *v = (TxtViewer *)lv_event_get_user_data(e);
    if (v) {
        if (v->toolbar_visible) toggle_toolbar(v);
        txt_viewer_close(v);
    }
}

static void goto_ta_clicked_cb(lv_event_t *e) {
    TxtViewer *v = (TxtViewer *)lv_event_get_user_data(e);
    if (!v) return;
    if (v->toolbar_visible) toggle_toolbar(v);
    show_goto_keyboard(v);
}

static void goto_cancel_cb(lv_event_t *e) {
    TxtViewer *v = (TxtViewer *)lv_event_get_user_data(e);
    cleanup_goto_screen(v);
    epd_mark_refresh_pending();
}

static void goto_confirm_cb(lv_event_t *e) {
    TxtViewer *v = (TxtViewer *)lv_event_get_user_data(e);
    if (!v || !v->goto_ta) return;

    const char *text = lv_textarea_get_text(v->goto_ta);
    float pct = 0.0f;
    if (text && text[0]) pct = (float)atof(text);
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;

    cleanup_goto_screen(v);
    history_clear(v);
    if (v->toolbar_visible) toggle_toolbar(v);

    int new_offset = pct_to_offset(v, (int)pct);
    v->read_offset = new_offset;
    update_display(v);
}

/* ========== 公共接口 ========== */

void txt_viewer_goto_offset(TxtViewer *v, int raw_offset) {
    if (!v) return;
    if (raw_offset < v->bom_size) raw_offset = v->bom_size;
    if (raw_offset >= v->file_size) raw_offset = (v->file_size > 0) ? v->file_size - 1 : 0;
    history_clear(v);
    v->read_offset = align_char_boundary(v, raw_offset);
    update_display(v);
}

void txt_viewer_refresh(TxtViewer *v) {
    update_display(v);
}

void txt_viewer_set_close_cb(TxtViewer *v, txt_close_callback_t cb) {
    if (v) v->close_cb = cb;
}

int txt_viewer_get_read_offset(TxtViewer *v) {
    return v ? v->read_offset : 0;
}

int txt_viewer_get_overall_pct(TxtViewer *v) {
    return v ? get_read_percentage(v) : 0;
}
