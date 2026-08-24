/**
 * @file bookshelf.c
 * @brief 书架：扫描 0:/Inkbook 下 .txt/.epub，每页 6 本（2×3）
 *
 * 布局对齐 EPD 240×415：封面 66×99（2:3 竖长方形文字书脊框）
 * 书名：EPUB 优先 metadata (dc:title)，否则文件名
 * 进度：已读 xx%（书签章节 / 总章数估算）
 */

#include "bookshelf.h"
#include "file_manager.h"
#include "epub_reader.h"
#include "settings_storage.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "wifi_controller.h"
#include "wlan_manager.h"
#include "fs/fatfs/ff.h"
#include "lvgl/lvgl.h"
#include "loading.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifndef BS_ABS
#define BS_ABS(x) ((x) < 0 ? -(x) : (x))
#endif

extern void main_ui_create(void);
LV_FONT_DECLARE(lv_font_misans_16);

#define BS_SCREEN_W       240
#define BS_SCREEN_H       415
#define BS_HEADER_H       36
#define BS_FOOTER_H       16
#define BS_MARGIN_X       8
#define BS_MARGIN_Y       4
#define BS_GAP_X          8
#define BS_GAP_Y          4
#define BS_CELL_W         108
#define BS_CELL_H         117
#define BS_COVER_W        66
#define BS_COVER_H        99
#define BS_PER_PAGE       6
#define BS_MAX_BOOKS      64
#define BS_TITLE_LEN      64
#define BS_NAME_LEN       96
#define BS_PATH_LEN       192
#define BS_DIR            "/Inkbook"
#define BS_SWIPE_TH       8

typedef enum {
    BS_KIND_TXT = 0,
    BS_KIND_EPUB = 1
} BsKind;

typedef struct {
    char filename[BS_NAME_LEN];
    char filepath[BS_PATH_LEN];
    char title[BS_TITLE_LEN];
    BsKind kind;
    int progress;      /* 0..100 */
    int chapter_count; /* EPUB 缓存，未知为 0 */
} BookEntry;

static BookEntry *g_books = NULL;
static int g_book_count = 0;
static int g_page = 0;
static int g_page_count = 1;

static lv_obj_t *g_bs_screen = NULL;
static lv_obj_t *g_page_label = NULL;
static lv_obj_t *g_grid = NULL;
static lv_obj_t *g_dots_box = NULL;

static void bs_rebuild_ui(void);
static void bs_scan_library(void);
static void bs_fill_meta(BookEntry *b);
static int bs_calc_progress(const BookEntry *b);
static void bs_open_book(BookEntry *b);
static void bs_goto_page(int page);
static void bs_swipe_handler(int32_t delta_x, int32_t delta_y);
static void bs_physical_back(void);

static int str_ieq_ext(const char *name, const char *ext)
{
    size_t nlen = strlen(name);
    size_t elen = strlen(ext);
    if (nlen < elen) return 0;
    const char *p = name + nlen - elen;
    for (size_t i = 0; i < elen; i++) {
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)ext[i])) {
            return 0;
        }
    }
    return 1;
}

