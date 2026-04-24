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
#include "fs/fatfs/ff.h"

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
#define EPUB_PAGINATE_BUDGET       8       /* 每次增量分页最多处理的block数 */

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
    int known_pages;
    int current_page;
    bool pagination_complete;
    bool pagination_dirty;
    bool pagination_loaded_from_disk;
    char index_file_path[64];

    lv_timer_t *paginate_timer;
    uint32_t paginate_stream_offset;
    uint32_t paginate_block_start_offset;
    int paginate_page_y_offset;
    int paginate_current_level;
    int paginate_block_len;
    bool paginate_eof;

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
static int heading_need_next_page(EpubViewer *viewer, int current_level,
                                  int page_y_offset, int actual_h);
static int flush_render_block(EpubViewer *viewer, const char *block_start, int block_len,
                              lv_font_t *font, int line_height, int *y_offset);
static int build_decoded_stream(EpubViewer *viewer);

/* 流式解析核心 - 渐进式分页 */
static int prepare_chapter_stream(EpubViewer *viewer, int chapter_index);
static int scan_pages_from(EpubViewer *viewer, uint32_t start_offset, int start_page,
                           int start_y_offset, int start_level, int max_pages_to_find);
static void save_page_index(EpubViewer *viewer);
static void free_page_index(EpubViewer *viewer);
static void update_display(EpubViewer *viewer);

static void prev_page_handler(EpubViewer *viewer);
static void next_page_handler(EpubViewer *viewer);

/**
 * @brief 增量分页Timer回调 - 每次tick预算式算几页
 */
static void paginate_timer_cb(lv_timer_t *timer) {
    EpubViewer *viewer = timer ? (EpubViewer *)timer->user_data : NULL;
    if (!viewer) return;
    
    if (viewer->pagination_complete) {
        lv_timer_del(timer);
        viewer->paginate_timer = NULL;
        VIEW_LOG("[PAGINATE] done, timer stopped\n");
        return;
    }
    
    if (viewer->known_pages >= viewer->max_pages || viewer->paginate_eof) {
        viewer->pagination_complete = true;
        lv_timer_del(timer);
        viewer->paginate_timer = NULL;
        VIEW_LOG("[PAGINATE] reached max_pages or EOF, complete\n");
        return;
    }
    
    // 增量扫描：每次预算 EPUB_PAGINATE_BUDGET 个block
    int budget = EPUB_PAGINATE_BUDGET;
    int prev_known = viewer->known_pages;
    uint32_t prev_offset = viewer->paginate_stream_offset;
    
    int added = scan_pages_from(viewer,
                                viewer->paginate_stream_offset,
                                viewer->known_pages,
                                viewer->paginate_page_y_offset,
                                viewer->paginate_current_level,
                                budget);
    
    VIEW_LOG("[PAGINATE] tick: prev_known=%d now_known=%d added=%d eof=%d\n",
             prev_known, viewer->known_pages, viewer->known_pages - prev_known, viewer->paginate_eof);
    
    // 只有在完全没有前进时才认为异常结束，避免“尚未翻出新页”被误判为完成
    if (viewer->known_pages <= prev_known && !viewer->paginate_eof &&
        viewer->paginate_stream_offset == prev_offset) {
        viewer->pagination_complete = true;
        if (viewer->paginate_timer) {
            lv_timer_del(viewer->paginate_timer);
            viewer->paginate_timer = NULL;
        }
        VIEW_LOG("[PAGINATE] no offset progress, marking complete to avoid dead loop\n");
    }
}

/**
 * @brief 保存分页索引到磁盘（stub实现）
 */
static void save_page_index(EpubViewer *viewer) {
    if (!viewer) return;
    VIEW_LOG("[PIDX] save stub: chapter=%d known=%d total=%d complete=%d\n",
             viewer->current_chapter, viewer->known_pages, viewer->total_pages, viewer->pagination_complete);
}

/**
 * @brief 准备章节流（解码HTML到decoded文件）
 */
static int prepare_chapter_stream(EpubViewer *viewer, int chapter_index) {
    if (!viewer || !viewer->reader) return -1;
    
    free_page_index(viewer);
    
    strncpy(viewer->temp_file_path, "0:/epub_temp.html", sizeof(viewer->temp_file_path) - 1);
    strncpy(viewer->decoded_file_path, "0:/epub_temp.decoded", sizeof(viewer->decoded_file_path) - 1);

    int ret = epub_reader_extract_chapter_to_file(viewer->reader, chapter_index, viewer->temp_file_path);
    if (ret < 0) {
        VIEW_ERR("[PREP] Failed to extract chapter %d\n", chapter_index);
        return -1;
    }
    
    uint32_t uncomp_size = (uint32_t)ret;
    viewer->chapter_uncomp_size = uncomp_size;
    VIEW_LOG("[PREP] chapter %d uncompressed size: %u bytes\n", chapter_index, uncomp_size);

    viewer->use_cache_mode = (uncomp_size <= EPUB_CACHE_THRESHOLD);
    viewer->max_pages = EPUB_MAX_PAGES_PER_CHAPTER;
    
    // 分配页码索引数组
    viewer->page_char_offsets = (uint32_t*)_dma_malloc(viewer->max_pages * sizeof(uint32_t), DMAHEAP_PSRAM);
    viewer->page_start_styles = (uint8_t*)_dma_malloc(viewer->max_pages * sizeof(uint8_t), DMAHEAP_PSRAM);
    if (!viewer->page_char_offsets || !viewer->page_start_styles) {
        VIEW_ERR("[PREP] Failed to alloc page index buffers\n");
        return -1;
    }
    memset(viewer->page_char_offsets, 0, viewer->max_pages * sizeof(uint32_t));
    memset(viewer->page_start_styles, 0, viewer->max_pages * sizeof(uint8_t));

    // 可选分配缓存
    if (viewer->use_cache_mode) {
        uint32_t cache_size = (uncomp_size + 4095) & ~4095UL;
        if (cache_size < 4096) cache_size = 4096;
        if (cache_size > EPUB_CACHE_THRESHOLD + 4096) cache_size = EPUB_CACHE_THRESHOLD + 4096;
        viewer->chapter_decoded_cache = (char*)_dma_malloc(cache_size, DMAHEAP_PSRAM);
        if (!viewer->chapter_decoded_cache) {
            VIEW_ERR("[PREP] Cache alloc failed, falling back to streaming\n");
            viewer->use_cache_mode = false;
        } else {
            viewer->chapter_decoded_cache[0] = '\0';
            VIEW_LOG("[PREP] Using CACHE mode, cache_size=%u\n", cache_size);
        }
    }

    // 解码HTML到decoded文件
    if (build_decoded_stream(viewer) != 0) {
        VIEW_ERR("[PREP] Failed to build decoded stream\n");
        return -1;
    }

    // 初始化渐进式分页状态：先保证第一页可显示，再后台继续分页
    viewer->page_char_offsets[0] = 0;
    viewer->page_start_styles[0] = 0;
    viewer->known_pages = 1;
    viewer->total_pages = 1;
    viewer->paginate_stream_offset = 0;
    viewer->paginate_block_start_offset = 0;
    viewer->paginate_page_y_offset = 0;
    viewer->paginate_current_level = 0;
    viewer->paginate_block_len = 0;
    viewer->paginate_eof = false;
    viewer->pagination_complete = false;

    // 预扫描到“第二页起点”，这样第一页只渲染单页内容而不是整章内容
    scan_pages_from(viewer,
                    viewer->paginate_stream_offset,
                    viewer->known_pages,
                    viewer->paginate_page_y_offset,
                    viewer->paginate_current_level,
                    1);

    VIEW_LOG("[PREP] Chapter stream ready: decoded_len=%u\n", viewer->chapter_decoded_len);
    return 0;
}

/**
 * @brief 从指定位置开始扫描并计算页码边界
 * 
 * @param viewer EpubViewer指针
 * @param start_offset 从decoded文件的哪个字节偏移开始扫描
 * @param start_page 从第几页开始编号（通常是known_pages）
 * @param start_y_offset 起始页内y偏移
 * @param start_level 起始样式级别
 * @param max_pages_to_find 要找多少页，-1表示不受限制直到文件结束
 * @return int 新增的页数
 */
static int scan_pages_from(EpubViewer *viewer, uint32_t start_offset, int start_page,
                           int start_y_offset, int start_level, int max_pages_to_find) {
    if (!viewer || !viewer->page_char_offsets) return 0;
    if (viewer->paginate_eof) return 0;
    if (start_page >= viewer->max_pages) return 0;
    
    VIEW_LOG("[SCAN] start offset=%u page=%d y=%d level=%d max=%d\n",
             start_offset, start_page, start_y_offset, start_level, max_pages_to_find);

    FIL decoded_fp;
    if (f_open(&decoded_fp, viewer->decoded_file_path, FA_READ) != FR_OK) {
        VIEW_ERR("[SCAN] Failed to open decoded file\n");
        return 0;
    }

    if (f_lseek(&decoded_fp, start_offset) != FR_OK) {
        f_close(&decoded_fp);
        VIEW_ERR("[SCAN] Failed to seek to offset %u\n", start_offset);
        return 0;
    }

    int page_count = start_page;
    int page_y_offset = start_y_offset;
    int current_level = start_level;
    lv_font_t *current_font = FONT;
    int current_lh = LH_BODY;
    int block_cap = EPUB_WORK_BUF_SIZE * 2 - 1;
    int block_len = 0;
    uint32_t block_start_offset = start_offset;
    uint32_t stream_offset = start_offset;
    int blocks_processed = 0;
    int pages_found = 0;

    // 根据start_level设置字体
    switch (current_level) {
        case 1: current_font = FONT_H1; current_lh = LH_H1; break;
        case 2: current_font = FONT_H2; current_lh = LH_H2; break;
        case 3: current_font = FONT_H3; current_lh = LH_H3; break;
        default: current_font = FONT; current_lh = LH_BODY; break;
    }

    while (1) {
        // 检查预算
        if (max_pages_to_find > 0 && pages_found >= max_pages_to_find) {
            VIEW_LOG("[SCAN] reached max_pages_to_find=%d, stopping\n", max_pages_to_find);
            break;
        }

        UINT br = 0;
        if (f_read(&decoded_fp, viewer->decoded_buf, EPUB_WORK_BUF_SIZE * 2 - 1, &br) != FR_OK || br == 0) {
            viewer->paginate_eof = true;
            VIEW_LOG("[SCAN] EOF reached at offset=%u\n", stream_offset);
            break;
        }
        viewer->decoded_buf[br] = '\0';

        const char *p = viewer->decoded_buf;
        const char *end = viewer->decoded_buf + br;

        while (p < end && *p) {
            // 处理样式标记
            if ((unsigned char)*p == 0x02 && (p + 2) < end && 
                *(p + 1) >= '0' && *(p + 1) <= '3' && 
                (unsigned char)*(p + 2) == 0x03) {
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

            // 处理换行符
            if (*p == '\n') {
                if (block_len > 0) {
                    viewer->reflowed_buf[block_len] = '\0';
                    memcpy(viewer->page_text_buf, viewer->reflowed_buf, block_len + 1);
                    strip_style_markers(viewer->page_text_buf);
                    
                    blocks_processed++;
                    lv_point_t txt_size;
                    lv_txt_get_size(&txt_size, viewer->page_text_buf, current_font, 0, 0, 
                                   CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
                    int actual_h = txt_size.y;
                    if (actual_h < current_lh) actual_h = current_lh;

                    if (heading_need_next_page(viewer, current_level, page_y_offset, actual_h) &&
                        page_count < viewer->max_pages) {
                        viewer->page_char_offsets[page_count] = block_start_offset;
                        viewer->page_start_styles[page_count] = (uint8_t)current_level;
                        page_count++;
                        pages_found++;
                        page_y_offset = 0;
                        VIEW_LOG("[SCAN] move heading to next page at offset=%u, now page=%d\n",
                                 block_start_offset, page_count);
                        if (max_pages_to_find > 0 && pages_found >= max_pages_to_find) {
                            goto scan_finish;
                        }
                    }

                    // 检查是否需要换页
                    if (page_y_offset > 0 && page_y_offset + actual_h > CONTENT_HEIGHT && 
                        page_count < viewer->max_pages) {
                        viewer->page_char_offsets[page_count] = block_start_offset;
                        viewer->page_start_styles[page_count] = (uint8_t)current_level;
                        page_count++;
                        pages_found++;
                        page_y_offset = 0;
                        VIEW_LOG("[SCAN] new page at offset=%u, now page=%d\n", 
                                block_start_offset, page_count);
                        if (max_pages_to_find > 0 && pages_found >= max_pages_to_find) {
                            goto scan_finish;
                        }
                    }

                    // 处理超长段落分割
                    if (actual_h > CONTENT_HEIGHT && page_count < viewer->max_pages) {
                        uint32_t s_len = strlen(viewer->page_text_buf);
                        uint32_t s_off = 0;

                        while (actual_h > CONTENT_HEIGHT && s_off < s_len && 
                               page_count < viewer->max_pages) {
                            uint32_t fit = calc_bytes_for_height(
                                viewer->page_text_buf + s_off, s_len - s_off,
                                current_font, current_lh, CONTENT_HEIGHT);
                            if (fit == 0) break;
                            s_off += fit;

                            // 映射到raw偏移
                            int ri = 0, si = 0;
                            while (ri < block_len && si < (int)s_off) {
                                if ((unsigned char)viewer->reflowed_buf[ri] == 0x02 &&
                                    ri + 2 < block_len &&
                                    (unsigned char)viewer->reflowed_buf[ri + 2] == 0x03) {
                                    ri += 3;
                                } else {
                                    ri++;
                                    si++;
                                }
                            }

                            viewer->page_char_offsets[page_count] = block_start_offset + ri;
                            viewer->page_start_styles[page_count] = (uint8_t)current_level;
                            page_count++;
                            pages_found++;
                            if (max_pages_to_find > 0 && pages_found >= max_pages_to_find) {
                                stream_offset = block_start_offset + ri;
                                goto scan_finish;
                            }

                            lv_txt_get_size(&txt_size, viewer->page_text_buf + s_off,
                                          current_font, 0, 0, CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
                            actual_h = txt_size.y;
                            if (actual_h < current_lh) actual_h = current_lh;
                        }
                        page_y_offset = actual_h;
                    } else {
                        page_y_offset += actual_h;
                    }
                    
                    block_len = 0;
                }
                p++;
                stream_offset++;
                block_start_offset = stream_offset;
                continue;
            }

            // 处理普通字符
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

    // 处理最后一个block
    if (block_len > 0) {
        viewer->reflowed_buf[block_len] = '\0';
        memcpy(viewer->page_text_buf, viewer->reflowed_buf, block_len + 1);
        strip_style_markers(viewer->page_text_buf);
        
        blocks_processed++;
        lv_point_t txt_size;
        lv_txt_get_size(&txt_size, viewer->page_text_buf, current_font, 0, 0,
                       CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
        int actual_h = txt_size.y;
        if (actual_h < current_lh) actual_h = current_lh;

        if (heading_need_next_page(viewer, current_level, page_y_offset, actual_h) &&
            page_count < viewer->max_pages) {
            viewer->page_char_offsets[page_count] = block_start_offset;
            viewer->page_start_styles[page_count] = (uint8_t)current_level;
            page_count++;
            pages_found++;
            page_y_offset = 0;
        }

        if (page_y_offset > 0 && page_y_offset + actual_h > CONTENT_HEIGHT &&
            page_count < viewer->max_pages) {
            viewer->page_char_offsets[page_count] = block_start_offset;
            viewer->page_start_styles[page_count] = (uint8_t)current_level;
            page_count++;
            pages_found++;
            page_y_offset = 0;
        }
    }

scan_finish:
    f_close(&decoded_fp);

    // 更新viewer状态
    int added = page_count - start_page;
    viewer->known_pages = page_count;
    viewer->total_pages = page_count;
    viewer->paginate_stream_offset = stream_offset;
    viewer->paginate_page_y_offset = page_y_offset;
    viewer->paginate_current_level = current_level;
    viewer->paginate_block_len = block_len;
    viewer->paginate_block_start_offset = block_start_offset;

    VIEW_LOG("[SCAN] done: added=%d pages, now known=%d/%d, next_offset=%u, eof=%d\n",
             added, viewer->known_pages, viewer->total_pages, 
             viewer->paginate_stream_offset, viewer->paginate_eof);

    return added;
}

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

static void sanitize_utf8(char *str) {
    int len = strlen(str);
    if (len == 0) return;
    unsigned char *s = (unsigned char *)str;
    int i = 0;
    int write = 0;
    while (i < len) {
        int char_len = 1;
        if (s[i] < 0x80) {
            char_len = 1;
        } else if ((s[i] & 0xE0) == 0xC0) {
            char_len = 2;
        } else if ((s[i] & 0xF0) == 0xE0) {
            char_len = 3;
        } else if ((s[i] & 0xF8) == 0xF0) {
            char_len = 4;
        } else {
            i++;
            continue;
        }
        if (i + char_len > len) {
            break;
        }
        int valid = 1;
        for (int j = 1; j < char_len; j++) {
            if ((s[i + j] & 0xC0) != 0x80) {
                valid = 0;
                break;
            }
        }
        if (!valid) {
            i++;
            continue;
        }
        if (write != i) {
            for (int j = 0; j < char_len; j++) s[write + j] = s[i + j];
        }
        write += char_len;
        i += char_len;
    }
    str[write] = '\0';
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
        } else if ((read_ptr[0] & 0xE0) == 0xC0 &&
                   (read_ptr[1] & 0xC0) == 0x80) {
            unicode = ((read_ptr[0] & 0x1F) << 6) | (read_ptr[1] & 0x3F); char_len = 2;
        } else if ((read_ptr[0] & 0xF0) == 0xE0 &&
                   (read_ptr[1] & 0xC0) == 0x80 &&
                   (read_ptr[2] & 0xC0) == 0x80) {
            unicode = ((read_ptr[0] & 0x0F) << 12) | ((read_ptr[1] & 0x3F) << 6) | (read_ptr[2] & 0x3F); char_len = 3;
        } else if ((read_ptr[0] & 0xF8) == 0xF0 &&
                   (read_ptr[1] & 0xC0) == 0x80 &&
                   (read_ptr[2] & 0xC0) == 0x80 &&
                   (read_ptr[3] & 0xC0) == 0x80) {
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
    int in_pre = 0;
    int in_head = 0;
    int in_style = 0;
    int in_script = 0;
    const char *tag_start = NULL;
    int current_level = 0;
    int pending_para_indent = 0;
    const char *p = html;
    const char *end = html + html_len;

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
        } else if (*p == '>' && in_tag) {
            in_tag = 0;
            int closing = (tag_start && (tag_start + 1) < end && tag_start[1] == '/');
            const char *name = closing ? tag_start + 2 : tag_start + 1;

            if (!closing) {
                if (strncasecmp(name, "head", 4) == 0) in_head = 1;
                else if (strncasecmp(name, "style", 5) == 0) in_style = 1;
                else if (strncasecmp(name, "script", 6) == 0) in_script = 1;
                else if (strncasecmp(name, "pre", 3) == 0 || strncasecmp(name, "code", 4) == 0) in_pre = 1;
            } else {
                if (strncasecmp(name, "head", 4) == 0) in_head = 0;
                else if (strncasecmp(name, "style", 5) == 0) in_style = 0;
                else if (strncasecmp(name, "script", 6) == 0) in_script = 0;
                else if (strncasecmp(name, "pre", 3) == 0 || strncasecmp(name, "code", 4) == 0) in_pre = 0;
            }

            if (!(in_head || in_style || in_script)) {
                int len = (int)(p - tag_start + 1);
                if (len > 2 && tag_start[1] == '/') {
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
                    if ((tag_start + 2) < end && (tag_start[1] == 'h' || tag_start[1] == 'H') &&
                        (tag_start[2] >= '1' && tag_start[2] <= '6')) {
                        int level = tag_start[2] - '0';
                        if (current_level != level) {
                            if (out_pos > 0 && output[out_pos - 1] != '\n') {
                                if (out_pos < out_size - 1) output[out_pos++] = '\n';
                            }
                            int dbg_len = (int)(end - tag_start);
                            if (dbg_len > 20) dbg_len = 20;
                            if (dbg_len < 0) dbg_len = 0;
                            VIEW_LOG("[HTML] H%d tag: %.*s\n", level, dbg_len, tag_start);
                            EMIT_LEVEL(level);
                            current_level = level;
                        }
                    } else if (strncasecmp(tag_start + 1, "p", 1) == 0 ||
                               strncasecmp(tag_start + 1, "div", 3) == 0 ||
                               strncasecmp(tag_start + 1, "br", 2) == 0) {
                        if (out_pos > 0 && output[out_pos - 1] != '\n') {
                            if (out_pos < out_size - 1) output[out_pos++] = '\n';
                        }
                        if (strncasecmp(tag_start + 1, "p", 1) == 0) {
                            pending_para_indent = 1;
                        }
                    }
                }
            }
        } else if (!in_tag) {
            if (in_head || in_style || in_script) {
                p++;
                continue;
            }
            if (*p == '&') {
                if (out_pos < out_size - 1) output[out_pos++] = *p;
            } else if (*p == '\n') {
                if (!in_pre) {
                    if (out_pos > 0 && output[out_pos - 1] != '\n') {
                        if (out_pos < out_size - 1) output[out_pos++] = '\n';
                    }
                } else {
                    if (out_pos < out_size - 1) output[out_pos++] = '\n';
                }
            } else if (*p != '\r' && *p != '\t') {
                if (pending_para_indent && *p != ' ') {
                    /* 4个ASCII空格作为段落缩进（全字体兼容） */
                    if (out_pos < out_size - 4) {
                        output[out_pos++] = ' ';
                        output[out_pos++] = ' ';
                        output[out_pos++] = ' ';
                        output[out_pos++] = ' ';
                    }
                    pending_para_indent = 0;
                }
                if (out_pos < out_size - 1) output[out_pos++] = *p;
            }
        }
        p++;
    }

    #undef EMIT_LEVEL

    if (out_pos > 0 && output[out_pos - 1] != '\n' && out_pos < out_size - 1) {
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
            } else if (p[1] == '#' && ((p[2] >= '0' && p[2] <= '9') || p[2] == 'x' || p[2] == 'X')) {
                int is_hex = (p[2] == 'x' || p[2] == 'X');
                p += is_hex ? 3 : 2;
                uint32_t code = 0;
                while (*p) {
                    if (!is_hex && *p >= '0' && *p <= '9') {
                        code = code * 10 + (*p - '0');
                    } else if (is_hex && *p >= '0' && *p <= '9') {
                        code = (code << 4) | (*p - '0');
                    } else if (is_hex && *p >= 'a' && *p <= 'f') {
                        code = (code << 4) | (*p - 'a' + 10);
                    } else if (is_hex && *p >= 'A' && *p <= 'F') {
                        code = (code << 4) | (*p - 'A' + 10);
                    } else {
                        break;
                    }
                    p++;
                }
                if (*p == ';') p++;
                if (code > 0 && code <= 0x10FFFF) {
                    if (code < 0x80) {
                        output[out_pos++] = (char)code;
                    } else if (code < 0x800 && out_pos < out_size - 2) {
                        output[out_pos++] = (char)(0xC0 | (code >> 6));
                        output[out_pos++] = (char)(0x80 | (code & 0x3F));
                    } else if (code < 0x10000 && out_pos < out_size - 3) {
                        output[out_pos++] = (char)(0xE0 | (code >> 12));
                        output[out_pos++] = (char)(0x80 | ((code >> 6) & 0x3F));
                        output[out_pos++] = (char)(0x80 | (code & 0x3F));
                    } else if (out_pos < out_size - 4) {
                        output[out_pos++] = (char)(0xF0 | (code >> 18));
                        output[out_pos++] = (char)(0x80 | ((code >> 12) & 0x3F));
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
    if (viewer->paginate_timer) {
        lv_timer_del(viewer->paginate_timer);
        viewer->paginate_timer = NULL;
    }
    viewer->max_pages = 0;
    viewer->total_pages = 0;
    viewer->known_pages = 0;
    viewer->pagination_complete = false;
    viewer->pagination_dirty = false;
    viewer->pagination_loaded_from_disk = false;
    viewer->paginate_stream_offset = 0;
    viewer->paginate_block_start_offset = 0;
    viewer->paginate_page_y_offset = 0;
    viewer->paginate_current_level = 0;
    viewer->paginate_block_len = 0;
    viewer->paginate_eof = false;
    viewer->use_cache_mode = false;
    viewer->chapter_decoded_len = 0;
    viewer->decoded_file_path[0] = '\0';
    viewer->index_file_path[0] = '\0';
}

/* 计算指定高度需要多少 UTF-8 安全字节 */
static uint32_t calc_bytes_for_height(const char *text, uint32_t text_len,
                                      lv_font_t *font, int line_height,
                                      int target_height) {
    if (!text || text_len == 0 || target_height <= 0) return 0;

    uint32_t low = 0;
    uint32_t high = text_len;
    uint32_t best = 0;
    char work_buf[2048];

    if (high > sizeof(work_buf) - 1) high = sizeof(work_buf) - 1;

    VIEW_LOG("[CBH] enter text_len=%u high=%u tgt=%d\n", text_len, high, target_height);
    int cbh_iter = 0;

    while (low <= high) {
        uint32_t mid = low + ((high - low) / 2);
        while (mid > 0 && (((unsigned char)text[mid]) & 0xC0) == 0x80) {
            mid--;
        }
        if (mid == 0) mid = 1;
        if (mid >= sizeof(work_buf)) mid = sizeof(work_buf) - 1;
        if (mid < low) mid = low;

        memcpy(work_buf, text, mid);
        work_buf[mid] = '\0';

        lv_point_t txt_size;
        lv_txt_get_size(&txt_size, work_buf, font, 0, 0, CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
        int actual_h = txt_size.y;
        if (actual_h < line_height) actual_h = line_height;

        cbh_iter++;
        if (cbh_iter <= 3 || (cbh_iter % 5) == 0) {
            VIEW_LOG("[CBH] iter=%d low=%u mid=%u high=%u h=%d\n",
                     cbh_iter, low, mid, high, actual_h);
        }

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
    VIEW_LOG("[CBH] exit best=%u iters=%d\n", best, cbh_iter);
    return best;
}

static int flush_render_block(EpubViewer *viewer, const char *block_start, int block_len,
                              lv_font_t *font, int line_height, int *y_offset) {
    if (!viewer || !block_start || block_len <= 0 || !font || !y_offset) return 0;

    while (block_len > 0 && (block_start[block_len - 1] == '\n' || block_start[block_len - 1] == ' ')) {
        block_len--;
    }
    if (block_len <= 0) return 0;

    int max_copy = EPUB_WORK_BUF_SIZE * 2 - 1;
    if (block_len > max_copy) block_len = max_copy;
    memcpy(viewer->reflowed_buf, block_start, block_len);
    viewer->reflowed_buf[block_len] = '\0';

    lv_point_t txt_size;
    lv_txt_get_size(&txt_size, viewer->reflowed_buf, font, 0, 0, CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
    int actual_h = txt_size.y;
    if (actual_h < line_height) actual_h = line_height;

    int remaining = CONTENT_HEIGHT - *y_offset;
    if (remaining < line_height) return 0;

    if (actual_h <= remaining) {
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

    memcpy(viewer->page_text_buf, block_start, block_len);
    viewer->page_text_buf[block_len] = '\0';
    strip_style_markers(viewer->page_text_buf);
    uint32_t stripped_len = strlen(viewer->page_text_buf);

    uint32_t fit_bytes = calc_bytes_for_height(viewer->page_text_buf, stripped_len, font, line_height, remaining);
    if (fit_bytes == 0) return 0;

    int raw_pos = 0;
    int stripped_count = 0;
    while (raw_pos < block_len && stripped_count < (int)fit_bytes) {
        if ((unsigned char)viewer->reflowed_buf[raw_pos] == 0x02 &&
            raw_pos + 2 < block_len &&
            (unsigned char)viewer->reflowed_buf[raw_pos + 2] == 0x03) {
            raw_pos += 3;
        } else {
            raw_pos++;
            stripped_count++;
        }
    }

    viewer->reflowed_buf[raw_pos] = '\0';
    lv_txt_get_size(&txt_size, viewer->reflowed_buf, font, 0, 0, CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
    int trunc_h = txt_size.y;
    if (trunc_h < line_height) trunc_h = line_height;

    lv_obj_t *line_label = lv_label_create(viewer->content_container);
    lv_obj_align(line_label, LV_ALIGN_TOP_LEFT, 0, *y_offset);
    lv_obj_set_size(line_label, CONTENT_WIDTH, trunc_h);
    lv_obj_set_style_text_font(line_label, font, 0);
    lv_obj_set_style_text_color(line_label, lv_color_black(), 0);
    lv_obj_set_style_pad_top(line_label, 0, 0);
    lv_label_set_long_mode(line_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(line_label, viewer->reflowed_buf);
    *y_offset += trunc_h;
    return trunc_h;
}

static int heading_need_next_page(EpubViewer *viewer, int current_level,
                                  int page_y_offset, int actual_h) {
    if (!viewer || current_level <= 0) return 0;
    if (page_y_offset <= 0) return 0;
    int reserve_h = actual_h + (LH_BODY * 2);
    return (page_y_offset + reserve_h > CONTENT_HEIGHT);
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
    VIEW_LOG("[DECODE] build_decoded_stream ENTRY (streaming)\n");
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

    FSIZE_t file_size = f_size(&html_fp);
    VIEW_LOG("[DECODE] file_size=%lu\n", (unsigned long)file_size);

    char *carry_buf = (char *)_dma_malloc(EPUB_WORK_BUF_SIZE * 2, DMAHEAP_PSRAM);
    if (!carry_buf) {
        VIEW_ERR("[DECODE] carry alloc failed\n");
        f_close(&html_fp);
        f_close(&decoded_fp);
        return -1;
    }

    uint32_t carry_len = 0;
    uint32_t total_decoded_len = 0;
    char *cache_ptr = viewer->chapter_decoded_cache;
    uint32_t cache_left = viewer->chapter_uncomp_size * 2 + 4096;
    bool cache_active = (viewer->use_cache_mode && cache_ptr != NULL);
    carry_buf[0] = '\0';

    {
        UINT total_scanned = 0;
        bool head_found = false;
        while (total_scanned < file_size) {
            UINT brr = 0;
            fr = f_read(&html_fp, viewer->html_buf, EPUB_WORK_BUF_SIZE, &brr);
            if (fr != FR_OK || brr == 0) break;
            viewer->html_buf[brr] = '\0';

            const char *head_end = NULL;
            const char *p1 = strstr(viewer->html_buf, "</head>");
            const char *p2 = strstr(viewer->html_buf, "</HEAD>");
            if (p1 && (!p2 || p1 < p2)) head_end = p1;
            else if (p2) head_end = p2;

            if (head_end) {
                const char *after_head = head_end + 7;
                const char *body_tag = strstr(after_head, "<body");
                if (body_tag) {
                    const char *body_close = strchr(body_tag, '>');
                    if (body_close) after_head = body_close + 1;
                }
                uint32_t skip_to = total_scanned + (uint32_t)(after_head - viewer->html_buf);
                f_lseek(&html_fp, skip_to);
                head_found = true;
                VIEW_LOG("[DECODE] skipped head, body_offset=%lu\n", (unsigned long)skip_to);
                break;
            }
            total_scanned += brr;
        }
        if (!head_found) {
            f_lseek(&html_fp, 0);
            VIEW_LOG("[DECODE] no <head> tag, processing from start\n");
        }
    }

    while (1) {
        UINT br = 0;
        fr = f_read(&html_fp, viewer->html_buf, EPUB_WORK_BUF_SIZE, &br);
        if (fr != FR_OK || br == 0) break;
        viewer->html_buf[br] = '\0';

        uint32_t work_len = carry_len;
        if (carry_len > 0) {
            memcpy(viewer->stripped_buf, carry_buf, carry_len);
        }
        memcpy(viewer->stripped_buf + carry_len, viewer->html_buf, br);
        work_len += br;
        viewer->stripped_buf[work_len] = '\0';

        uint32_t safe_end = work_len;

        {
            int scan_start = (int)work_len - 512;
            int last_lt = -1;
            int last_gt = -1;
            if (scan_start < 0) scan_start = 0;
            for (int j = scan_start; j < (int)work_len; j++) {
                if ((unsigned char)viewer->stripped_buf[j] == '<') last_lt = j;
                else if ((unsigned char)viewer->stripped_buf[j] == '>') last_gt = j;
            }
            if (last_lt >= 0 && last_lt > last_gt) {
                safe_end = (uint32_t)last_lt;
            }
        }

        if (safe_end > 0 && safe_end < work_len) {
            int amp_pos = -1;
            int semi_found = 0;
            int j;
            for (j = (int)safe_end - 1; j >= 0 && j > (int)safe_end - 16; j--) {
                if (viewer->stripped_buf[j] == ';') { semi_found = 1; break; }
                if (viewer->stripped_buf[j] == '&') { amp_pos = j; break; }
            }
            if (amp_pos >= 0 && !semi_found) {
                safe_end = (uint32_t)amp_pos;
            }
        }

        if (safe_end > 0 && safe_end < work_len) {
            int check_start = (int)safe_end - 4;
            if (check_start < 0) check_start = 0;
            for (int k = (int)safe_end - 1; k >= check_start; k--) {
                unsigned char c = (unsigned char)viewer->stripped_buf[k];
                if (c < 0x80) continue;
                if ((c & 0xC0) == 0x80) continue;
                int expected = 1;
                if ((c & 0xF8) == 0xF0) expected = 4;
                else if ((c & 0xF0) == 0xE0) expected = 3;
                else if ((c & 0xE0) == 0xC0) expected = 2;
                if (k + expected > (int)safe_end) {
                    safe_end = (uint32_t)k;
                    break;
                }
            }
        }

        if (safe_end > 0 && safe_end < work_len) {
            int i = 0;
            while (i < (int)safe_end) {
                unsigned char c = (unsigned char)viewer->stripped_buf[i];
                if (c < 0x80) { i++; continue; }
                int expected = 1;
                if ((c & 0xF8) == 0xF0) expected = 4;
                else if ((c & 0xF0) == 0xE0) expected = 3;
                else if ((c & 0xE0) == 0xC0) expected = 2;
                else { i++; continue; }
                if (i + expected > (int)safe_end) {
                    safe_end = (uint32_t)i;
                    break;
                }
                int valid = 1;
                for (int j = 1; j < expected; j++) {
                    if (((unsigned char)viewer->stripped_buf[i + j] & 0xC0) != 0x80) {
                        valid = 0;
                        break;
                    }
                }
                if (!valid) {
                    safe_end = (uint32_t)i;
                    break;
                }
                i += expected;
            }
        }

        if (safe_end > 0) {
            viewer->stripped_buf[safe_end] = '\0';

            int slen = strip_html_tags_with_styles(
                viewer->stripped_buf, (int)safe_end,
                viewer->decoded_buf, EPUB_WORK_BUF_SIZE * 2);

            if (slen > 0) {
                decode_html_entities(viewer->decoded_buf,
                                     viewer->reflowed_buf, EPUB_WORK_BUF_SIZE * 2);
                sanitize_utf8(viewer->reflowed_buf);
                fix_chinese_punctuation(viewer->reflowed_buf);
                filter_unsupported_chars_ex(viewer->reflowed_buf, true);

                uint32_t flen = strlen(viewer->reflowed_buf);
                if (flen > 0) {
                    UINT bw = 0;
                    fr = f_write(&decoded_fp, viewer->reflowed_buf, flen, &bw);
                    if (fr != FR_OK || bw != flen) {
                        VIEW_ERR("[DECODE] f_write failed fr=%d\n", fr);
                        _dma_free(carry_buf, 0);
                        f_close(&html_fp);
                        f_close(&decoded_fp);
                        return -1;
                    }
                    if (cache_active && cache_left > flen + 1) {
                        memcpy(cache_ptr, viewer->reflowed_buf, flen);
                        cache_ptr += flen;
                        cache_left -= flen;
                    }
                    total_decoded_len += flen;
                }
            }
        }

        carry_len = work_len - safe_end;
        if (carry_len > 0) {
            if (carry_len >= (EPUB_WORK_BUF_SIZE * 2 - 1)) carry_len = EPUB_WORK_BUF_SIZE * 2 - 1;
            memcpy(carry_buf, viewer->stripped_buf + safe_end, carry_len);
        }
        carry_buf[carry_len] = '\0';
    }

    if (carry_len > 0) {
        carry_buf[carry_len] = '\0';
        int slen = strip_html_tags_with_styles(
            carry_buf, (int)carry_len,
            viewer->decoded_buf, EPUB_WORK_BUF_SIZE * 2);
        if (slen > 0) {
            decode_html_entities(viewer->decoded_buf,
                                 viewer->reflowed_buf, EPUB_WORK_BUF_SIZE * 2);
            sanitize_utf8(viewer->reflowed_buf);
            fix_chinese_punctuation(viewer->reflowed_buf);
            filter_unsupported_chars_ex(viewer->reflowed_buf, true);

            uint32_t flen = strlen(viewer->reflowed_buf);
            if (flen > 0) {
                UINT bw = 0;
                fr = f_write(&decoded_fp, viewer->reflowed_buf, flen, &bw);
                if (fr == FR_OK && bw == flen) {
                    if (cache_active && cache_left > flen + 1) {
                        memcpy(cache_ptr, viewer->reflowed_buf, flen);
                    }
                    total_decoded_len += flen;
                }
            }
        }
    }

    if (cache_active && cache_ptr) {
        *cache_ptr = '\0';
    }

    _dma_free(carry_buf, 0);
    f_close(&html_fp);
    f_close(&decoded_fp);

    viewer->chapter_decoded_len = total_decoded_len;
    VIEW_LOG("[DECODE] DONE streaming total_decoded_len=%u\n", total_decoded_len);
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
                VIEW_LOG("[IDX] style L%d font=%p lh=%d blk=%d ofs=%u\n",
                         current_level, current_font, current_lh, block_len,
                         (unsigned)stream_offset);
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
                    VIEW_LOG("[IDX] \\n block_len=%d page_y=%d page=%d\n",
                             block_len, page_y_offset, page_count);
                    VIEW_LOG("[IDX] measuring block (lv_txt_get_size)...\n");
                    lv_point_t txt_size;
                    lv_txt_get_size(&txt_size, viewer->page_text_buf, current_font, 0, 0, CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
                    int actual_h = txt_size.y;
                    if (actual_h < current_lh) actual_h = current_lh;
                    VIEW_LOG("[IDX] measure done: h=%d > CONTENT_HEIGHT=%d?\n", actual_h, CONTENT_HEIGHT);

                    if (page_y_offset > 0 && page_y_offset + actual_h > CONTENT_HEIGHT && page_count < viewer->max_pages) {
                        viewer->page_char_offsets[page_count] = block_start_offset;
                        viewer->page_start_styles[page_count] = (uint8_t)current_level;
                        page_count++;
                        page_y_offset = 0;
                        VIEW_LOG("[IDX] new page (overflow), now page=%d\n", page_count);
                    }

                    if (actual_h > CONTENT_HEIGHT && page_count < viewer->max_pages) {
                        uint32_t s_len = strlen(viewer->page_text_buf);
                        uint32_t s_off = 0;
                        int split_iter = 0;

                        VIEW_LOG("[IDX] SPLIT start s_len=%u\n", s_len);
                        while (actual_h > CONTENT_HEIGHT && s_off < s_len && page_count < viewer->max_pages) {
                            split_iter++;
                            VIEW_LOG("[IDX] split iter=%d s_off=%u s_len=%u\n", split_iter, s_off, s_len);
                            uint32_t fit = calc_bytes_for_height(
                                viewer->page_text_buf + s_off, s_len - s_off,
                                current_font, current_lh, CONTENT_HEIGHT);
                            if (fit == 0) break;
                            s_off += fit;
                            if (s_off >= s_len) { actual_h = 0; break; }

                            int ri = 0, si = 0;
                            while (ri < block_len && si < (int)s_off) {
                                if ((unsigned char)viewer->reflowed_buf[ri] == 0x02 &&
                                    ri + 2 < block_len &&
                                    (unsigned char)viewer->reflowed_buf[ri + 2] == 0x03) {
                                    ri += 3;
                                } else {
                                    ri++;
                                    si++;
                                }
                            }

                            viewer->page_char_offsets[page_count] = block_start_offset + ri;
                            viewer->page_start_styles[page_count] = (uint8_t)current_level;
                            page_count++;
                            VIEW_LOG("[IDX] split page=%d raw_off=%u\n", page_count, block_start_offset + ri);

                            lv_txt_get_size(&txt_size, viewer->page_text_buf + s_off,
                                            current_font, 0, 0, CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
                            actual_h = txt_size.y;
                            if (actual_h < current_lh) actual_h = current_lh;
                        }
                        VIEW_LOG("[IDX] SPLIT done iters=%d\n", split_iter);
                        page_y_offset = actual_h;
                    } else {
                        page_y_offset += actual_h;
                    }
                    block_len = 0;
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
        VIEW_LOG("[IDX] FINAL block_len=%d page_y=%d page=%d\n",
                 block_len, page_y_offset, page_count);
        VIEW_LOG("[IDX] measuring final block...\n");
        lv_point_t txt_size;
        lv_txt_get_size(&txt_size, viewer->page_text_buf, current_font, 0, 0, CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
        int actual_h = txt_size.y;
        if (actual_h < current_lh) actual_h = current_lh;
        VIEW_LOG("[IDX] final measure done: h=%d\n", actual_h);

        if (page_y_offset > 0 && page_y_offset + actual_h > CONTENT_HEIGHT && page_count < viewer->max_pages) {
            viewer->page_char_offsets[page_count] = block_start_offset;
            viewer->page_start_styles[page_count] = (uint8_t)current_level;
            page_count++;
            page_y_offset = 0;
            VIEW_LOG("[IDX] final new page, now page=%d\n", page_count);
        }

        if (actual_h > CONTENT_HEIGHT && page_count < viewer->max_pages) {
            uint32_t s_len = strlen(viewer->page_text_buf);
            uint32_t s_off = 0;
            int split_iter = 0;

            VIEW_LOG("[IDX] FINAL SPLIT start s_len=%u\n", s_len);
            while (actual_h > CONTENT_HEIGHT && s_off < s_len && page_count < viewer->max_pages) {
                split_iter++;
                VIEW_LOG("[IDX] final split iter=%d s_off=%u\n", split_iter, s_off);
                uint32_t fit = calc_bytes_for_height(
                    viewer->page_text_buf + s_off, s_len - s_off,
                    current_font, current_lh, CONTENT_HEIGHT);
                if (fit == 0) break;
                s_off += fit;
                if (s_off >= s_len) { actual_h = 0; break; }

                int ri = 0, si = 0;
                while (ri < block_len && si < (int)s_off) {
                    if ((unsigned char)viewer->reflowed_buf[ri] == 0x02 &&
                        ri + 2 < block_len &&
                        (unsigned char)viewer->reflowed_buf[ri + 2] == 0x03) {
                        ri += 3;
                    } else {
                        ri++;
                        si++;
                    }
                }

                viewer->page_char_offsets[page_count] = block_start_offset + ri;
                viewer->page_start_styles[page_count] = (uint8_t)current_level;
                page_count++;

                lv_txt_get_size(&txt_size, viewer->page_text_buf + s_off,
                                current_font, 0, 0, CONTENT_WIDTH, LV_LABEL_LONG_WRAP);
                actual_h = txt_size.y;
                if (actual_h < current_lh) actual_h = current_lh;
            }
            VIEW_LOG("[IDX] FINAL SPLIT done iters=%d\n", split_iter);
        }
    }

    f_close(&decoded_fp);
    viewer->total_pages = page_count;

    {
        int dump_pages = page_count < 10 ? page_count : 10;
        VIEW_LOG("[IDX] First %d page offsets:\n", dump_pages);
        for (int i = 0; i < dump_pages; i++) {
            VIEW_LOG("[IDX] page[%d] offset=%u style=%d\n", i,
                     viewer->page_char_offsets[i], viewer->page_start_styles[i]);
        }
    }

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

    {
        int dump_len = (int)copy_len;
        if (dump_len > 200) dump_len = 200;
        VIEW_LOG("[PAGE_TEXT] len=%u first200:'%.*s'\n", copy_len, dump_len, viewer->decoded_buf);
        int newline_count = 0;
        int style_marker_count = 0;
        for (uint32_t i = 0; i < copy_len; i++) {
            if (viewer->decoded_buf[i] == '\n') newline_count++;
            if ((unsigned char)viewer->decoded_buf[i] == 0x02) style_marker_count++;
        }
        VIEW_LOG("[PAGE_TEXT] newlines=%d style_markers=%d level=%d\n", newline_count, style_marker_count, current_level);
        int hex_len = (int)copy_len;
        if (hex_len > 40) hex_len = 40;
        VIEW_LOG("[PAGE_HEX_FIRST] ");
        for (int hi = 0; hi < hex_len; hi++) VIEW_LOG("%02X ", (unsigned char)viewer->decoded_buf[hi]);
        VIEW_LOG("\n");
        int tail_start = (int)copy_len - 40;
        if (tail_start < 0) tail_start = 0;
        int tail_len = (int)copy_len - tail_start;
        VIEW_LOG("[PAGE_HEX_LAST] ");
        for (int hi = 0; hi < tail_len; hi++) VIEW_LOG("%02X ", (unsigned char)viewer->decoded_buf[tail_start + hi]);
        VIEW_LOG("\n");
    }

    lv_obj_clean(viewer->content_container);

    const char *p = viewer->decoded_buf;
    const char *block_start = p;
    int y_offset = 0;
    int render_block_count = 0;
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
            int blen = (int)(p - block_start);
            if (blen > 0) {
                render_block_count++;
                int dlen = blen > 60 ? 60 : blen;
                VIEW_LOG("[RENDER_BLK #%d] len=%d y=%d '%.*s'\n", render_block_count, blen, y_offset, dlen, block_start);
            }
            flush_render_block(viewer, block_start, blen, current_font, current_lh, &y_offset);
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
            int blen = (int)(p - block_start);
            if (blen > 0) {
                render_block_count++;
                int dlen = blen > 60 ? 60 : blen;
                VIEW_LOG("[RENDER_BLK #%d] len=%d y=%d '%.*s'\n", render_block_count, blen, y_offset, dlen, block_start);
            }
            flush_render_block(viewer, block_start, blen, current_font, current_lh, &y_offset);
            p++;
            block_start = p;
            continue;
        }
        p++;
    }
    {
        int blen = (int)(p - block_start);
        if (blen > 0) {
            render_block_count++;
            int dlen = blen > 60 ? 60 : blen;
            VIEW_LOG("[RENDER_BLK #%d FINAL] len=%d y=%d '%.*s'\n", render_block_count, blen, y_offset, dlen, block_start);
        }
        flush_render_block(viewer, block_start, blen, current_font, current_lh, &y_offset);
    }
    VIEW_LOG("[PAGE_SUMMARY] blocks=%d final_y=%d\n", render_block_count, y_offset);
    {
        int empty_lines = (CONTENT_HEIGHT - y_offset) / current_lh;
        int tail_start = (int)copy_len - 40;
        if (tail_start < 0) tail_start = 0;
        int tail_len = (int)copy_len - tail_start;
        int tdl = tail_len > 40 ? 40 : tail_len;
        VIEW_LOG("[PAGE_TAIL] empty_lines=%d last40:'%.*s'\n", empty_lines, tdl, viewer->decoded_buf + tail_start);
        VIEW_LOG("[PAGE_TAIL_HEX] ");
        for (int hi = 0; hi < tdl; hi++) VIEW_LOG("%02X ", (unsigned char)viewer->decoded_buf[tail_start + hi]);
        VIEW_LOG("\n");
    }

    char page_str[64];
    if (viewer->pagination_complete && viewer->total_pages > 0) {
        snprintf(page_str, sizeof(page_str), "%d/%d", viewer->current_page + 1, viewer->total_pages);
    } else {
        snprintf(page_str, sizeof(page_str), "%d/?", viewer->current_page + 1);
    }
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
    if (viewer->current_page < viewer->known_pages - 1) {
        viewer->current_page++;
        update_display(viewer);
    } else if (!viewer->pagination_complete) {
        VIEW_LOG("[PAGINATE] next page requested but not indexed yet: current=%d known=%d\n",
                 viewer->current_page + 1, viewer->known_pages);
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
    viewer->total_pages = 0;
    viewer->known_pages = 0;
    viewer->pagination_complete = false;
    viewer->pagination_dirty = false;
    viewer->pagination_loaded_from_disk = false;

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
    lv_label_set_text(viewer->page_label, "1/?");
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
    viewer->pagination_complete = false;
    viewer->pagination_dirty = false;
    viewer->known_pages = 0;

    VIEW_LOG("Goto chapter %d...\n", chapter_index);

    if (prepare_chapter_stream(viewer, chapter_index) != 0) {
        VIEW_ERR("Failed to build page index for chapter %d\n", chapter_index);
        lv_obj_t *err = lv_label_create(viewer->content_container);
        lv_obj_align(err, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_text_font(err, FONT, 0);
        lv_label_set_text(err, "章节加载失败");
        viewer->total_pages = 0;
        viewer->known_pages = 0;
        viewer->current_page = 0;
        return false;
    }

    update_display(viewer);
    if (!viewer->paginate_timer) {
        viewer->paginate_timer = lv_timer_create(paginate_timer_cb, 500, viewer);
    }
    VIEW_LOG("Chapter %d ready: known=%d total=%d complete=%d\n",
             chapter_index, viewer->known_pages, viewer->total_pages, viewer->pagination_complete);
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