static void strip_ext_to(char *dst, size_t dst_sz, const char *filename)
{
    strncpy(dst, filename, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
    char *dot = strrchr(dst, '.');
    if (dot) *dot = '\0';
}

/* 规范化书架标题字符：lv_font_misans_16 仅含基本汉字，全角标点/特殊空白
 * 无字模会显示成方框。把全角标点转成半角、各类空白转半角空格。
 * 就地修改（输出长度 <= 输入长度，多字节→单字节只会变短）。 */
static void bs_normalize_title(char *s)
{
    if (!s) return;
    unsigned char *r = (unsigned char *)s;
    unsigned char *w = (unsigned char *)s;
    while (*r) {
        uint32_t u = 0;
        int cl = 0;
        if (r[0] < 0x80) { u = r[0]; cl = 1; }
        else if ((r[0] & 0xE0) == 0xC0 && r[1]) {
            u = ((r[0] & 0x1F) << 6) | (r[1] & 0x3F); cl = 2;
        } else if ((r[0] & 0xF0) == 0xE0 && r[1] && r[2]) {
            u = ((r[0] & 0x0F) << 12) | ((r[1] & 0x3F) << 6) | (r[2] & 0x3F); cl = 3;
        } else if ((r[0] & 0xF8) == 0xF0 && r[1] && r[2] && r[3]) {
            u = ((r[0] & 0x07) << 18) | ((r[1] & 0x3F) << 12) | ((r[2] & 0x3F) << 6) | (r[3] & 0x3F); cl = 4;
        } else { *w++ = *r++; continue; }

        /* 全角标点 U+FF01-FF5E → 对应半角 (偏移 0xFEE0) */
        if (u >= 0xFF01 && u <= 0xFF5E) {
            *w++ = (unsigned char)(u - 0xFEE0);
        }
        /* 各类空白字符 → 半角空格 */
        else if (u == 0x09 || u == 0x00A0 || u == 0x3000 || u == 0x202F ||
                 u == 0x205F || u == 0xFEFF || (u >= 0x2000 && u <= 0x200D)) {
            *w++ = ' ';
        }
        /* 其它 CJK 标点 → 半角等价物（与 epub_viewer filter 一致） */
        else if (u == 0x3001 || u == 0xFF0C) *w++ = ',';
        else if (u == 0x3002) *w++ = '.';
        else if (u == 0x201C || u == 0x201D) *w++ = '"';
        else if (u == 0x2018 || u == 0x2019) *w++ = '\'';
        else if (u == 0x2014 || u == 0x2015) *w++ = '-';
        else if (u == 0x300A) *w++ = '<';
        else if (u == 0x300B) *w++ = '>';
        else {
            for (int i = 0; i < cl; i++) *w++ = r[i];
        }
        r += cl;
    }
    *w = '\0';
}

static void make_meta_section(char *out, size_t out_sz, const char *filepath)
{
    /* settings section 名不宜过长；用 bookmeta + 截断路径 */
    snprintf(out, out_sz, "bookmeta:%s", filepath);
    out[out_sz - 1] = '\0';
}

static void ensure_book_dir(void)
{
    FRESULT res = f_mkdir(BS_DIR);
    if (res != FR_OK && res != FR_EXIST) {
        printf("[BS] mkdir %s failed: %d\n", BS_DIR, (int)res);
    }
}

static void bs_fill_meta(BookEntry *b)
{
    char section[SETTINGS_MAX_SECTION];
    char cached_title[BS_TITLE_LEN];
    char cached_ch[16];
    int have_title = 0;

    strip_ext_to(b->title, sizeof(b->title), b->filename);
    make_meta_section(section, sizeof(section), b->filepath);

    if (settings_get_string(section, "title", cached_title, sizeof(cached_title)) == 0 &&
        cached_title[0] != '\0') {
        strncpy(b->title, cached_title, sizeof(b->title) - 1);
        b->title[sizeof(b->title) - 1] = '\0';
        have_title = 1;
    }
    /* 文件名兜底标题也需规范化（含全角字符的文件名） */
    bs_normalize_title(b->title);

    b->chapter_count = 0;
    if (settings_get_string(section, "chapters", cached_ch, sizeof(cached_ch)) == 0) {
        b->chapter_count = atoi(cached_ch);
        if (b->chapter_count < 0) b->chapter_count = 0;
    }

    if (b->kind != BS_KIND_EPUB) {
        return;
    }

    /* 已有 title 缓存即跳过打开 EPUB。注意: 不能把跳过条件挂在 chapter_count>0 上——
     * spine_count 为 0 的书(chapters 只在 >0 时才写缓存)每次都会因 chapter_count==0
     * 而重新打开+解析 EPUB, 与是否新书无关, 导致每次进书架都慢/卡。 */
    if (have_title) {
        return;
    }

    EpubReader *r = epub_reader_create();
    if (!r) {
        return;
    }
    if (epub_reader_open(r, b->filepath)) {
        const char *t = epub_reader_get_title(r);
        if (t && t[0]) {
            strncpy(b->title, t, sizeof(b->title) - 1);
            b->title[sizeof(b->title) - 1] = '\0';
            bs_normalize_title(b->title);  /* 全角标点/空白→半角，避免书架字体方框 */
            settings_set_string(section, "title", b->title);
        }
        b->chapter_count = epub_reader_get_chapter_count(r);
        if (b->chapter_count > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", b->chapter_count);
            settings_set_string(section, "chapters", buf);
        }
        epub_reader_close(r);
    }
    epub_reader_destroy(r);
}

static int bs_calc_progress(const BookEntry *b)
{
    /* EPUB / TXT 统一：读阅读器渲染时存入的全书百分比（pct 字段）。
     * txt_viewer 每翻页按 read_offset/文件大小 计算并保存 pct，精确。 */
    return settings_load_bookmark_pct(b->filepath);
}

static void bs_scan_library(void)
{
    DIR dir;
    FILINFO fno;

    g_book_count = 0;
    ensure_book_dir();

    if (!g_books) {
        g_books = (BookEntry *)lv_mem_alloc(sizeof(BookEntry) * BS_MAX_BOOKS);
        if (!g_books) {
            printf("[BS] alloc books failed\n");
            return;
        }
    }
    memset(g_books, 0, sizeof(BookEntry) * BS_MAX_BOOKS);

    FRESULT res = f_opendir(&dir, BS_DIR);
    if (res != FR_OK) {
        printf("[BS] open dir %s failed: %d\n", BS_DIR, (int)res);
        g_page_count = 1;
        g_page = 0;
        return;
    }

    for (;;) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) {
            break;
        }
        if (fno.fattrib & AM_DIR) {
            continue;
        }
        if (fno.fname[0] == '.') {
            continue;
        }
        const char *name = fno.fname;
        int is_txt = str_ieq_ext(name, ".txt");
        int is_epub = str_ieq_ext(name, ".epub");
        if (!is_txt && !is_epub) {
            continue;
        }
        if (g_book_count >= BS_MAX_BOOKS) {
            break;
        }

        BookEntry *b = &g_books[g_book_count];
        memset(b, 0, sizeof(*b));
        strncpy(b->filename, name, sizeof(b->filename) - 1);
        snprintf(b->filepath, sizeof(b->filepath), "%s/%s", BS_DIR, name);
        b->kind = is_epub ? BS_KIND_EPUB : BS_KIND_TXT;
        g_book_count++;
    }
    f_closedir(&dir);

    printf("[BS] scanned %d books in %s\n", g_book_count, BS_DIR);

    for (int i = 0; i < g_book_count; i++) {
        bs_fill_meta(&g_books[i]);
        g_books[i].progress = bs_calc_progress(&g_books[i]);
        printf("[BS] #%d %s title='%s' prog=%d%%\n",
               i, g_books[i].filename, g_books[i].title, g_books[i].progress);
    }

    g_page_count = g_book_count > 0 ? (g_book_count + BS_PER_PAGE - 1) / BS_PER_PAGE : 1;
    if (g_page >= g_page_count) {
        g_page = g_page_count - 1;
    }
    if (g_page < 0) {
        g_page = 0;
    }
}

static void bs_book_clicked(lv_event_t *e)
{
    BookEntry *b = (BookEntry *)lv_event_get_user_data(e);
    if (!b) return;
    bs_open_book(b);
}

static void bs_open_book(BookEntry *b)
{
    printf("[BS] open %s\n", b->filepath);
    /* 离开书架网格滑动，避免阅读中误翻页 */
    touch_clear_swipe_callback();
    touch_set_swipe_area(0, 0);

    if (b->kind == BS_KIND_EPUB) {
        file_manager_open_epub(b->filepath, bookshelf_show);
    } else {
        file_manager_open_txt(b->filepath, bookshelf_show);
    }

    /* 阅读期间物理返回交由 FM 处理器接管: 它负责存书签并通过
     * g_reader_ui_return(=bookshelf_show) 正确回到书架, 而不是误回主页 */
    fm_assign_physical_back();
}

static void bs_back_cb(lv_event_t *e)
{
    (void)e;
    bookshelf_close();
}

static void bs_swipe_handler(int32_t delta_x, int32_t delta_y)
{
    int32_t adx = delta_x < 0 ? -delta_x : delta_x;
    int32_t ady = delta_y < 0 ? -delta_y : delta_y;

    if (adx > ady && adx > BS_SWIPE_TH) {
        /* 横向为主: 左滑(dx>0) 下一页, 右滑(dx<0) 上一页 */
        if (delta_x > 0) {
            bs_goto_page(g_page + 1);
        } else {
            bs_goto_page(g_page - 1);
        }
    } else if (ady > BS_SWIPE_TH) {
        /* 纵向为主: 上滑(dy>0) 下一页, 下滑(dy<0) 上一页 */
        if (delta_y > 0) {
            bs_goto_page(g_page + 1);
        } else {
            bs_goto_page(g_page - 1);
        }
    }
}

static void bs_physical_back(void)
{
    printf("[BS] physical back → home\n");
    bookshelf_close();
}

static void bs_update_dots(void)
{
    if (!g_dots_box) return;
    lv_obj_clean(g_dots_box);
    int n = g_page_count;
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) {
        lv_obj_t *d = lv_obj_create(g_dots_box);
        lv_obj_set_size(d, 5, 5);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(d, 1, 0);
        lv_obj_set_style_border_color(d, lv_color_black(), 0);
        lv_obj_set_style_bg_color(d, (i == g_page) ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_pad_all(d, 0, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        epd_disable_all_animations_recursive(d);
    }
}

static lv_obj_t *bs_create_cover(lv_obj_t *parent, BookEntry *b)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_size(cell, BS_CELL_W, BS_CELL_H);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    epd_disable_all_animations_recursive(cell);

    lv_obj_t *cover = lv_obj_create(cell);
    lv_obj_set_size(cover, BS_COVER_W, BS_COVER_H);
    lv_obj_align(cover, LV_ALIGN_TOP_MID, 0, 1);
    lv_obj_set_style_bg_color(cover, lv_color_white(), 0);
    lv_obj_set_style_border_width(cover, 2, 0);
    lv_obj_set_style_border_color(cover, lv_color_black(), 0);
    lv_obj_set_style_radius(cover, 0, 0);
    lv_obj_set_style_pad_left(cover, 8, 0);
    lv_obj_set_style_pad_right(cover, 4, 0);
    lv_obj_set_style_pad_top(cover, 6, 0);
    lv_obj_set_style_pad_bottom(cover, 10, 0);
    lv_obj_clear_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    epd_disable_all_animations_recursive(cover);

    /* 左侧书脊线 */
    lv_obj_t *spine = lv_obj_create(cover);
    lv_obj_set_size(spine, 3, BS_COVER_H - 4);
    lv_obj_align(spine, LV_ALIGN_LEFT_MID, -6, 0);
    lv_obj_set_style_bg_color(spine, lv_color_black(), 0);
    lv_obj_set_style_border_width(spine, 0, 0);
    lv_obj_set_style_radius(spine, 0, 0);
    lv_obj_clear_flag(spine, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(cover);
    lv_label_set_text(title, b->title);
    lv_obj_set_style_text_font(title, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(title, BS_COVER_W - 14);
    lv_obj_align(title, LV_ALIGN_CENTER, 2, -4);

    lv_obj_t *badge = lv_label_create(cover);
    lv_label_set_text(badge, b->kind == BS_KIND_EPUB ? "EPUB" : "TXT");
    lv_obj_set_style_text_font(badge, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(badge, lv_color_make(80, 80, 80), 0);
    lv_obj_align(badge, LV_ALIGN_BOTTOM_RIGHT, 0, 2);

    char prog[24];
    snprintf(prog, sizeof(prog), "已读 %d%%", b->progress);
    lv_obj_t *pl = lv_label_create(cell);
    lv_label_set_text(pl, prog);
    lv_obj_set_style_text_font(pl, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(pl, lv_color_black(), 0);
    lv_obj_align(pl, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_add_flag(cover, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cover, bs_book_clicked, LV_EVENT_CLICKED, b);
    return cell;
}

static void bs_render_page(void)
{
    if (!g_grid || !g_page_label) return;

    char buf[16];
    snprintf(buf, sizeof(buf), "%d / %d", g_page + 1, g_page_count);
    lv_label_set_text(g_page_label, buf);

    lv_obj_clean(g_grid);

    int start = g_page * BS_PER_PAGE;
    for (int i = 0; i < BS_PER_PAGE; i++) {
        int idx = start + i;
        int col = i % 2;
        int row = i / 2;
        lv_coord_t x = col * (BS_CELL_W + BS_GAP_X);
        lv_coord_t y = row * (BS_CELL_H + BS_GAP_Y);

        if (idx < g_book_count) {
            lv_obj_t *cell = bs_create_cover(g_grid, &g_books[idx]);
            lv_obj_set_pos(cell, x, y);
        }
    }
    bs_update_dots();
}

static void bs_goto_page(int page)
{
    if (page < 0 || page >= g_page_count) {
        return;
    }
    if (page == g_page) {
        return;
    }
    g_page = page;
    epd_set_content_dirty();
    bs_render_page();
    epd_mark_refresh_pending();
}

static void bs_rebuild_ui(void)
{
    if (g_bs_screen) {
        lv_obj_clean(g_bs_screen);
    } else {
        g_bs_screen = lv_obj_create(NULL);
        lv_obj_set_size(g_bs_screen, BS_SCREEN_W, BS_SCREEN_H);
        lv_obj_set_style_bg_color(g_bs_screen, lv_color_white(), 0);
        lv_obj_set_style_border_width(g_bs_screen, 0, 0);
        lv_obj_clear_flag(g_bs_screen, LV_OBJ_FLAG_SCROLLABLE);
        epd_disable_all_animations_recursive(g_bs_screen);
    }

    /* 滑动翻页全部交给驱动层 touch 回调(bs_swipe_handler), 取消 LVGL 事件判滑 */
    touch_set_swipe_area((int16_t)BS_HEADER_H, (int16_t)(BS_SCREEN_H - BS_FOOTER_H));

    /* 顶栏 */
    lv_obj_t *header = lv_obj_create(g_bs_screen);
    lv_obj_set_size(header, BS_SCREEN_W, BS_HEADER_H);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_white(), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, lv_color_black(), 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    epd_disable_all_animations_recursive(header);

    lv_obj_t *back = lv_btn_create(header);
    lv_obj_set_size(back, 60, 30);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(back, lv_color_white(), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_black(), 0);
    lv_obj_set_style_radius(back, 0, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    epd_disable_all_animations_recursive(back);
    lv_obj_add_event_cb(back, bs_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, "返回");
    lv_obj_set_style_text_font(bl, get_reader_font(), 0);
    lv_obj_center(bl);

    lv_obj_t *ht = lv_label_create(header);
    lv_label_set_text(ht, "书架");
    lv_obj_set_style_text_font(ht, &lv_font_misans_16, 0);
    lv_obj_align(ht, LV_ALIGN_CENTER, 0, 0);

    g_page_label = lv_label_create(header);
    lv_label_set_text(g_page_label, "1 / 1");
    lv_obj_set_style_text_font(g_page_label, &lv_font_montserrat_12, 0);
    lv_obj_align(g_page_label, LV_ALIGN_RIGHT_MID, -6, 0);

    /* 网格 */
    g_grid = lv_obj_create(g_bs_screen);
    lv_obj_set_size(g_grid, 2 * BS_CELL_W + BS_GAP_X, 3 * BS_CELL_H + 2 * BS_GAP_Y);
    lv_obj_set_pos(g_grid, BS_MARGIN_X, BS_HEADER_H + BS_MARGIN_Y);
    lv_obj_set_style_bg_opa(g_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_grid, 0, 0);
    lv_obj_set_style_pad_all(g_grid, 0, 0);
    lv_obj_clear_flag(g_grid, LV_OBJ_FLAG_SCROLLABLE);
    epd_disable_all_animations_recursive(g_grid);

    /* 底栏圆点 */
    g_dots_box = lv_obj_create(g_bs_screen);
    lv_obj_set_size(g_dots_box, BS_SCREEN_W, BS_FOOTER_H);
    lv_obj_align(g_dots_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(g_dots_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_dots_box, 0, 0);
    lv_obj_set_style_pad_all(g_dots_box, 0, 0);
    lv_obj_set_flex_flow(g_dots_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_dots_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_dots_box, 6, 0);
    lv_obj_clear_flag(g_dots_box, LV_OBJ_FLAG_SCROLLABLE);
    epd_disable_all_animations_recursive(g_dots_box);

    bs_render_page();
    lv_disp_load_scr(g_bs_screen);

    touch_register_swipe_callback(bs_swipe_handler);
    touch_register_back_btn_callback(bs_physical_back);
    touch_set_swipe_area(BS_HEADER_H, BS_SCREEN_H - BS_FOOTER_H);
}

/* 进书架时"先出遮罩、下一拍再解析"的延时与定时器。
 * 不能同步解析: EPD 硬件刷新需持 epd_lock, 而它是被 LVGL 主线程占着, 同步
 * 解析期间屏幕完全无法刷新, 点多少次都没反馈(看起来像卡死)。跨 tick 分两步,
 * 全留在 LVGL 线程, 不引入后台线程, 规避 FATFS/LVGL 内存管理并发冲突。 */
#define BS_LIB_DELAY_MS   80
static lv_timer_t * s_bs_lib_timer = NULL;

static void bs_library_task_cb(lv_timer_t * t)
{
    (void)t;
    s_bs_lib_timer = NULL;
    bookshelf_init();
    loading_hide();
}

void bookshelf_begin_load(void)
{
    if(s_bs_lib_timer) {
        lv_timer_del(s_bs_lib_timer);
        s_bs_lib_timer = NULL;
    }
    /* 1) 先推"正在解析..."遮罩并请求刷新, 让 EPD 把反馈刷上去; */
    loading_show("Parsing books...");
    epd_mark_refresh_pending();
    /* 2) 下一拍真正扫描/解析(此时遮罩已上屏, 解析期间用户看到的是"解析中")。 */
    s_bs_lib_timer = lv_timer_create(bs_library_task_cb, BS_LIB_DELAY_MS, NULL);
    lv_timer_set_repeat_count(s_bs_lib_timer, 1);
}

void bookshelf_init(void)
{
    printf("[BS] init\n");
    epd_pause_refresh();

    if (g_wifi.phase != WLAN_PHASE_CONNECTED) {
        wlan_manager_cancel_connect();
    }

    bs_scan_library();
    bs_rebuild_ui();

    epd_resume_refresh();
    epd_mark_refresh_pending();
}

void bookshelf_show(void)
{
    printf("[BS] show (return from reader)\n");
    epd_pause_refresh();

    /* 刷新书签进度 */
    for (int i = 0; i < g_book_count; i++) {
        /* EPUB 可能刚产生/更新了书签，刷新 chapters 缓存 */
        if (g_books[i].kind == BS_KIND_EPUB && g_books[i].chapter_count <= 0) {
            bs_fill_meta(&g_books[i]);
        }
        g_books[i].progress = bs_calc_progress(&g_books[i]);
    }

    bs_rebuild_ui();
    epd_resume_refresh();
    epd_mark_refresh_pending();
}

void bookshelf_close(void)
{
    printf("[BS] close → home\n");
    touch_clear_swipe_callback();
    touch_set_swipe_area(0, 0);
    touch_register_back_btn_callback(NULL);

    if (g_wifi.fm_paused) {
        g_wifi.fm_paused = 0;
        /* FM 入口时调了 wlan_sta_disable() 彻底关 WiFi, 退出后需重新启动 */
        printf("[BS] WiFi was paused by FM, requesting re-enable\n");
        wifi_controller_request_enable();
    }
    /* 注意: 不在这里调 font_warm_request —— L1 预热在 boot 时已完成,
     * 重复预热会阻塞 LVGL 线程(读 1.3MB glyf); 后台线程则和 LVGL
     * 内存管理冲突崩溃。boot warm 已覆盖这个路径。 */

    g_page_label = NULL;
    g_grid = NULL;
    g_dots_box = NULL;

    /* 【关键】用 lv_obj_clean 清空书架屏内容，不删除屏对象本身。
     * 若 lv_obj_del 删除当前活动屏幕，main_ui_create() 里 lv_scr_act()
     * 会返回已释放的野指针，lv_obj_clean 在其上递归 → 栈溢出/卡死。
     * 保留 g_bs_screen 对象，复用为首页屏（与 file_manager_close 一致）。
     * 下次 bookshelf_init 时 bs_rebuild_ui 会检测并复用该屏。 */
    if (g_bs_screen) {
        lv_obj_clean(g_bs_screen);
    }

    /* 活动屏仍是 g_bs_screen，main_ui_create 在其上重建首页 */
    main_ui_create();
    epd_mark_refresh_pending();
}
