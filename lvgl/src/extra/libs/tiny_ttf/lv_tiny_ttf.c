#include "lv_tiny_ttf.h"

#if LV_USE_TINY_TTF
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../../../misc/lv_lru.h"
#include "sys/sys_heap.h"
#include <sys/dma_heap.h>

extern void * psram_malloc(size_t size);
extern void psram_free(void * ptr);
extern size_t psram_GetFreeHeapSize(void);

#define STB_RECT_PACK_IMPLEMENTATION
#define STBRP_STATIC
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_HEAP_FACTOR_SIZE_32 50
#define STBTT_HEAP_FACTOR_SIZE_128 20
#define STBTT_HEAP_FACTOR_SIZE_DEFAULT 10
#define STBTT_malloc(x, u) ((void)(u), psram_malloc(x))
#define STBTT_free(x, u) ((void)(u), psram_free(x))
#define TTF_MALLOC(x) (psram_malloc(x))
#define TTF_FREE(x) (psram_free(x))

static void * ttf_scratch_alloc(size_t size, int * is_dma)
{
    void * p = psram_malloc(size);
    if(p) {
        if(is_dma) *is_dma = 0;
        return p;
    }
    p = _dma_malloc(size, DMAHEAP_PSRAM);
    if(p && is_dma) *is_dma = 1;
    return p;
}

static void ttf_scratch_free(void * p, int is_dma, size_t size)
{
    if(!p) return;
    if(is_dma) _dma_free(p, DMAHEAP_PSRAM);
    else psram_free(p);
    (void)size;
}

/* 持久字体元数据：优先 DMA heap，失败再 psram_heap（glyf 大块仍只用 psram） */
static void * ttf_meta_alloc(size_t size, int * is_dma)
{
    void * p = _dma_malloc(size, DMAHEAP_PSRAM);
    if(p) {
        if(is_dma) *is_dma = 1;
        return p;
    }
    p = psram_malloc(size);
    if(p && is_dma) *is_dma = 0;
    return p;
}

static void ttf_meta_free(void * p, int is_dma)
{
    if(!p) return;
    if(is_dma) _dma_free(p, DMAHEAP_PSRAM);
    else psram_free(p);
}

#define CJK_METRICS_START 0x4E00u
#define CJK_METRICS_END   0x9FFFu
#define CJK_METRICS_COUNT (CJK_METRICS_END - CJK_METRICS_START + 1u)

#define ASCII_METRICS_START 0x20u
#define ASCII_METRICS_END   0x7Eu
#define ASCII_METRICS_COUNT (ASCII_METRICS_END - ASCII_METRICS_START + 1u)  /* 95 */

#define LEVEL1_GLYPH_CACHE_SIZE 3600  /* 3500 CJK + 95 ASCII + 5 reserve */

/* glyf 预加载保留的 psram 余量（防碎片/后续小分配） */
#define TTF_GLYF_PSRAM_RESERVE   (48 * 1024)
/* head/hhea/os2/glyf_lookup 等零碎常驻开销 */
#define TTF_SMALL_TABLES_SLACK   (16 * 1024)
/* 全量 L1 尝试后留给 psram_heap 的余量（loca 等已 alloc 后不再重复扣 pinned） */
#define TTF_GLYF_POST_RESERVE    (96 * 1024)

/* Phase1: glyf 批量读 — 跨小间隙合并 seek，减少 SD 随机读次数
 * 实测 12KB/64KB: ops=163 physical=5.9MB 5.4s（空洞过多）
 * 调参目标：降低 physical，ops 控制在 200~400 */
#define GLYF_MERGE_GAP_MAX       (8U * 1024U)
#define GLYF_READ_RUN_MAX        (128U * 1024U)
#define GLYF_IO_BUFFER_SIZE      (128U * 1024U)

/* .l1glyf offline compact glyf cache (see tools/build_l1glyf_cache.py) */
#define L1GLYF_MAGIC             0x3146474Cu  /* "LGF1" */
#define L1GLYF_VERSION           1U
#define L1GLYF_HEADER_CORE_SIZE  32U
#define L1GLYF_HEADER_TOTAL_SIZE (L1GLYF_HEADER_CORE_SIZE + 4U)

typedef struct {
    uint32_t unicode;
    uint16_t glyph_index;
    uint16_t pad;
    uint32_t compact_offset;
    uint32_t glyf_size;
} l1glyf_entry_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t ttf_size;
    uint32_t ttf_crc32;
    uint32_t lookup_count;
    uint32_t glyf_data_size;
    uint32_t lookup_offset;
    uint32_t data_offset;
} l1glyf_header_t;

static int g_metrics_cache_is_dma = 0;
static int g_ascii_metrics_cache_is_dma = 0;
static int g_glyf_lookup_is_dma = 0;
static int g_level1_glyphs_is_dma = 0;

/* ===== Verbose debug logging switches (set to 0 to suppress, 1 to enable) =====
 * These macros control per-character verbose output during page rendering.
 * Disabling them saves ~7 seconds of serial output time per page turn.
 * ALL logging code is preserved — only the printf calls are compiled out.
 * Summary-level logs (BM_RENDER summary, DSC_STATS, EPD_RENDER, etc.) remain active.
 */
#define TTF_DEBUG_BITMAP_VERBOSE   0  /* FB_AREA ASCII art, GLYPH_HEX, GLYPH_BITMAP per char */
#define TTF_DEBUG_CACHE_HIT_VERBOSE 0 /* FB_CACHE_HIT + full ASCII art on cache hit */
#define TTF_DEBUG_METRICS_VERBOSE  0  /* CJK_METRICS per-char output in layout phase */
#define TTF_DEBUG_MISMATCH_VERBOSE 0  /* MISMATCH debug between dsc cache and actual render */
/* ============================================================================= */

typedef struct {
    uint16_t adv_w_raw;   // unscaled advance width from hmtx
    uint16_t box_w_raw;   // unscaled bbox width (xMax - xMin + 1)
    uint16_t box_h_raw;   // unscaled bbox height (yMax - yMin + 1)
    int16_t  ofs_x_raw;   // unscaled bbox x offset (xMin)
    int16_t  ofs_y_raw;   // stbtt bitmap box bottom at scale 1.0, negated (-iy1)
    uint16_t glyph_index; // glyph index in font
    uint8_t  valid;
} ttf_metrics_entry_t;

typedef struct ttf_font_desc ttf_font_desc_t;

#if LV_TINY_TTF_FILE_SUPPORT
typedef struct ttf_cb_stream {
    lv_fs_file_t * file;
    const void * data;
    size_t size;
    size_t position;
    ttf_font_desc_t * dsc;
} ttf_cb_stream_t;

static int ttf_stream_read_from_cache(ttf_font_desc_t *dsc, size_t pos, void *out, size_t to_read);

static void ttf_cb_stream_read(ttf_cb_stream_t * stream, void * data, size_t to_read)
{
    if(stream->file != NULL) {
        if(ttf_stream_read_from_cache(stream->dsc, stream->position, data, to_read)) {
            stream->position += to_read;
            return;
        }
        /* Cache miss: now seek the SD card to the correct position before reading */
        lv_fs_seek(stream->file, stream->position, LV_FS_SEEK_SET);
        uint32_t br;
        lv_fs_read(stream->file, data, to_read, &br);
        stream->position += br;
    }
    else {
        if(to_read + stream->position >= stream->size) {
            to_read = stream->size - stream->position;
        }
        lv_memcpy(data, ((const unsigned char *)stream->data + stream->position), to_read);
        stream->position += to_read;
    }
}
static void ttf_cb_stream_seek(ttf_cb_stream_t * stream, size_t position)
{
    if(stream->file != NULL) {
        /* Lazy seek: only update logical position, don't touch SD card.
         * Actual lv_fs_seek is deferred to cache-miss in ttf_cb_stream_read.
         * This avoids wasting 3-5ms per seek when data is in PSRAM cache. */
        stream->position = position;
    }
    else {
        if(position > stream->size) {
            stream->position = stream->size;
        }
        else {
            stream->position = position;
        }
    }
}

#define STBTT_STREAM_TYPE ttf_cb_stream_t *
#define STBTT_STREAM_SEEK(s, x) ttf_cb_stream_seek(s, x);
#define STBTT_STREAM_READ(s, x, y) ttf_cb_stream_read(s, x, y);
#endif

#include "stb_rect_pack.h"
#include "stb_truetype_htcw.h"

typedef struct {
    uint8_t *data;
    uint32_t file_offset;
    uint32_t size;
} ttf_cached_table_t;

typedef struct {
    ttf_cached_table_t cmap;
    ttf_cached_table_t loca;
    ttf_cached_table_t hmtx;
    ttf_cached_table_t glyf;
    ttf_cached_table_t head;
    ttf_cached_table_t hhea;
    ttf_cached_table_t os2;
    uint8_t active;
} ttf_table_cache_t;

typedef struct {
    uint16_t unicode;
    uint16_t glyph_index;
    uint32_t compact_offset;   // 在紧凑glyf缓存中的偏移
    uint32_t file_glyf_offset; // 在原始TTF文件glyf表中的偏移（相对于glyf表起始）
    uint16_t glyf_size;
    uint8_t cached;
} level1_glyph_info_t;

// glyf cache查找条目（按文件偏移排序，用于二分查找）
typedef struct {
    uint32_t glyf_rel_offset;  // glyph在glyf表内的相对偏移（来自loca）
    uint32_t compact_offset;   // 对应compact缓冲区中的偏移
    uint16_t glyf_size;        // 该glyph的数据大小
    uint32_t read_span;        // compact 中可连续读取的字节数（含文件 相邻 glyph）
} glyf_cache_entry_t;

typedef struct ttf_font_desc {
    lv_fs_file_t file;
    char file_path[256];
#if LV_TINY_TTF_FILE_SUPPORT
    ttf_cb_stream_t stream;
#else
    const uint8_t * stream;
#endif
    stbtt_fontinfo info;
    float scale;
    int ascent;
    int descent;
    ttf_metrics_entry_t * metrics_cache;
    ttf_metrics_entry_t * ascii_metrics_cache;  /* ASCII 0x20-0x7E metrics */
    level1_glyph_info_t * level1_glyphs;
    uint8_t * level1_glyf_data;
    uint32_t level1_glyf_total_size;
    uint16_t level1_glyph_count;
    uint8_t level1_loaded;
    ttf_table_cache_t table_cache;
} ttf_font_desc_t;

// 全局共享的 Level1 缓存（解决多个字体实例重复加载问题）
static uint8_t *g_shared_level1_glyf_data = NULL;
static uint32_t g_shared_level1_glyf_size = 0;
static level1_glyph_info_t *g_shared_level1_glyphs = NULL;
static uint16_t g_shared_level1_glyph_count = 0;
static ttf_table_cache_t g_shared_table_cache;
static int g_shared_level1_loaded = 0;
static ttf_metrics_entry_t *g_shared_metrics_cache = NULL;
static ttf_metrics_entry_t *g_shared_ascii_metrics_cache = NULL;

// 全局glyf查找表（二分查找，将文件偏移翻译为compact缓冲区偏移）
static glyf_cache_entry_t *g_glyf_lookup = NULL;     // 排序后的查找表
static uint16_t g_glyf_lookup_count = 0;              // 查找表条目数
static uint32_t g_glyf_table_file_offset = 0;         // glyf表在文件中的绝对偏移
static uint32_t g_glyf_table_size = 0;                // glyf表总大小

static int compare_cache_entries(const void *a, const void *b)
{
    const glyf_cache_entry_t *ea = (const glyf_cache_entry_t *)a;
    const glyf_cache_entry_t *eb = (const glyf_cache_entry_t *)b;
    if(ea->glyf_rel_offset < eb->glyf_rel_offset) return -1;
    if(ea->glyf_rel_offset > eb->glyf_rel_offset) return 1;
    return 0;
}

/* 相邻 file 偏移的 glyph 在 compact 缓冲里连续存放；扩展 read_span 供 stbtt 大块读 */
static void ttf_glyf_lookup_fill_read_spans(void)
{
    for(uint16_t i = 0; i < g_glyf_lookup_count; i++) {
        glyf_cache_entry_t *e = &g_glyf_lookup[i];
        e->read_span = e->glyf_size;
        for(uint16_t j = i + 1; j < g_glyf_lookup_count; j++) {
            glyf_cache_entry_t *n = &g_glyf_lookup[j];
            uint32_t cur_end = e->glyf_rel_offset + e->read_span;
            if(n->glyf_rel_offset != cur_end) break;
            e->read_span += n->glyf_size;
        }
    }
}

/* Comparison for sorting level1_glyphs by glyph_index (for binary search) */
static int compare_level1_by_glyph_index(const void *a, const void *b)
{
    const level1_glyph_info_t *ga = (const level1_glyph_info_t *)a;
    const level1_glyph_info_t *gb = (const level1_glyph_info_t *)b;
    if(ga->glyph_index < gb->glyph_index) return -1;
    if(ga->glyph_index > gb->glyph_index) return 1;
    return 0;
}

static int ttf_ensure_metrics_cache(ttf_font_desc_t * dsc)
{
    if(!dsc) return -1;
    if(g_shared_metrics_cache) {
        dsc->metrics_cache = g_shared_metrics_cache;
    } else if(!dsc->metrics_cache) {
        int is_dma = 0;
        size_t sz = CJK_METRICS_COUNT * sizeof(ttf_metrics_entry_t);
        dsc->metrics_cache = (ttf_metrics_entry_t *)ttf_meta_alloc(sz, &is_dma);
        if(!dsc->metrics_cache) return -1;
        g_metrics_cache_is_dma = is_dma;
        memset(dsc->metrics_cache, 0, sz);
        printf("[TTF] metrics_cache %lu bytes on %s\n",
               (unsigned long)sz, is_dma ? "DMA" : "psram");
    }
    if(g_shared_ascii_metrics_cache) {
        dsc->ascii_metrics_cache = g_shared_ascii_metrics_cache;
    } else if(!dsc->ascii_metrics_cache) {
        int is_dma = 0;
        size_t sz = ASCII_METRICS_COUNT * sizeof(ttf_metrics_entry_t);
        dsc->ascii_metrics_cache = (ttf_metrics_entry_t *)ttf_meta_alloc(sz, &is_dma);
        if(!dsc->ascii_metrics_cache) return -1;
        g_ascii_metrics_cache_is_dma = is_dma;
        memset(dsc->ascii_metrics_cache, 0, sz);
    }
    return 0;
}

static void ttf_free_level1_glyphs(level1_glyph_info_t * glyphs)
{
    if(!glyphs) return;
    if(g_level1_glyphs_is_dma) _dma_free(glyphs, DMAHEAP_PSRAM);
    else psram_free(glyphs);
}

static void ttf_free_cached_table_slot(ttf_cached_table_t * slot)
{
    if(!slot || !slot->data) return;
    psram_free(slot->data);
    slot->data = NULL;
    slot->size = 0;
    slot->file_offset = 0;
}

static void ttf_free_table_cache(ttf_table_cache_t * tc)
{
    if(!tc) return;
    ttf_free_cached_table_slot(&tc->cmap);
    ttf_free_cached_table_slot(&tc->loca);
    ttf_free_cached_table_slot(&tc->hmtx);
    ttf_free_cached_table_slot(&tc->glyf);
    ttf_free_cached_table_slot(&tc->head);
    ttf_free_cached_table_slot(&tc->hhea);
    ttf_free_cached_table_slot(&tc->os2);
    tc->active = 0;
}

/* 全量 L1：free 已反映 loca 等已分配对象，只留 post reserve */
static uint32_t ttf_glyf_psram_budget_full(void)
{
    size_t free_bytes = psram_GetFreeHeapSize();
    if(free_bytes <= TTF_GLYF_POST_RESERVE) return 0;
    return (uint32_t)(free_bytes - TTF_GLYF_POST_RESERVE);
}

/* 部分 L1 fallback：仍为尚未分配的 hmtx/cmap 预留，并保留 85% 安全系数 */
static uint32_t ttf_glyf_psram_budget_partial(uint32_t pending_pin_bytes)
{
    size_t free_bytes = psram_GetFreeHeapSize();
    uint32_t reserve = TTF_GLYF_PSRAM_RESERVE + pending_pin_bytes + TTF_SMALL_TABLES_SLACK;
    if(free_bytes <= reserve) return 0;
    return (uint32_t)((free_bytes - reserve) * 85 / 100);
}

/* 小表常驻 PSRAM：宁可裁 glyf 字数，也不释放 loca/hmtx/cmap */
static int ttf_persist_table_cache(ttf_cached_table_t * slot, const uint8_t * src,
                                   uint32_t file_offset, uint32_t size, const char * name)
{
    if(!slot || !src || size == 0) return -1;
    if(slot->data) {
        psram_free(slot->data);
        slot->data = NULL;
    }
    slot->data = (uint8_t *)psram_malloc(size);
    if(!slot->data) {
        printf("[TTF] ERROR: %s psram cache alloc failed (%lu bytes)\n", name, (unsigned long)size);
        slot->file_offset = file_offset;
        slot->size = size;
        return -1;
    }
    lv_memcpy(slot->data, src, size);
    slot->file_offset = file_offset;
    slot->size = size;
    return 0;
}

/* glyf 预算裁剪优先级：数字/符号必须尽量留在 L1，避免 150ms+ SD 读 */
static int ttf_glyph_load_priority(uint32_t u)
{
    if((u >= '0' && u <= '9') || (u >= 0xFF10 && u <= 0xFF19)) return 0;
    if(u >= 0x3000 && u <= 0x303F) return 1;
    if(u >= 0xFF01 && u <= 0xFF0F) return 1;
    if(u == 0x2014 || u == 0x2018 || u == 0x2019 || u == 0x201C || u == 0x201D || u == 0x2026 || u == 0x00B7)
        return 1;
    switch(u) {
        case '.': case ',': case ';': case ':': case '!': case '?':
        case '"': case '\'': case '(': case ')': case '[': case ']':
        case '-': case '_': case '+': case '=': case '*': case '/':
        case '\\': case '@': case '#': case '$': case '%': case '&':
        case '<': case '>': case '{': case '}': case '|': case '~':
        case '`': case '^': case ' ':
            return 1;
        default:
            break;
    }
    if(u >= 0x20 && u <= 0x7E) return 2;
    return 3;
}

static int compare_glyph_load_priority(const void * a, const void * b)
{
    const level1_glyph_info_t * ga = (const level1_glyph_info_t *)a;
    const level1_glyph_info_t * gb = (const level1_glyph_info_t *)b;
    int pa = ttf_glyph_load_priority(ga->unicode);
    int pb = ttf_glyph_load_priority(gb->unicode);
    if(pa != pb) return pa - pb;
    if(ga->unicode < gb->unicode) return -1;
    if(ga->unicode > gb->unicode) return 1;
    return 0;
}

static void ttf_sort_glyphs_for_budget(level1_glyph_info_t * glyphs, uint16_t count)
{
    qsort(glyphs, count, sizeof(level1_glyph_info_t), compare_glyph_load_priority);
}

static uint16_t ttf_trim_glyphs_to_budget(level1_glyph_info_t * glyphs, uint16_t count,
                                          uint32_t * inout_total, uint16_t * inout_valid,
                                          uint32_t pending_pin_bytes)
{
    uint32_t budget = ttf_glyf_psram_budget_partial(pending_pin_bytes);
    uint32_t total = *inout_total;
    uint16_t valid = *inout_valid;

    if(total <= budget) return valid;

    printf("[TTF] glyf need %lu bytes > partial budget %lu (pending_pin=%lu), trimming by priority...\n",
           (unsigned long)total, (unsigned long)budget, (unsigned long)pending_pin_bytes);

    total = 0;
    valid = 0;
    for(uint16_t i = 0; i < count; i++) {
        if(!glyphs[i].cached) continue;
        if(total + glyphs[i].glyf_size > budget) {
            glyphs[i].cached = 0;
            continue;
        }
        total += glyphs[i].glyf_size;
        valid++;
    }

    *inout_total = total;
    *inout_valid = valid;
    printf("[TTF] glyf budget trim: loaded %u glyphs, %lu bytes (budget %lu)\n",
           (unsigned)valid, (unsigned long)total, (unsigned long)budget);
    return valid;
}

static uint16_t ttf_count_glyf_cached(const level1_glyph_info_t * glyphs, uint16_t count)
{
    uint16_t n = 0;
    for(uint16_t i = 0; i < count; i++) {
        if(glyphs[i].cached) n++;
    }
    return n;
}

static int ttf_stream_read_from_cache(ttf_font_desc_t *dsc, size_t pos, void *out, size_t to_read)
{
    // 统计（仅用于偶发性调试，不在热路径上打印）
    static int cache_hit = 0, cache_miss = 0;
    
    // === 路径1: 检查当前字体的表缓存（cmap/loca/hmtx/head/hhea/os2）===
    // 这些是高频命中表，优先检查
    ttf_table_cache_t *tc = (dsc && dsc->table_cache.active) ? &dsc->table_cache : 
                            (g_shared_level1_loaded ? &g_shared_table_cache : NULL);
    if(tc) {
        // 依次检查各表，按命中频率排序：hmtx > cmap > loca > head > hhea > os2
        ttf_cached_table_t *tables[] = {&tc->hmtx, &tc->cmap, &tc->loca, &tc->head, &tc->hhea, &tc->os2, NULL};
        for(int i = 0; tables[i] != NULL; i++) {
            ttf_cached_table_t *t = tables[i];
            if(t->data && pos >= t->file_offset && (pos + to_read) <= (t->file_offset + t->size)) {
                lv_memcpy(out, t->data + (pos - t->file_offset), to_read);
                cache_hit++;
                return 1;
            }
        }
    }
    
    // === 路径2: glyf compact cache（通过二分查找翻译文件偏移到compact偏移）===
    if(g_glyf_lookup && g_shared_level1_glyf_data) {
        // 快速范围检查：pos是否在glyf表范围内
        if(pos >= g_glyf_table_file_offset && 
           pos < g_glyf_table_file_offset + g_glyf_table_size) {
            uint32_t rel_pos = pos - g_glyf_table_file_offset;
            int lo = 0, hi = g_glyf_lookup_count - 1;
            while(lo <= hi) {
                int mid = (lo + hi) / 2;
                glyf_cache_entry_t *e = &g_glyf_lookup[mid];
                if(rel_pos < e->glyf_rel_offset) {
                    hi = mid - 1;
                } else if(rel_pos >= e->glyf_rel_offset + e->glyf_size) {
                    lo = mid + 1;
                } else {
                    uint32_t in_glyph_off = rel_pos - e->glyf_rel_offset;
                    uint32_t span = e->read_span ? e->read_span : e->glyf_size;
                    if(in_glyph_off < span) {
                        uint32_t avail = span - in_glyph_off;
                        size_t serve = (to_read <= avail) ? to_read : avail;
                        lv_memcpy(out, g_shared_level1_glyf_data + e->compact_offset + in_glyph_off, serve);
                        if(serve < to_read) {
                            lv_memset((uint8_t *)out + serve, 0, to_read - serve);
                        }
                        cache_hit++;
                        return 1;
                    }
                    break;
                }
            }
            // glyf范围内但不在level1缓存中，直接返回miss（不再尝试其他表）
            cache_miss++;
            return 0;
        }
    }
    
    
    cache_miss++;
    // 仅每65536次miss打印一次统计（大幅减少打印开销）
    if((cache_miss & 0xFFFF) == 0) {
        printf("[CACHE_STAT] hits=%d misses=%d (hit_rate=%d%%)\n", 
               cache_hit, cache_miss, cache_hit * 100 / (cache_hit + cache_miss));
    }
    return 0;
}

static int find_table_location(lv_fs_file_t *file, uint32_t font_start, const char *table_name, uint32_t *out_offset, uint32_t *out_length)
{
    uint16_t num_tables;
    uint32_t header_pos = font_start;
    lv_fs_seek(file, header_pos, LV_FS_SEEK_SET);
    
    uint32_t br;
    uint8_t buf[12];
    
    // 读取TTF头部 (12 bytes)
    lv_fs_read(file, buf, 12, &br);
    
    // 检查签名 (bytes 0-3)
    uint32_t signature = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
    if(signature != 0x00010000 && signature != 0x4F54544F) {
        printf("[TTF] Invalid TTF signature\n");
        return 0;
    }
    
    // numTables (bytes 4-5, big-endian uint16)
    num_tables = (buf[4] << 8) | buf[5];
    
    if(num_tables > 100) {
        printf("[TTF] Too many tables, invalid TTF\n");
        return 0;
    }
    
    // 遍历表目录 (每个条目16 bytes)
    for(int i = 0; i < num_tables; i++) {
        uint8_t entry[16];
        lv_fs_read(file, entry, 16, &br);
        
        // 表名 (bytes 0-3)
        char name[5] = {entry[0], entry[1], entry[2], entry[3], 0};
        
        // 偏移 (bytes 8-11, big-endian uint32)
        uint32_t offset = (entry[8] << 24) | (entry[9] << 16) | (entry[10] << 8) | entry[11];
        
        // 长度 (bytes 12-15, big-endian uint32)
        uint32_t length = (entry[12] << 24) | (entry[13] << 16) | (entry[14] << 8) | entry[15];
        
        if(strcmp(name, table_name) == 0) {
            *out_offset = offset + font_start;
            *out_length = length;
            return 1;
        }
    }
    return 0;
}

// 从内存TTF数据中查找表偏移
static int find_table_location_in_memory(const uint8_t *ttf_data, uint32_t font_start, const char *table_name, uint32_t *out_offset, uint32_t *out_length)
{
    uint16_t num_tables = (ttf_data[font_start + 4] << 8) | ttf_data[font_start + 5];
    if(num_tables > 100) return 0;
    for(int i = 0; i < num_tables; i++) {
        uint32_t entry_offs = font_start + 12 + i * 16;
        char name[5] = {ttf_data[entry_offs], ttf_data[entry_offs + 1], ttf_data[entry_offs + 2], ttf_data[entry_offs + 3], 0};
        uint32_t offset = (ttf_data[entry_offs + 8] << 24) | (ttf_data[entry_offs + 9] << 16) | (ttf_data[entry_offs + 10] << 8) | ttf_data[entry_offs + 11];
        uint32_t length = (ttf_data[entry_offs + 12] << 24) | (ttf_data[entry_offs + 13] << 16) | (ttf_data[entry_offs + 14] << 8) | ttf_data[entry_offs + 15];
        if(strcmp(name, table_name) == 0) {
            *out_offset = offset + font_start;
            *out_length = length;
            return 1;
        }
    }
    return 0;
}

// 从内存TTF数据中读取表到PSRAM
static uint8_t *read_table_from_memory(const uint8_t *ttf_data, uint32_t font_start, const char *table_name, uint32_t *out_size)
{
    uint32_t offset, length;
    if(!find_table_location_in_memory(ttf_data, font_start, table_name, &offset, &length)) return NULL;
    uint8_t *data = psram_malloc(length);
    if(!data) return NULL;
    lv_memcpy(data, ttf_data + offset, length);
    *out_size = length;
    return data;
}

static int ttf_find_table(ttf_font_desc_t *dsc, const char *table_name, uint32_t *out_offset, uint32_t *out_length)
{
    const uint8_t *ttf_mem = (dsc->stream.data != NULL) ? (const uint8_t *)dsc->stream.data : NULL;
    if(ttf_mem) {
        return find_table_location_in_memory(ttf_mem, dsc->info.fontstart, table_name, out_offset, out_length);
    }
    if(dsc->file_path[0] != '\0') {
        return find_table_location(&dsc->file, dsc->info.fontstart, table_name, out_offset, out_length);
    }
    return 0;
}

static int ttf_font_has_cff_outlines(ttf_font_desc_t *dsc)
{
    uint32_t off, len;
    if(ttf_find_table(dsc, "CFF ", &off, &len)) return 1;
    if(ttf_find_table(dsc, "CFF2", &off, &len)) return 1;
    return 0;
}

static void ttf_list_tables(ttf_font_desc_t *dsc)
{
    const uint8_t *ttf_mem = (dsc->stream.data != NULL) ? (const uint8_t *)dsc->stream.data : NULL;
    uint32_t font_start = dsc->info.fontstart;
    uint16_t num_tables;
    if(ttf_mem) {
        num_tables = (ttf_mem[font_start + 4] << 8) | ttf_mem[font_start + 5];
        printf("[TTF_DBG] tables in memory (count=%u):\n", (unsigned)num_tables);
        for(int i = 0; i < num_tables && i < 32; i++) {
            uint32_t entry_offs = font_start + 12 + (uint32_t)i * 16;
            printf("  %c%c%c%c\n", ttf_mem[entry_offs], ttf_mem[entry_offs + 1],
                   ttf_mem[entry_offs + 2], ttf_mem[entry_offs + 3]);
        }
        return;
    }
    if(dsc->file_path[0] == '\0') return;
    lv_fs_seek(&dsc->file, font_start, LV_FS_SEEK_SET);
    uint8_t buf[12];
    uint32_t br;
    lv_fs_read(&dsc->file, buf, 12, &br);
    num_tables = (buf[4] << 8) | buf[5];
    printf("[TTF_DBG] tables via dsc->file (count=%u, br=%u):\n", (unsigned)num_tables, (unsigned)br);
    for(int i = 0; i < num_tables && i < 32; i++) {
        uint8_t entry[16];
        lv_fs_read(&dsc->file, entry, 16, &br);
        printf("  %c%c%c%c\n", entry[0], entry[1], entry[2], entry[3]);
    }
}

static uint8_t * ttf_read_table_alloc(ttf_font_desc_t *dsc, const char *table_name,
                                      uint32_t *out_file_offset, uint32_t *out_size)
{
    const uint8_t *ttf_mem = (dsc->stream.data != NULL) ? (const uint8_t *)dsc->stream.data : NULL;
    if(ttf_mem) {
        uint32_t sz = 0;
        uint8_t *data = read_table_from_memory(ttf_mem, dsc->info.fontstart, table_name, &sz);
        if(data) {
            if(out_size) *out_size = sz;
            if(out_file_offset) ttf_find_table(dsc, table_name, out_file_offset, &sz);
        }
        return data;
    }
    uint32_t offset, length;
    if(!ttf_find_table(dsc, table_name, &offset, &length)) return NULL;
    if(out_file_offset) *out_file_offset = offset;
    if(out_size) *out_size = length;
    uint8_t *data = psram_malloc(length);
    if(!data) return NULL;
    lv_fs_seek(&dsc->file, offset, LV_FS_SEEK_SET);
    uint32_t bytes_read = 0;
    while(bytes_read < length) {
        uint32_t to_read = length - bytes_read;
        if(to_read > 4096) to_read = 4096;
        uint32_t br;
        lv_fs_read(&dsc->file, data + bytes_read, to_read, &br);
        if(br == 0) break;
        bytes_read += br;
    }
    if(bytes_read < length) {
        psram_free(data);
        return NULL;
    }
    return data;
}

// 读取指定表到PSRAM
static uint8_t *read_table_to_psram(lv_fs_file_t *file, uint32_t font_start, const char *table_name, uint32_t *out_size)
{
    uint32_t offset, length;
    if(!find_table_location(file, font_start, table_name, &offset, &length)) {
        printf("[TTF] Table '%s' not found\n", table_name);
        return NULL;
    }
    
    printf("[TTF] Reading table '%s': offset=%lu, size=%lu\n", table_name, offset, length);
    
    uint8_t *data = psram_malloc(length);
    if(data == NULL) {
        printf("[TTF] Failed to allocate %lu bytes for table '%s'\n", length, table_name);
        return NULL;
    }
    
    if(LV_FS_RES_OK != lv_fs_seek(file, offset, LV_FS_SEEK_SET)) {
        psram_free(data);
        return NULL;
    }
    
    uint32_t bytes_read = 0;
    uint8_t *ptr = data;
    while(bytes_read < length) {
        uint32_t to_read = length - bytes_read;
        if(to_read > 4096) to_read = 4096;
        uint32_t br;
        if(LV_FS_RES_OK != lv_fs_read(file, ptr + bytes_read, to_read, &br)) {
            psram_free(data);
            return NULL;
        }
        if(br == 0) break;
        bytes_read += br;
    }
    
    *out_size = bytes_read;
    return data;
}

/* 独立打开 TTF 读表，避免 dsc->file 在长 batch 读后被 seek 污染 */
static uint8_t * ttf_read_table_via_path(const char * path, uint32_t font_start, const char * table_name,
                                         uint32_t * out_file_offset, uint32_t * out_size)
{
    lv_fs_file_t f;
    if(!path || path[0] == '\0') return NULL;
    if(LV_FS_RES_OK != lv_fs_open(&f, path, LV_FS_MODE_RD)) {
        printf("[TTF] Failed to open '%s' for table '%s'\n", path, table_name);
        return NULL;
    }
    uint32_t sz = 0;
    uint8_t * data = read_table_to_psram(&f, font_start, table_name, &sz);
    if(data) {
        if(out_size) *out_size = sz;
        if(out_file_offset) {
            uint32_t off, len;
            if(find_table_location(&f, font_start, table_name, &off, &len)) {
                *out_file_offset = off;
            }
        }
    }
    lv_fs_close(&f);
    return data;
}

// 解析cmap表，查找Unicode对应的glyph index
static uint16_t lookup_glyph_in_cmap_format4(uint8_t *cmap_data, uint32_t unicode)
{
    // cmap format 4 解析（大端序）
    uint16_t seg_count_x2 = (cmap_data[6] << 8) | cmap_data[7];
    uint16_t seg_count = seg_count_x2 / 2;
    
    // 各数组的起始位置（都是大端序uint16）
    uint8_t *end_code_ptr = cmap_data + 14;       // 偏移14
    uint8_t *reserved_pad = end_code_ptr + seg_count * 2;  // 2字节保留
    uint8_t *start_code_ptr = reserved_pad + 2;   // 偏移16 + seg_count*2
    uint8_t *id_delta_ptr = start_code_ptr + seg_count * 2;  // 偏移16 + seg_count*4
    uint8_t *id_range_offset_ptr = id_delta_ptr + seg_count * 2;  // 偏移16 + seg_count*6
    
    for(int i = 0; i < seg_count; i++) {
        // 手动解析大端序uint16
        uint16_t end = (end_code_ptr[i * 2] << 8) | end_code_ptr[i * 2 + 1];
        uint16_t start = (start_code_ptr[i * 2] << 8) | start_code_ptr[i * 2 + 1];
        int16_t delta = (int16_t)((id_delta_ptr[i * 2] << 8) | id_delta_ptr[i * 2 + 1]);
        uint16_t range_offset = (id_range_offset_ptr[i * 2] << 8) | id_range_offset_ptr[i * 2 + 1];
        
        if(unicode >= start && unicode <= end) {
            if(range_offset == 0) {
                // 使用delta计算glyph index
                return (uint16_t)((int32_t)unicode + delta);
            } else {
                // 使用range offset计算glyph index
                uint8_t *glyph_array_ptr = id_range_offset_ptr + i * 2 + range_offset;
                uint16_t glyph_index = (glyph_array_ptr[(unicode - start) * 2] << 8) | glyph_array_ptr[(unicode - start) * 2 + 1];
                if(glyph_index != 0) {
                    glyph_index = (uint16_t)((int32_t)glyph_index + delta);
                }
                return glyph_index;
            }
        }
    }
    
    return 0;  // 未找到
}

static uint16_t lookup_glyph_in_cmap_format12(uint8_t *cmap_data, uint32_t unicode)
{
    // cmap format 12 解析（大端序）- 使用二分查找，groups按start_char排序
    uint32_t num_groups = ((uint32_t)cmap_data[12] << 24) | ((uint32_t)cmap_data[13] << 16) |
                          ((uint32_t)cmap_data[14] << 8) | cmap_data[15];
    
    uint8_t *groups_base = cmap_data + 16;
    int lo = 0, hi = (int)num_groups - 1;
    
    while(lo <= hi) {
        int mid = (lo + hi) / 2;
        uint8_t *g = groups_base + mid * 12;
        uint32_t start_char = ((uint32_t)g[0] << 24) | ((uint32_t)g[1] << 16) |
                             ((uint32_t)g[2] << 8) | g[3];
        uint32_t end_char = ((uint32_t)g[4] << 24) | ((uint32_t)g[5] << 16) |
                           ((uint32_t)g[6] << 8) | g[7];
        
        if(unicode < start_char) {
            hi = mid - 1;
        } else if(unicode > end_char) {
            lo = mid + 1;
        } else {
            uint32_t start_glyph = ((uint32_t)g[8] << 24) | ((uint32_t)g[9] << 16) |
                                  ((uint32_t)g[10] << 8) | g[11];
            return (uint16_t)(start_glyph + (unicode - start_char));
        }
    }
    
    return 0;  // 未找到
}

static uint16_t lookup_glyph_in_cmap(uint8_t *cmap_data, uint32_t unicode)
{
    uint16_t num_tables = (cmap_data[2] << 8) | cmap_data[3];
    
    uint8_t *encoding_records = cmap_data + 4;
    for(int i = 0; i < num_tables; i++) {
        uint16_t platform_id = (encoding_records[i * 8] << 8) | encoding_records[i * 8 + 1];
        uint16_t encoding_id = (encoding_records[i * 8 + 2] << 8) | encoding_records[i * 8 + 3];
        uint32_t subtable_offset = ((uint32_t)encoding_records[i * 8 + 4] << 24) | 
                                  ((uint32_t)encoding_records[i * 8 + 5] << 16) |
                                  ((uint32_t)encoding_records[i * 8 + 6] << 8) | 
                                   encoding_records[i * 8 + 7];
        
        uint8_t *subtable = cmap_data + subtable_offset;
        uint16_t format = (subtable[0] << 8) | subtable[1];
        
        // 优先使用format 12（支持UCS-4）
        if(platform_id == 0 && format == 12) {  // Unicode format 12
            return lookup_glyph_in_cmap_format12(subtable, unicode);
        }
        // 否则使用format 4（常用）
        else if(platform_id == 0 && format == 4) {  // Unicode format 4
            return lookup_glyph_in_cmap_format4(subtable, unicode);
        }
        else if(platform_id == 3 && format == 4) {  // Microsoft Symbol format 4
            return lookup_glyph_in_cmap_format4(subtable, unicode);
        }
        else if(platform_id == 3 && format == 12) { // Microsoft Symbol format 12
            return lookup_glyph_in_cmap_format12(subtable, unicode);
        }
    }
    
    return 0;  // 未找到
}

// 批量查找Level1字符的glyph index
static int batch_lookup_glyph_indices(ttf_font_desc_t *dsc, 
                                     const uint32_t *unicode_list, 
                                     uint16_t count,
                                     level1_glyph_info_t *results)
{
    uint32_t cmap_size;
    uint32_t cmap_file_offset;
    uint8_t *cmap_data = NULL;
    int cmap_is_dma = 0;
    
    // 优先从PSRAM内存数据读取，其次从文件读取
    if(dsc->stream.data != NULL) {
        printf("[TTF_DBG] batch: mem path stream.data=%p\n", (void*)dsc->stream.data);
        cmap_data = read_table_from_memory((const uint8_t *)dsc->stream.data, dsc->info.fontstart, "cmap", &cmap_size);
        if(!cmap_data) printf("[TTF_DBG] batch: read_table_from_memory FAILED\n");
    }
    if(!cmap_data) {
        printf("[TTF_DBG] batch: file path %s (reuse dsc->file)\n", dsc->file_path);
        if(!ttf_find_table(dsc, "cmap", &cmap_file_offset, &cmap_size)) {
            printf("[TTF_DBG] batch: ttf_find_table(cmap) FAILED\n");
            return -1;
        }
        printf("[TTF_DBG] batch: cmap ofs=%lu sz=%lu\n", (unsigned long)cmap_file_offset, (unsigned long)cmap_size);
        cmap_data = (uint8_t *)ttf_scratch_alloc(cmap_size, &cmap_is_dma);
        printf("[TTF_DBG] batch: scratch_alloc(%lu)=%p dma=%d\n", (unsigned long)cmap_size, (void*)cmap_data, cmap_is_dma);
        if(cmap_data) {
            lv_fs_seek(&dsc->file, cmap_file_offset, LV_FS_SEEK_SET);
            uint32_t bytes_read = 0;
            while(bytes_read < cmap_size) {
                uint32_t to_read = cmap_size - bytes_read;
                if(to_read > 4096) to_read = 4096;
                uint32_t br;
                lv_fs_read(&dsc->file, cmap_data + bytes_read, to_read, &br);
                if(br == 0) { printf("[TTF_DBG] batch: read 0 at %lu/%lu\n", (unsigned long)bytes_read, (unsigned long)cmap_size); break; }
                bytes_read += br;
            }
            if(bytes_read < cmap_size) {
                printf("[TTF_DBG] batch: read incomplete %lu/%lu\n", (unsigned long)bytes_read, (unsigned long)cmap_size);
                ttf_scratch_free(cmap_data, cmap_is_dma, cmap_size);
                cmap_data = NULL;
            }
        }
    }
    
    if(!cmap_data) {
        printf("[TTF] Failed to read cmap table\n");
        return -1;
    }
    
    int valid_count = 0;
    int not_found_count = 0;
    
    // === 修复1: 预扫描cmap，优先使用format 12（二分查找O(log n)），避免format 4线性扫描 ===
    uint16_t cmap_num_tables = (cmap_data[2] << 8) | cmap_data[3];
    uint8_t *best_subtable = NULL;
    uint16_t best_format = 0;
    uint8_t *encoding_records = cmap_data + 4;
    for(int ei = 0; ei < cmap_num_tables; ei++) {
        uint16_t pid = (encoding_records[ei * 8] << 8) | encoding_records[ei * 8 + 1];
        uint32_t sub_off = ((uint32_t)encoding_records[ei * 8 + 4] << 24) | 
                           ((uint32_t)encoding_records[ei * 8 + 5] << 16) |
                           ((uint32_t)encoding_records[ei * 8 + 6] << 8) | 
                            encoding_records[ei * 8 + 7];
        uint8_t *sub = cmap_data + sub_off;
        uint16_t fmt = (sub[0] << 8) | sub[1];
        if(fmt == 12 && (pid == 0 || pid == 3)) {
            best_subtable = sub;
            best_format = 12;
            break;  // format 12最佳，立即停止
        }
        if(fmt == 4 && best_format != 12 && (pid == 0 || pid == 3)) {
            best_subtable = sub;
            best_format = 4;
        }
    }
    printf("[TTF] cmap pre-scan: selected format %d subtable for batch lookup\n", best_format);
    
    for(uint16_t i = 0; i < count; i++) {
        uint32_t unicode = unicode_list[i];
        uint16_t glyph_index;
        if(best_format == 12) {
            glyph_index = lookup_glyph_in_cmap_format12(best_subtable, unicode);
        } else if(best_format == 4) {
            glyph_index = lookup_glyph_in_cmap_format4(best_subtable, unicode);
        } else {
            glyph_index = lookup_glyph_in_cmap(cmap_data, unicode);
        }
        
        results[i].unicode = unicode;
        results[i].glyph_index = glyph_index;
        results[i].cached = 0;
        results[i].glyf_size = 0;
        results[i].compact_offset = 0;
        results[i].file_glyf_offset = 0;
        
        if(glyph_index > 0) {
            valid_count++;
        } else {
            not_found_count++;
            if(not_found_count <= 20) {
                printf("[TTF] Not found: unicode=0x%04X (index %d)\n", unicode, i);
            }
        }
    }
    
    printf("[TTF] Found %d valid glyphs, %d not found\n", valid_count, not_found_count);

    if(!ttf_find_table(dsc, "cmap", &cmap_file_offset, &cmap_size)) {
        if(dsc->stream.data) {
            find_table_location_in_memory((const uint8_t *)dsc->stream.data, dsc->info.fontstart,
                                          "cmap", &cmap_file_offset, &cmap_size);
        }
    }
    if(ttf_persist_table_cache(&dsc->table_cache.cmap, cmap_data, cmap_file_offset, cmap_size, "cmap") != 0) {
        if(dsc->stream.data == NULL) {
            ttf_scratch_free(cmap_data, cmap_is_dma, cmap_size);
        } else {
            psram_free(cmap_data);
        }
        return -1;
    }
    if(dsc->stream.data == NULL) {
        ttf_scratch_free(cmap_data, cmap_is_dma, cmap_size);
    } else {
        psram_free(cmap_data);
    }
    cmap_data = NULL;
    
    return valid_count;
}

typedef struct {
    uint16_t glyph_index;    // glyph索引
    uint16_t orig_index;     // 原数组索引
    uint32_t file_offset;    // 文件中的绝对偏移
    uint32_t size;           // 数据大小
    uint32_t compact_offset; // 在紧凑 glyf_data 中的目标偏移
} glyf_sort_entry_t;

static int compare_glyf_entries(const void *a, const void *b)
{
    const glyf_sort_entry_t *ga = (const glyf_sort_entry_t *)a;
    const glyf_sort_entry_t *gb = (const glyf_sort_entry_t *)b;
    if(ga->file_offset < gb->file_offset) return -1;
    if(ga->file_offset > gb->file_offset) return 1;
    return 0;
}

static int ttf_build_glyf_lookup_from_glyphs(ttf_font_desc_t * dsc,
                                             level1_glyph_info_t * glyphs,
                                             uint16_t count,
                                             uint32_t glyf_table_file_offset,
                                             uint32_t glyf_table_size)
{
    uint16_t valid_count = ttf_count_glyf_cached(glyphs, count);
    if(valid_count == 0) return 0;

    if(g_glyf_lookup) {
        ttf_meta_free(g_glyf_lookup, g_glyf_lookup_is_dma);
        g_glyf_lookup = NULL;
    }
    {
        int lk_dma = 0;
        g_glyf_lookup = (glyf_cache_entry_t *)ttf_meta_alloc(
            valid_count * sizeof(glyf_cache_entry_t), &lk_dma);
        g_glyf_lookup_is_dma = lk_dma;
    }
    if(!g_glyf_lookup) {
        g_glyf_lookup_count = 0;
        printf("[TTF] WARNING: Failed to allocate glyf lookup table\n");
        return -1;
    }

    g_glyf_lookup_count = valid_count;
    g_glyf_table_file_offset = glyf_table_file_offset;
    g_glyf_table_size = glyf_table_size;

    uint16_t lk_idx = 0;
    for(uint16_t i = 0; i < count; i++) {
        if(glyphs[i].cached) {
            g_glyf_lookup[lk_idx].glyf_rel_offset = glyphs[i].file_glyf_offset;
            g_glyf_lookup[lk_idx].compact_offset = glyphs[i].compact_offset;
            g_glyf_lookup[lk_idx].glyf_size = glyphs[i].glyf_size;
            lk_idx++;
        }
    }
    qsort(g_glyf_lookup, valid_count, sizeof(glyf_cache_entry_t), compare_cache_entries);
    ttf_glyf_lookup_fill_read_spans();
    printf("[TTF] Glyf lookup table built: %u entries, %lu bytes\n",
           valid_count, (uint32_t)(valid_count * sizeof(glyf_cache_entry_t)));

    if(dsc) {
        dsc->table_cache.glyf.file_offset = glyf_table_file_offset;
        dsc->table_cache.glyf.size = glyf_table_size;
        dsc->table_cache.glyf.data = NULL;
    }
    return 0;
}

#if LV_TINY_TTF_FILE_SUPPORT
static int ttf_read_exact(lv_fs_file_t * file, uint8_t * dst, uint32_t size)
{
    uint32_t done = 0;
    while(done < size) {
        uint32_t br = 0;
        if(lv_fs_read(file, dst + done, size - done, &br) != LV_FS_RES_OK || br == 0) {
            return -1;
        }
        done += br;
    }
    return 0;
}

static uint32_t ttf_read_le32(const uint8_t * p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void ttf_l1glyf_sidecar_path_from_ttf(const char * ttf_path, char * out, size_t out_sz)
{
    if(!ttf_path || !out || out_sz == 0) return;
    strncpy(out, ttf_path, out_sz - 1);
    out[out_sz - 1] = '\0';
    char * dot = strrchr(out, '.');
    if(dot) {
        *dot = '\0';
    }
    size_t used = strlen(out);
    if(used + 8 < out_sz) {
        strcpy(out + used, ".l1glyf");
    }
}

static void ttf_l1glyf_hidden_path_from_ttf(const char * ttf_path, char * out, size_t out_sz)
{
    if(!ttf_path || !out || out_sz == 0) return;
    const char * slash = strrchr(ttf_path, '/');
    const char * base = slash ? (slash + 1) : ttf_path;
    char stem[128];
    strncpy(stem, base, sizeof(stem) - 1);
    stem[sizeof(stem) - 1] = '\0';
    char * dot = strrchr(stem, '.');
    if(dot) *dot = '\0';
    snprintf(out, out_sz, "0:/Font/.l1glyf/%s.l1glyf", stem);
}

static int ttf_open_l1glyf_cache_file(const char * ttf_path, char * opened_path, size_t opened_sz,
                                      lv_fs_file_t * cache_file)
{
    char path_try[256];
    ttf_l1glyf_hidden_path_from_ttf(ttf_path, path_try, sizeof(path_try));
    if(lv_fs_open(cache_file, path_try, LV_FS_MODE_RD) == LV_FS_RES_OK) {
        strncpy(opened_path, path_try, opened_sz - 1);
        opened_path[opened_sz - 1] = '\0';
        return 0;
    }
    ttf_l1glyf_sidecar_path_from_ttf(ttf_path, path_try, sizeof(path_try));
    if(lv_fs_open(cache_file, path_try, LV_FS_MODE_RD) == LV_FS_RES_OK) {
        strncpy(opened_path, path_try, opened_sz - 1);
        opened_path[opened_sz - 1] = '\0';
        return 0;
    }
    if(opened_path && opened_sz) opened_path[0] = '\0';
    return -1;
}

static int compare_l1glyf_by_unicode(const void * a, const void * b)
{
    const l1glyf_entry_t * ea = (const l1glyf_entry_t *)a;
    const l1glyf_entry_t * eb = (const l1glyf_entry_t *)b;
    if(ea->unicode < eb->unicode) return -1;
    if(ea->unicode > eb->unicode) return 1;
    return 0;
}

static const l1glyf_entry_t * l1glyf_lookup_by_unicode(const l1glyf_entry_t * table, uint32_t count, uint32_t unicode)
{
    int lo = 0;
    int hi = (int)count - 1;
    while(lo <= hi) {
        int mid = (lo + hi) / 2;
        if(table[mid].unicode < unicode) {
            lo = mid + 1;
        } else if(table[mid].unicode > unicode) {
            hi = mid - 1;
        } else {
            return &table[mid];
        }
    }
    return NULL;
}

static int ttf_pin_loca_and_fill_offsets(ttf_font_desc_t * dsc,
                                         level1_glyph_info_t * glyphs,
                                         uint16_t count,
                                         uint32_t * out_glyf_offset,
                                         uint32_t * out_glyf_size)
{
    uint32_t loca_file_offset = 0;
    uint32_t loca_size = 0;
    uint32_t glyf_offset = 0;
    uint32_t glyf_size = 0;
    uint8_t * loca_data = NULL;
    int loca_is_dma = 0;
    const uint8_t * ttf_mem = (dsc->stream.data != NULL) ? (const uint8_t *)dsc->stream.data : NULL;

    if(dsc->table_cache.loca.data) {
        loca_data = dsc->table_cache.loca.data;
        loca_file_offset = dsc->table_cache.loca.file_offset;
        loca_size = dsc->table_cache.loca.size;
    } else if(ttf_mem) {
        if(!find_table_location_in_memory(ttf_mem, dsc->info.fontstart, "loca", &loca_file_offset, &loca_size)) {
            return -1;
        }
        loca_data = (uint8_t *)ttf_scratch_alloc(loca_size, &loca_is_dma);
        if(!loca_data) return -1;
        lv_memcpy(loca_data, ttf_mem + loca_file_offset, loca_size);
        if(ttf_persist_table_cache(&dsc->table_cache.loca, loca_data, loca_file_offset, loca_size, "loca") != 0) {
            ttf_scratch_free(loca_data, loca_is_dma, loca_size);
            return -1;
        }
        ttf_scratch_free(loca_data, loca_is_dma, loca_size);
        loca_data = dsc->table_cache.loca.data;
    } else if(dsc->file_path[0]) {
        if(!ttf_find_table(dsc, "loca", &loca_file_offset, &loca_size)) {
            return -1;
        }
        loca_data = (uint8_t *)ttf_scratch_alloc(loca_size, &loca_is_dma);
        if(!loca_data) return -1;
        lv_fs_seek(&dsc->file, loca_file_offset, LV_FS_SEEK_SET);
        if(ttf_read_exact(&dsc->file, loca_data, loca_size) != 0) {
            ttf_scratch_free(loca_data, loca_is_dma, loca_size);
            return -1;
        }
        if(ttf_persist_table_cache(&dsc->table_cache.loca, loca_data, loca_file_offset, loca_size, "loca") != 0) {
            ttf_scratch_free(loca_data, loca_is_dma, loca_size);
            return -1;
        }
        ttf_scratch_free(loca_data, loca_is_dma, loca_size);
        loca_data = dsc->table_cache.loca.data;
    } else {
        return -1;
    }

    if(ttf_mem) {
        if(!find_table_location_in_memory(ttf_mem, dsc->info.fontstart, "glyf", &glyf_offset, &glyf_size)) {
            return -1;
        }
    } else if(!ttf_find_table(dsc, "glyf", &glyf_offset, &glyf_size)) {
        return -1;
    }

    for(uint16_t i = 0; i < count; i++) {
        if(!glyphs[i].cached || glyphs[i].glyph_index == 0) continue;
        uint16_t idx = glyphs[i].glyph_index;
        uint32_t g1 = 0;
        uint32_t g2 = 0;
        if(dsc->info.indexToLocFormat == 0) {
            uint16_t off1 = (loca_data[idx * 2] << 8) | loca_data[idx * 2 + 1];
            uint16_t off2 = (loca_data[(idx + 1) * 2] << 8) | loca_data[(idx + 1) * 2 + 1];
            g1 = glyf_offset + off1 * 2;
            g2 = glyf_offset + off2 * 2;
        } else {
            uint32_t off1 = (loca_data[idx * 4] << 24) | (loca_data[idx * 4 + 1] << 16) |
                            (loca_data[idx * 4 + 2] << 8) | loca_data[idx * 4 + 3];
            uint32_t off2 = (loca_data[(idx + 1) * 4] << 24) | (loca_data[(idx + 1) * 4 + 1] << 16) |
                            (loca_data[(idx + 1) * 4 + 2] << 8) | loca_data[(idx + 1) * 4 + 3];
            g1 = glyf_offset + off1;
            g2 = glyf_offset + off2;
        }
        glyphs[i].file_glyf_offset = g1 - glyf_offset;
        if(glyphs[i].glyf_size == 0) {
            glyphs[i].glyf_size = (uint16_t)(g2 - g1);
        }
    }

    if(out_glyf_offset) *out_glyf_offset = glyf_offset;
    if(out_glyf_size) *out_glyf_size = glyf_size;
    return 0;
}

/* 0=missing, 1=ok, -1=invalid */
static int ttf_try_load_l1glyf_cache(ttf_font_desc_t * dsc,
                                     level1_glyph_info_t * glyphs,
                                     uint16_t count,
                                     uint8_t ** out_data,
                                     uint32_t * out_size)
{
    if(!dsc || dsc->file_path[0] == '\0' || dsc->stream.data != NULL) {
        return 0;
    }

    uint32_t t0 = xTaskGetTickCount();
    char cache_path[256];
    lv_fs_file_t cache_file;
    if(ttf_open_l1glyf_cache_file(dsc->file_path, cache_path, sizeof(cache_path), &cache_file) != 0) {
        printf("[L1GLYF] No cache file for %s\n", dsc->file_path);
        return 0;
    }

    uint8_t hdr_buf[L1GLYF_HEADER_TOTAL_SIZE];
    if(ttf_read_exact(&cache_file, hdr_buf, L1GLYF_HEADER_TOTAL_SIZE) != 0) {
        lv_fs_close(&cache_file);
        printf("[L1GLYF] Read header failed: %s\n", cache_path);
        return -1;
    }

    l1glyf_header_t hdr;
    hdr.magic = ttf_read_le32(hdr_buf + 0);
    hdr.version = (uint16_t)(hdr_buf[4] | (hdr_buf[5] << 8));
    hdr.lookup_count = ttf_read_le32(hdr_buf + 16);
    hdr.glyf_data_size = ttf_read_le32(hdr_buf + 20);

    if(hdr.magic != L1GLYF_MAGIC || hdr.version != L1GLYF_VERSION) {
        lv_fs_close(&cache_file);
        printf("[L1GLYF] Bad file: %s\n", cache_path);
        return -1;
    }
    if(hdr.lookup_count == 0 || hdr.glyf_data_size == 0) {
        lv_fs_close(&cache_file);
        printf("[L1GLYF] Empty cache: %s\n", cache_path);
        return -1;
    }

    uint32_t lookup_bytes = hdr.lookup_count * (uint32_t)sizeof(l1glyf_entry_t);
    uint32_t lookup_off = L1GLYF_HEADER_TOTAL_SIZE;
    uint32_t data_off = lookup_off + lookup_bytes;

    int lookup_is_dma = 0;
    l1glyf_entry_t * lookup = (l1glyf_entry_t *)ttf_scratch_alloc(lookup_bytes, &lookup_is_dma);
    if(!lookup) {
        lv_fs_close(&cache_file);
        printf("[L1GLYF] lookup alloc failed (%lu bytes)\n", (unsigned long)lookup_bytes);
        return -1;
    }

    if(lv_fs_seek(&cache_file, lookup_off, LV_FS_SEEK_SET) != LV_FS_RES_OK ||
       ttf_read_exact(&cache_file, (uint8_t *)lookup, lookup_bytes) != 0) {
        ttf_scratch_free(lookup, lookup_is_dma, lookup_bytes);
        lv_fs_close(&cache_file);
        printf("[L1GLYF] Read lookup failed\n");
        return -1;
    }

    qsort(lookup, hdr.lookup_count, sizeof(l1glyf_entry_t), compare_l1glyf_by_unicode);

    uint16_t matched = 0;
    for(uint16_t i = 0; i < count; i++) {
        const l1glyf_entry_t * e = l1glyf_lookup_by_unicode(lookup, hdr.lookup_count, (uint32_t)glyphs[i].unicode);
        if(!e || e->glyf_size == 0) {
            continue;
        }
        glyphs[i].compact_offset = e->compact_offset;
        glyphs[i].glyf_size = (uint16_t)e->glyf_size;
        glyphs[i].cached = 1;
        matched++;
    }

    uint8_t * glyf_data = psram_malloc(hdr.glyf_data_size);
    if(!glyf_data) {
        ttf_scratch_free(lookup, lookup_is_dma, lookup_bytes);
        lv_fs_close(&cache_file);
        printf("[L1GLYF] psram_malloc(%lu) failed\n", (unsigned long)hdr.glyf_data_size);
        return -1;
    }

    if(lv_fs_seek(&cache_file, data_off, LV_FS_SEEK_SET) != LV_FS_RES_OK ||
       ttf_read_exact(&cache_file, glyf_data, hdr.glyf_data_size) != 0) {
        psram_free(glyf_data);
        ttf_scratch_free(lookup, lookup_is_dma, lookup_bytes);
        lv_fs_close(&cache_file);
        printf("[L1GLYF] Read glyf_data failed\n");
        return -1;
    }
    lv_fs_close(&cache_file);
    ttf_scratch_free(lookup, lookup_is_dma, lookup_bytes);

    uint32_t glyf_offset = 0;
    uint32_t glyf_size = 0;
    if(ttf_pin_loca_and_fill_offsets(dsc, glyphs, count, &glyf_offset, &glyf_size) == 0) {
        ttf_build_glyf_lookup_from_glyphs(dsc, glyphs, count, glyf_offset, glyf_size);
    } else {
        printf("[L1GLYF] Warning: loca pin failed, stream glyf cache disabled\n");
    }

    printf("[L1GLYF] Loaded %s: glyf=%lu bytes mapped=%u/%u %lums\n",
           cache_path, (unsigned long)hdr.glyf_data_size,
           (unsigned)matched, (unsigned)count,
           (unsigned long)(xTaskGetTickCount() - t0));

    *out_data = glyf_data;
    *out_size = hdr.glyf_data_size;
    return 1;
}
#endif /* LV_TINY_TTF_FILE_SUPPORT */

static int batch_read_glyf_data(ttf_font_desc_t *dsc,
                                level1_glyph_info_t *glyphs,
                                uint16_t count,
                                uint8_t **out_data,
                                uint32_t *out_size)
{
    uint32_t loca_file_offset, loca_size;
    uint8_t *loca_data = NULL;
    int loca_is_dma = 0;
    int sort_is_dma = 0;
    uint32_t glyf_offset, glyf_size;
    const uint8_t *ttf_mem = (dsc->stream.data != NULL) ? (const uint8_t *)dsc->stream.data : NULL;
    int use_dsc_file = (!ttf_mem && dsc->file_path[0] != '\0');
    
    if(ttf_mem) {
        if(!find_table_location_in_memory(ttf_mem, dsc->info.fontstart, "loca", &loca_file_offset, &loca_size)) {
            printf("[TTF_DBG] batch: loca table not found in memory\n");
            return -1;
        }
        loca_data = (uint8_t *)ttf_scratch_alloc(loca_size, &loca_is_dma);
        if(!loca_data) {
            printf("[TTF_DBG] batch: scratch_alloc(loca %lu) failed\n", (unsigned long)loca_size);
            return -1;
        }
        lv_memcpy(loca_data, ttf_mem + loca_file_offset, loca_size);
        if(!find_table_location_in_memory(ttf_mem, dsc->info.fontstart, "glyf", &glyf_offset, &glyf_size)) {
            ttf_scratch_free(loca_data, loca_is_dma, loca_size);
            printf("[TTF_DBG] batch: glyf table not found in memory\n");
            return -1;
        }
    } else if(use_dsc_file) {
        if(!ttf_find_table(dsc, "loca", &loca_file_offset, &loca_size)) {
            printf("[TTF_DBG] batch: loca table not found via dsc->file\n");
            ttf_list_tables(dsc);
            if(ttf_font_has_cff_outlines(dsc)) {
                printf("[TTF_DBG] batch: CFF outline font, glyf batch skipped\n");
            }
            return -1;
        }
        loca_data = (uint8_t *)ttf_scratch_alloc(loca_size, &loca_is_dma);
        if(!loca_data) {
            printf("[TTF_DBG] batch: scratch_alloc(loca %lu) failed\n", (unsigned long)loca_size);
            return -1;
        }
        lv_fs_seek(&dsc->file, loca_file_offset, LV_FS_SEEK_SET);
        uint32_t bytes_read = 0;
        while(bytes_read < loca_size) {
            uint32_t to_read = loca_size - bytes_read;
            if(to_read > 4096) to_read = 4096;
            uint32_t br;
            lv_fs_read(&dsc->file, loca_data + bytes_read, to_read, &br);
            if(br == 0) break;
            bytes_read += br;
        }
        if(bytes_read < loca_size) {
            ttf_scratch_free(loca_data, loca_is_dma, loca_size);
            printf("[TTF_DBG] batch: read loca (%lu) failed\n", (unsigned long)loca_size);
            return -1;
        }
        if(!ttf_find_table(dsc, "glyf", &glyf_offset, &glyf_size)) {
            ttf_scratch_free(loca_data, loca_is_dma, loca_size);
            printf("[TTF_DBG] batch: glyf table not found via dsc->file\n");
            return -1;
        }
    } else {
        printf("[TTF_DBG] batch: no font data source\n");
        return -1;
    }
    
    printf("[TTF] indexToLocFormat=%d (0=short, 1=long)\n", dsc->info.indexToLocFormat);
    printf("[TTF] glyf_offset=%lu, loca_size=%lu\n", glyf_offset, loca_size);
    
    // 第一步：计算所有有效 glyph 的文件偏移
    uint32_t min_glyf_file = 0xFFFFFFFF, max_glyf_file = 0;
    uint16_t valid_count = 0;
    
    for(uint16_t i = 0; i < count; i++) {
        if(glyphs[i].glyph_index > 0) {
            uint32_t g1 = 0, g2 = 0;
            uint16_t idx = glyphs[i].glyph_index;
            if(dsc->info.indexToLocFormat == 0) {
                uint16_t off1 = (loca_data[idx * 2] << 8) | loca_data[idx * 2 + 1];
                uint16_t off2 = (loca_data[(idx + 1) * 2] << 8) | loca_data[(idx + 1) * 2 + 1];
                g1 = glyf_offset + off1 * 2;
                g2 = glyf_offset + off2 * 2;
            } else {
                uint32_t off1 = (loca_data[idx * 4] << 24) | (loca_data[idx * 4 + 1] << 16) | (loca_data[idx * 4 + 2] << 8) | loca_data[idx * 4 + 3];
                uint32_t off2 = (loca_data[(idx + 1) * 4] << 24) | (loca_data[(idx + 1) * 4 + 1] << 16) | (loca_data[(idx + 1) * 4 + 2] << 8) | loca_data[(idx + 1) * 4 + 3];
                g1 = glyf_offset + off1;
                g2 = glyf_offset + off2;
            }
            glyphs[i].glyf_size = (uint16_t)(g2 - g1);
            glyphs[i].file_glyf_offset = g1 - glyf_offset;
            glyphs[i].cached = 0;
            if(g1 != g2) {
                if(g1 < min_glyf_file) min_glyf_file = g1;
                if(g2 > max_glyf_file) max_glyf_file = g2;
                glyphs[i].cached = 1;
                valid_count++;
            }
        }
    }
    
    if(valid_count == 0) {
        ttf_scratch_free(loca_data, loca_is_dma, loca_size);
        return 0;
    }
    
    // 紧凑存储：按文件偏移排序，合并相邻读取，紧凑排列到缓冲区
    // 先计算总大小
    uint32_t total_glyf_size = 0;
    for(uint16_t i = 0; i < count; i++) {
        if(glyphs[i].cached) {
            total_glyf_size += glyphs[i].glyf_size;
        }
    }
    
    printf("[TTF] Compact storage: valid=%u total_glyf_size=%lu (vs one-to-one %lu, saved %lu%%)\n", 
           valid_count, total_glyf_size, max_glyf_file - glyf_offset,
           (max_glyf_file - glyf_offset > 0) ? 
           (100 - total_glyf_size * 100 / (max_glyf_file - glyf_offset)) : 0);
    
    /* loca 常驻 PSRAM：宁可裁 glyf 字数，也不释放 */
    if(ttf_persist_table_cache(&dsc->table_cache.loca, loca_data, loca_file_offset, loca_size, "loca") != 0) {
        ttf_scratch_free(loca_data, loca_is_dma, loca_size);
        return -1;
    }
    ttf_scratch_free(loca_data, loca_is_dma, loca_size);
    loca_data = NULL;

    uint32_t hmtx_sz = 0, hmtx_off_dummy = 0;
    ttf_find_table(dsc, "hmtx", &hmtx_off_dummy, &hmtx_sz);
    uint32_t pending_pin = hmtx_sz + dsc->table_cache.cmap.size;
    printf("[TTF] pinned tables: loca=%lu hmtx=%lu cmap=%lu (pending_pin=%lu for partial trim)\n",
           (unsigned long)loca_size, (unsigned long)hmtx_sz,
           (unsigned long)dsc->table_cache.cmap.size, (unsigned long)pending_pin);

    uint32_t full_glyf_size = total_glyf_size;
    uint16_t full_valid = valid_count;
    uint8_t l1_partial = 0;

    printf("[TTF] L1 FULL try: glyphs=%u bytes=%lu free=%lu full_budget=%lu\n",
           (unsigned)full_valid, (unsigned long)full_glyf_size,
           (unsigned long)psram_GetFreeHeapSize(), (unsigned long)ttf_glyf_psram_budget_full());

    uint8_t *glyf_data = psram_malloc(full_glyf_size);
    if(!glyf_data) {
        glyf_data = psram_malloc(full_glyf_size);
    }

    if(glyf_data) {
        total_glyf_size = full_glyf_size;
        valid_count = full_valid;
        printf("[TTF] L1 mode=FULL (%u glyphs, %lu bytes)\n",
               (unsigned)valid_count, (unsigned long)total_glyf_size);
    } else {
        l1_partial = 1;
        printf("[TTF] L1 FULL miss (malloc %lu), fallback PARTIAL trim...\n",
               (unsigned long)full_glyf_size);
        ttf_sort_glyphs_for_budget(glyphs, count);
        total_glyf_size = full_glyf_size;
        valid_count = full_valid;
        ttf_trim_glyphs_to_budget(glyphs, count, &total_glyf_size, &valid_count, pending_pin);
        if(valid_count == 0 || total_glyf_size == 0) {
            printf("[TTF_DBG] batch: no glyf fits in psram budget\n");
            return -1;
        }

        printf("[TTF_DBG] batch: psram_heap free before glyf=%lu need=%lu\n",
               (unsigned long)psram_GetFreeHeapSize(), (unsigned long)total_glyf_size);

        glyf_data = psram_malloc(total_glyf_size);
        while(!glyf_data && valid_count > 0) {
            for(int ri = (int)count - 1; ri >= 0; ri--) {
                if(glyphs[ri].cached) {
                    total_glyf_size -= glyphs[ri].glyf_size;
                    glyphs[ri].cached = 0;
                    valid_count--;
                    printf("[TTF_DBG] batch: shrink glyf to %u glyphs / %lu bytes\n",
                           (unsigned)valid_count, (unsigned long)total_glyf_size);
                    break;
                }
            }
            if(valid_count == 0 || total_glyf_size == 0) break;
            glyf_data = psram_malloc(total_glyf_size);
        }
        if(!glyf_data) {
            printf("[TTF_DBG] batch: psram_malloc(glyf %lu) failed, free=%lu (likely fragmented)\n",
                   (unsigned long)total_glyf_size, (unsigned long)psram_GetFreeHeapSize());
            return -1;
        }
        printf("[TTF] L1 mode=PARTIAL (%u/%u glyphs, %lu bytes)\n",
               (unsigned)valid_count, (unsigned)full_valid, (unsigned long)total_glyf_size);
    }

    if(!l1_partial) {
        printf("[TTF_DBG] batch: psram_heap free before glyf=%lu need=%lu\n",
               (unsigned long)psram_GetFreeHeapSize(), (unsigned long)total_glyf_size);
    }
    
    // 构建排序数组，按文件偏移排序以合并相邻读取
    glyf_sort_entry_t *sort_entries = (glyf_sort_entry_t *)ttf_scratch_alloc(
        valid_count * sizeof(glyf_sort_entry_t), &sort_is_dma);
    if(!sort_entries) {
        psram_free(glyf_data);
        printf("[TTF_DBG] batch: scratch_alloc(sort_entries %u) failed\n", valid_count);
        return -1;
    }
    
    uint16_t sort_idx = 0;
    for(uint16_t i = 0; i < count; i++) {
        if(glyphs[i].cached) {
            sort_entries[sort_idx].glyph_index = glyphs[i].glyph_index;
            sort_entries[sort_idx].orig_index = i;
            sort_entries[sort_idx].file_offset = glyf_offset + glyphs[i].file_glyf_offset;
            sort_entries[sort_idx].size = glyphs[i].glyf_size;
            sort_idx++;
        }
    }
    
    // 按文件偏移排序
    qsort(sort_entries, valid_count, sizeof(glyf_sort_entry_t), compare_glyf_entries);

    /* 预分配每个 glyph 在紧凑缓冲中的目标位置 */
    {
        uint32_t compact_pos = 0;
        for(uint16_t si = 0; si < valid_count; si++) {
            sort_entries[si].compact_offset = compact_pos;
            compact_pos += sort_entries[si].size;
        }
    }

    uint32_t read_ops = 0;
    uint32_t physical_bytes = 0;
    uint32_t useful_bytes = 0;

#if LV_TINY_TTF_FILE_SUPPORT
    int io_buf_is_dma = 0;
    uint32_t io_buf_size = 0;
    uint8_t * io_buf = NULL;
    if(!ttf_mem) {
        io_buf = (uint8_t *)ttf_scratch_alloc(GLYF_IO_BUFFER_SIZE, &io_buf_is_dma);
        if(io_buf) {
            io_buf_size = GLYF_IO_BUFFER_SIZE;
        } else {
            io_buf = (uint8_t *)ttf_scratch_alloc(32U * 1024U, &io_buf_is_dma);
            if(io_buf) {
                io_buf_size = 32U * 1024U;
            }
        }
        if(io_buf) {
            printf("[TTF_IO] run buffer %lu bytes on %s (gap_max=%u run_max=%u)\n",
                   (unsigned long)io_buf_size,
                   io_buf_is_dma ? "DMA" : "psram",
                   (unsigned)GLYF_MERGE_GAP_MAX, (unsigned)GLYF_READ_RUN_MAX);
        }
    }
    uint32_t effective_run_max = io_buf_size ? io_buf_size : GLYF_READ_RUN_MAX;
    if(effective_run_max > GLYF_READ_RUN_MAX) {
        effective_run_max = GLYF_READ_RUN_MAX;
    }
#else
    uint32_t effective_run_max = GLYF_READ_RUN_MAX;
#endif

    uint16_t i = 0;
    while(i < valid_count) {
        uint16_t run_first = i;
        uint16_t run_last = i;
        uint32_t run_start = sort_entries[i].file_offset;
        uint32_t run_end = run_start + sort_entries[i].size;

        while(run_last + 1 < valid_count) {
            glyf_sort_entry_t * next = &sort_entries[run_last + 1];
            uint32_t next_start = next->file_offset;
            uint32_t next_end = next_start + next->size;
            uint32_t gap = (next_start > run_end) ? (next_start - run_end) : 0;
            uint32_t proposed_end = (next_end > run_end) ? next_end : run_end;
            uint32_t proposed_size = proposed_end - run_start;

            if(gap > GLYF_MERGE_GAP_MAX) {
                break;
            }
            if(proposed_size > effective_run_max) {
                break;
            }

            run_last++;
            run_end = proposed_end;
        }

        uint32_t run_size = run_end - run_start;

        if(ttf_mem) {
            for(uint16_t k = run_first; k <= run_last; k++) {
                glyf_sort_entry_t * e = &sort_entries[k];
                uint32_t src_off = e->file_offset - run_start;
                lv_memcpy(glyf_data + e->compact_offset,
                          ttf_mem + run_start + src_off, e->size);
                glyphs[e->orig_index].compact_offset = e->compact_offset;
                useful_bytes += e->size;
            }
            physical_bytes += run_size;
            read_ops++;
        }
#if LV_TINY_TTF_FILE_SUPPORT
        else if(io_buf && run_size <= io_buf_size) {
            if(lv_fs_seek(&dsc->file, run_start, LV_FS_SEEK_SET) != LV_FS_RES_OK) {
                ttf_scratch_free(io_buf, io_buf_is_dma, io_buf_size);
                psram_free(glyf_data);
                ttf_scratch_free(sort_entries, sort_is_dma, valid_count * sizeof(glyf_sort_entry_t));
                return -1;
            }
            if(ttf_read_exact(&dsc->file, io_buf, run_size) != 0) {
                ttf_scratch_free(io_buf, io_buf_is_dma, io_buf_size);
                psram_free(glyf_data);
                ttf_scratch_free(sort_entries, sort_is_dma, valid_count * sizeof(glyf_sort_entry_t));
                return -1;
            }
            read_ops++;
            physical_bytes += run_size;
            for(uint16_t k = run_first; k <= run_last; k++) {
                glyf_sort_entry_t * e = &sort_entries[k];
                uint32_t src_off = e->file_offset - run_start;
                lv_memcpy(glyf_data + e->compact_offset, io_buf + src_off, e->size);
                glyphs[e->orig_index].compact_offset = e->compact_offset;
                useful_bytes += e->size;
            }
        }
#endif
        else {
            /* 无 IO 缓冲或 run 过大：逐 glyph 直读（退化路径） */
            for(uint16_t k = run_first; k <= run_last; k++) {
                glyf_sort_entry_t * e = &sort_entries[k];
#if LV_TINY_TTF_FILE_SUPPORT
                if(lv_fs_seek(&dsc->file, e->file_offset, LV_FS_SEEK_SET) != LV_FS_RES_OK) {
                    if(io_buf) ttf_scratch_free(io_buf, io_buf_is_dma, io_buf_size);
                    psram_free(glyf_data);
                    ttf_scratch_free(sort_entries, sort_is_dma, valid_count * sizeof(glyf_sort_entry_t));
                    return -1;
                }
                if(ttf_read_exact(&dsc->file, glyf_data + e->compact_offset, e->size) != 0) {
                    if(io_buf) ttf_scratch_free(io_buf, io_buf_is_dma, io_buf_size);
                    psram_free(glyf_data);
                    ttf_scratch_free(sort_entries, sort_is_dma, valid_count * sizeof(glyf_sort_entry_t));
                    return -1;
                }
                read_ops++;
                physical_bytes += e->size;
                useful_bytes += e->size;
                glyphs[e->orig_index].compact_offset = e->compact_offset;
#endif
            }
        }

        i = run_last + 1;
    }

#if LV_TINY_TTF_FILE_SUPPORT
    if(io_buf) {
        ttf_scratch_free(io_buf, io_buf_is_dma, io_buf_size);
    }
#endif

    printf("[TTF] Merged %u glyph reads into %lu batch read ops (saved %u ops, %u%% reduction)\n",
           valid_count, read_ops, valid_count - (uint16_t)read_ops,
           (valid_count > 0) ? (valid_count - (uint16_t)read_ops) * 100 / valid_count : 0);
    printf("[TTF_IO] ops=%lu physical=%lu useful=%lu overhead=%lu (gap_max=%u run_max=%u)\n",
           (unsigned long)read_ops, (unsigned long)physical_bytes,
           (unsigned long)useful_bytes, (unsigned long)(physical_bytes - useful_bytes),
           (unsigned)GLYF_MERGE_GAP_MAX, (unsigned)GLYF_READ_RUN_MAX);
    
    ttf_scratch_free(sort_entries, sort_is_dma, valid_count * sizeof(glyf_sort_entry_t));
    
    printf("[TTF] Glyf loaded: %u glyphs, %lu read ops, compact size=%lu\n",
           valid_count, read_ops, total_glyf_size);
    
    if(!dsc->table_cache.loca.data) {
        dsc->table_cache.loca.file_offset = loca_file_offset;
        dsc->table_cache.loca.size = loca_size;
    }
    
    // glyf表缓存：存储真实的偏移和大小供全局查找表使用
    ttf_build_glyf_lookup_from_glyphs(dsc, glyphs, count, glyf_offset, glyf_size);
    
    *out_data = glyf_data;
    *out_size = total_glyf_size;
    return 1;
}

static void ttf_verify_level1_glyphs(const level1_glyph_info_t * glyphs, uint16_t count)
{
    uint16_t need_glyf = 0;
    uint16_t empty_glyf = 0;
    uint16_t uncached = 0;

    for(uint16_t i = 0; i < count; i++) {
        if(glyphs[i].glyph_index == 0) continue;
        if(glyphs[i].glyf_size == 0) {
            empty_glyf++;
            continue;
        }
        need_glyf++;
        if(!glyphs[i].cached) uncached++;
    }
    printf("[TTF_VERIFY] listed=%u need_glyf=%u empty=%u uncached=%u\n",
           (unsigned)count, (unsigned)need_glyf, (unsigned)empty_glyf, (unsigned)uncached);
}

static int ttf_load_level1_glyphs(ttf_font_desc_t *dsc, const uint32_t *unicode_list, uint16_t count)
{
    printf("[TTF] Loading Level1 glyphs for %d chars...\n", count);
    
    // 释放旧数据
    if(dsc->level1_glyphs != NULL) {
        if(dsc->level1_glyphs != g_shared_level1_glyphs) {
            ttf_free_level1_glyphs(dsc->level1_glyphs);
        }
        dsc->level1_glyphs = NULL;
    }
    if(dsc->level1_glyf_data != NULL) {
        psram_free(dsc->level1_glyf_data);
        dsc->level1_glyf_data = NULL;
    }
    
    // 步骤1：批量查找glyph index（使用优化的查找函数，只读一次cmap表）
    printf("[TTF] Step 1: Looking up glyph indices...\n");

    if(ttf_ensure_metrics_cache(dsc) != 0) {
        printf("[TTF] Warning: metrics_cache alloc failed, layout may use stbtt fallback\n");
    }

    int glyphs_is_dma = 0;
    level1_glyph_info_t *glyphs = (level1_glyph_info_t *)ttf_scratch_alloc(
        count * sizeof(level1_glyph_info_t), &glyphs_is_dma);
    if(!glyphs) {
        glyphs = (level1_glyph_info_t *)psram_malloc(count * sizeof(level1_glyph_info_t));
        glyphs_is_dma = 0;
    }
    if(!glyphs) {
        printf("[TTF] Failed to allocate glyph info array\n");
        return -1;
    }
    
    int valid_count = batch_lookup_glyph_indices(dsc, unicode_list, count, glyphs);
    if(valid_count < 0) {
        ttf_scratch_free(glyphs, glyphs_is_dma, count * sizeof(level1_glyph_info_t));
        return -1;
    }
    
    printf("[TTF] Found %d valid glyphs\n", valid_count);

    // 步骤2：优先读 .l1glyf 缓存，失败则从 TTF 批量读 glyf
    printf("[TTF] Step 2: Loading glyf data...\n");
    uint8_t *glyf_data = NULL;
    uint32_t glyf_size = 0;
    int glyf_src = 0;

#if LV_TINY_TTF_FILE_SUPPORT
    glyf_src = ttf_try_load_l1glyf_cache(dsc, glyphs, count, &glyf_data, &glyf_size);
#endif
    if(glyf_src <= 0) {
        if(glyf_src < 0) {
            printf("[L1GLYF] Cache invalid, fallback to TTF batch read\n");
        }
        if(batch_read_glyf_data(dsc, glyphs, count, &glyf_data, &glyf_size) < 0) {
            ttf_scratch_free(glyphs, glyphs_is_dma, count * sizeof(level1_glyph_info_t));
            return -1;
        }
    } else {
        printf("[TTF] Step 2: glyf from .l1glyf cache (skipped TTF batch read)\n");
    }
    
    printf("[TTF] Loaded %lu bytes of glyf data\n", glyf_size);

    uint16_t cached_glyphs = ttf_count_glyf_cached(glyphs, count);
    if(cached_glyphs == 0) {
        ttf_scratch_free(glyphs, glyphs_is_dma, count * sizeof(level1_glyph_info_t));
        psram_free(glyf_data);
        return -1;
    }

    level1_glyph_info_t *glyphs_keep = (level1_glyph_info_t *)ttf_meta_alloc(
        count * sizeof(level1_glyph_info_t), &g_level1_glyphs_is_dma);
    if(!glyphs_keep) {
        printf("[TTF] Failed to allocate persistent glyph info (%u bytes)\n",
               (unsigned)(count * sizeof(level1_glyph_info_t)));
        ttf_scratch_free(glyphs, glyphs_is_dma, count * sizeof(level1_glyph_info_t));
        psram_free(glyf_data);
        return -1;
    }
    lv_memcpy(glyphs_keep, glyphs, count * sizeof(level1_glyph_info_t));
    ttf_scratch_free(glyphs, glyphs_is_dma, count * sizeof(level1_glyph_info_t));
    glyphs = glyphs_keep;

    printf("[TTF] Step 3: Pre-filling metrics_cache (%u/%u glyphs in L1 glyf cache)...\n",
           (unsigned)cached_glyphs, (unsigned)count);

    dsc->level1_glyf_data = glyf_data;
    dsc->level1_glyf_total_size = glyf_size;
    dsc->level1_glyph_count = count;
    dsc->level1_glyphs = glyphs;
    dsc->level1_loaded = 1;
    dsc->table_cache.glyf.data = glyf_data;
    dsc->table_cache.active = 1;

    /* P0 fix: 先缓存 head/hhea/OS2，因为 hmtx 处理需要 hhea.numberOfHMetrics */
    {
        const uint8_t *ttf_mem_cfg = (dsc->stream.data != NULL) ? (const uint8_t *)dsc->stream.data : NULL;
        if(ttf_mem_cfg) {
            uint32_t tbl_off, tbl_sz;
            if(find_table_location_in_memory(ttf_mem_cfg, dsc->info.fontstart, "head", &tbl_off, &tbl_sz)) {
                dsc->table_cache.head.data = psram_malloc(tbl_sz);
                if(dsc->table_cache.head.data) { lv_memcpy(dsc->table_cache.head.data, ttf_mem_cfg + tbl_off, tbl_sz); dsc->table_cache.head.file_offset = tbl_off; dsc->table_cache.head.size = tbl_sz; }
            }
            if(find_table_location_in_memory(ttf_mem_cfg, dsc->info.fontstart, "hhea", &tbl_off, &tbl_sz)) {
                dsc->table_cache.hhea.data = psram_malloc(tbl_sz);
                if(dsc->table_cache.hhea.data) { lv_memcpy(dsc->table_cache.hhea.data, ttf_mem_cfg + tbl_off, tbl_sz); dsc->table_cache.hhea.file_offset = tbl_off; dsc->table_cache.hhea.size = tbl_sz; }
            }
            if(find_table_location_in_memory(ttf_mem_cfg, dsc->info.fontstart, "OS/2", &tbl_off, &tbl_sz)) {
                dsc->table_cache.os2.data = psram_malloc(tbl_sz);
                if(dsc->table_cache.os2.data) { lv_memcpy(dsc->table_cache.os2.data, ttf_mem_cfg + tbl_off, tbl_sz); dsc->table_cache.os2.file_offset = tbl_off; dsc->table_cache.os2.size = tbl_sz; }
            }
        } else if(dsc->file_path[0]) {
            dsc->table_cache.head.data = read_table_to_psram(&dsc->file, dsc->info.fontstart, "head", &dsc->table_cache.head.size);
            if(dsc->table_cache.head.data) ttf_find_table(dsc, "head", &dsc->table_cache.head.file_offset, &dsc->table_cache.head.size);
            dsc->table_cache.hhea.data = read_table_to_psram(&dsc->file, dsc->info.fontstart, "hhea", &dsc->table_cache.hhea.size);
            if(dsc->table_cache.hhea.data) ttf_find_table(dsc, "hhea", &dsc->table_cache.hhea.file_offset, &dsc->table_cache.hhea.size);
            dsc->table_cache.os2.data = read_table_to_psram(&dsc->file, dsc->info.fontstart, "OS/2", &dsc->table_cache.os2.size);
            if(dsc->table_cache.os2.data) ttf_find_table(dsc, "OS/2", &dsc->table_cache.os2.file_offset, &dsc->table_cache.os2.size);
        }
    }

    uint32_t hmtx_file_offset, hmtx_size;
    uint8_t *hmtx_data = NULL;
    const uint8_t *ttf_mem2 = (dsc->stream.data != NULL) ? (const uint8_t *)dsc->stream.data : NULL;

    if(ttf_mem2) {
        hmtx_data = read_table_from_memory(ttf_mem2, dsc->info.fontstart, "hmtx", &hmtx_size);
        if(hmtx_data) {
            find_table_location_in_memory(ttf_mem2, dsc->info.fontstart, "hmtx", &hmtx_file_offset, &hmtx_size);
        }
    } else if(dsc->file_path[0]) {
        hmtx_data = ttf_read_table_via_path(dsc->file_path, dsc->info.fontstart, "hmtx",
                                            &hmtx_file_offset, &hmtx_size);
        if(!hmtx_data) {
            printf("[TTF] Failed to read hmtx via dedicated file open\n");
        }
    }

    if(hmtx_data) {
        uint16_t num_glyphs = dsc->info.numGlyphs;
        printf("[TTF] hmtx_size=%lu, num_glyphs=%u\n", hmtx_size, num_glyphs);
        
        // 从 hhea 表获取 numberOfHMetrics
        uint16_t numberOfHMetrics = 0;
        printf("[TTF] hhea.data=%p hhea.file_offset=%lu hhea.size=%lu\n", 
               dsc->table_cache.hhea.data, 
               (uint32_t)dsc->table_cache.hhea.file_offset,
               (uint32_t)dsc->table_cache.hhea.size);
        if(dsc->table_cache.hhea.data) {
            // 打印 hhea 表的前40字节内容
            printf("[TTF] hhea content (first 40 bytes): ");
            for(int j = 0; j < 40 && j < dsc->table_cache.hhea.size; j++) {
                printf("%02X ", dsc->table_cache.hhea.data[j]);
            }
            printf("\n");
            numberOfHMetrics = (dsc->table_cache.hhea.data[34] << 8) | dsc->table_cache.hhea.data[35];
        } else {
            // 如果 hhea 表不可用，使用 numGlyphs 作为默认值（虽然不准确但可作为fallback）
            numberOfHMetrics = num_glyphs;
        }
        printf("[TTF] numberOfHMetrics=%u\n", numberOfHMetrics);
        
        // 打印 hmtx_data 前20字节
        printf("[TTF] hmtx_data=%p, first 20 bytes: ", hmtx_data);
        if(hmtx_data) {
            for(int j = 0; j < 20; j++) {
                printf("%02X ", hmtx_data[j]);
            }
            printf("\n");
        } else {
            printf("NULL\n");
        }
        
        uint16_t filled = 0, ascii_filled = 0, not_found = 0, bbox_cached = 0, bbox_lazy = 0;
        static int debug_prefill_count = 0;
        for(uint16_t i = 0; i < count; i++) {
            uint32_t unicode = glyphs[i].unicode;
            uint16_t glyph_idx = glyphs[i].glyph_index;
            
            /* Determine which cache to use: ASCII or CJK */
            ttf_metrics_entry_t *entry = NULL;
            int is_ascii = (unicode >= ASCII_METRICS_START && unicode <= ASCII_METRICS_END);
            int is_cjk = (unicode >= CJK_METRICS_START && unicode <= CJK_METRICS_END);
            
            if(is_ascii) {
                if(!dsc->ascii_metrics_cache) continue; /* safety */
                uint32_t cache_idx = unicode - ASCII_METRICS_START;
                entry = &dsc->ascii_metrics_cache[cache_idx];
            } else if(is_cjk) {
                uint32_t cache_idx = unicode - CJK_METRICS_START;
                entry = &dsc->metrics_cache[cache_idx];
            } else {
                continue; /* Outside both ranges, skip */
            }
            
            if(glyph_idx == 0) {
                entry->valid = 2;
                not_found++;
                continue;
            }
            if(glyph_idx < num_glyphs) {
                uint8_t *hmtx_ptr = NULL;
                if(glyph_idx < numberOfHMetrics) {
                    hmtx_ptr = hmtx_data + glyph_idx * 4;
                } else {
                    hmtx_ptr = hmtx_data + (numberOfHMetrics - 1) * 4;
                }
                
                int16_t adv_w_raw = (int16_t)((hmtx_ptr[0] << 8) | hmtx_ptr[1]);
                entry->adv_w_raw = (uint16_t)(adv_w_raw > 0 ? adv_w_raw : 0);
                
                if(debug_prefill_count < 5) {
                    printf("[PREFILL] U+%04X glyph_idx=%u hmtx_offset=%u hmtx_bytes=%02X%02X%02X%02X adv_raw=%d\n",
                           unicode, glyph_idx, (unsigned)(hmtx_ptr - hmtx_data),
                           hmtx_ptr[0], hmtx_ptr[1], hmtx_ptr[2], hmtx_ptr[3],
                           adv_w_raw);
                    debug_prefill_count++;
                }
            }
            if(glyph_idx > 0) {
                entry->glyph_index = glyph_idx;
                if(glyphs[i].cached && glyphs[i].glyf_size >= 10) {
                    /* L1 已缓存：直接读 glyf 头 bbox，O(1) 内存访问 */
                    uint8_t *gh = glyf_data + glyphs[i].compact_offset;
                    int16_t xMin = (int16_t)((gh[2] << 8) | gh[3]);
                    int16_t yMin = (int16_t)((gh[4] << 8) | gh[5]);
                    int16_t xMax = (int16_t)((gh[6] << 8) | gh[7]);
                    int16_t yMax = (int16_t)((gh[8] << 8) | gh[9]);
                    entry->box_w_raw = (uint16_t)((xMax - xMin + 1) > 0 ? (xMax - xMin + 1) : 0);
                    entry->box_h_raw = (uint16_t)((yMax - yMin + 1) > 0 ? (yMax - yMin + 1) : 0);
                    entry->ofs_x_raw = xMin;
                    entry->ofs_y_raw = yMin;
                    bbox_cached++;
                } else {
                    /* glyf 未进 L1：仅 adv 预填，bbox 遇字时由 get_glyph_dsc stbtt 懒填 */
                    entry->box_w_raw = 0;
                    entry->box_h_raw = 0;
                    entry->ofs_x_raw = 0;
                    entry->ofs_y_raw = 0;
                    bbox_lazy++;
                }
            }
            entry->valid = 1;
            if(is_ascii) ascii_filled++;
            else filled++;
        }
        printf("[TTF] Metrics pre-filled: CJK=%u ASCII=%u not_found=%u bbox_cached=%u bbox_lazy=%u\n",
               filled, ascii_filled, not_found, bbox_cached, bbox_lazy);
        dsc->table_cache.hmtx.data = hmtx_data;
        dsc->table_cache.hmtx.file_offset = hmtx_file_offset;
        dsc->table_cache.hmtx.size = hmtx_size;
    } else {
        printf("[TTF] ERROR: hmtx not cached — layout/render will hit SD heavily\n");
    }

    // 缓存 head/hhea/OS2 等小表
    const uint8_t *ttf_mem3 = (dsc->stream.data != NULL) ? (const uint8_t *)dsc->stream.data : NULL;
    if(ttf_mem3) {
        uint32_t tbl_off, tbl_sz;
        if(find_table_location_in_memory(ttf_mem3, dsc->info.fontstart, "head", &tbl_off, &tbl_sz)) {
            dsc->table_cache.head.data = psram_malloc(tbl_sz);
            if(dsc->table_cache.head.data) { lv_memcpy(dsc->table_cache.head.data, ttf_mem3 + tbl_off, tbl_sz); dsc->table_cache.head.file_offset = tbl_off; dsc->table_cache.head.size = tbl_sz; }
        }
        if(find_table_location_in_memory(ttf_mem3, dsc->info.fontstart, "hhea", &tbl_off, &tbl_sz)) {
            dsc->table_cache.hhea.data = psram_malloc(tbl_sz);
            if(dsc->table_cache.hhea.data) { lv_memcpy(dsc->table_cache.hhea.data, ttf_mem3 + tbl_off, tbl_sz); dsc->table_cache.hhea.file_offset = tbl_off; dsc->table_cache.hhea.size = tbl_sz; }
        }
        if(find_table_location_in_memory(ttf_mem3, dsc->info.fontstart, "OS/2", &tbl_off, &tbl_sz)) {
            dsc->table_cache.os2.data = psram_malloc(tbl_sz);
            if(dsc->table_cache.os2.data) { lv_memcpy(dsc->table_cache.os2.data, ttf_mem3 + tbl_off, tbl_sz); dsc->table_cache.os2.file_offset = tbl_off; dsc->table_cache.os2.size = tbl_sz; }
        }
    } else if(dsc->file_path[0]) {
        dsc->table_cache.head.data = ttf_read_table_via_path(dsc->file_path, dsc->info.fontstart, "head",
                                                             &dsc->table_cache.head.file_offset,
                                                             &dsc->table_cache.head.size);
        dsc->table_cache.hhea.data = ttf_read_table_via_path(dsc->file_path, dsc->info.fontstart, "hhea",
                                                             &dsc->table_cache.hhea.file_offset,
                                                             &dsc->table_cache.hhea.size);
        dsc->table_cache.os2.data = ttf_read_table_via_path(dsc->file_path, dsc->info.fontstart, "OS/2",
                                                            &dsc->table_cache.os2.file_offset,
                                                            &dsc->table_cache.os2.size);
    }

    g_shared_table_cache = dsc->table_cache;
    g_shared_table_cache.glyf.data = NULL;
    printf("[TTF] pinned tables cached: loca=%luB hmtx=%luB cmap=%luB\n",
           (unsigned long)dsc->table_cache.loca.size,
           (unsigned long)dsc->table_cache.hmtx.size,
           (unsigned long)dsc->table_cache.cmap.size);
    // 全局共享：查找表已经在batch_read_glyf_data中设置好了，无需重复构建
    g_shared_level1_glyf_data = glyf_data;
    g_shared_level1_glyf_size = glyf_size;
    g_shared_level1_glyphs = glyphs;
    g_shared_level1_glyph_count = count;
    g_shared_level1_loaded = 1;
    g_shared_metrics_cache = dsc->metrics_cache;
    g_shared_ascii_metrics_cache = dsc->ascii_metrics_cache;

    /* Sort level1_glyphs by glyph_index for O(logN) binary search in ttf_get_cached_glyph_data */
    qsort(glyphs, count, sizeof(level1_glyph_info_t), compare_level1_by_glyph_index);

    printf("[TTF] Level1 glyphs loaded and sorted by glyph_index, table_cache active (cmap=%luB loca=%luB hmtx=%luB glyf=%luB)\n",
           dsc->table_cache.cmap.size, dsc->table_cache.loca.size,
           dsc->table_cache.hmtx.size, dsc->table_cache.glyf.size);
    printf("[TTF] L1 preload result: %u/%u glyphs cached in psram (%lu bytes glyf)\n",
           (unsigned)cached_glyphs, (unsigned)count, (unsigned long)glyf_size);
    ttf_verify_level1_glyphs(glyphs, count);
    return (int)cached_glyphs;
}

static uint8_t * ttf_get_cached_glyph_data(ttf_font_desc_t *dsc, uint16_t glyph_index, uint32_t *out_size)
{
    if(dsc->level1_loaded && dsc->level1_glyphs != NULL) {
        /* Binary search: level1_glyphs is sorted by glyph_index after loading */
        int lo = 0, hi = (int)dsc->level1_glyph_count - 1;
        while(lo <= hi) {
            int mid = (lo + hi) / 2;
            if(dsc->level1_glyphs[mid].glyph_index < glyph_index) {
                lo = mid + 1;
            } else if(dsc->level1_glyphs[mid].glyph_index > glyph_index) {
                hi = mid - 1;
            } else {
                if(dsc->level1_glyphs[mid].cached) {
                    *out_size = dsc->level1_glyphs[mid].glyf_size;
                    return dsc->level1_glyf_data + dsc->level1_glyphs[mid].compact_offset;
                }
                return NULL;
            }
        }
    }
    return NULL;
}

/* === 调试：跟踪 ttf_get_glyph_dsc_cb 调用来源 === */
static int g_dsc_total_calls = 0;
static int g_dsc_cache_hits = 0;
static int g_dsc_stbtt_fallback = 0;
static int g_dsc_non_cjk = 0;
static int g_dsc_phase = 0; /* 0=unknown, 1=layout, 2=render */
static uint32_t g_dsc_miss_uniques[200]; /* 记录miss的唯一unicode */
static int g_dsc_miss_unique_count = 0;

/* === DSC L2 缓存：缓存最终缩放后的 lv_font_glyph_dsc_t，避免重复 PSRAM 读取和 FP 运算 === */
#define DSC_L2_CACHE_SIZE 512
typedef struct {
    uint32_t unicode;
    const void *font_dsc_ptr;   /* font->dsc 指针，区分不同字号 */
    lv_font_glyph_dsc_t dsc_result;
    uint8_t valid;
} dsc_l2_entry_t;

static dsc_l2_entry_t g_dsc_l2_cache[DSC_L2_CACHE_SIZE];
static int g_dsc_l2_hits = 0;

void lv_tiny_ttf_reset_dsc_l2_cache(void) {
    memset(g_dsc_l2_cache, 0, sizeof(g_dsc_l2_cache));
    g_dsc_l2_hits = 0;
}

int lv_tiny_ttf_get_l2_hits(void) { return g_dsc_l2_hits; }

void lv_tiny_ttf_set_dsc_phase(int phase) { g_dsc_phase = phase; }
int lv_tiny_ttf_get_dsc_stats(int *total, int *hits, int *misses, int *non_cjk) {
    if(total) *total = g_dsc_total_calls;
    if(hits) *hits = g_dsc_cache_hits;
    if(misses) *misses = g_dsc_stbtt_fallback;
    if(non_cjk) *non_cjk = g_dsc_non_cjk;
    return g_dsc_miss_unique_count;
}
void lv_tiny_ttf_reset_dsc_stats(void) {
    g_dsc_total_calls = 0;
    g_dsc_cache_hits = 0;
    g_dsc_stbtt_fallback = 0;
    g_dsc_non_cjk = 0;
    g_dsc_miss_unique_count = 0;
    memset(g_dsc_miss_uniques, 0, sizeof(g_dsc_miss_uniques));
}

static bool ttf_get_glyph_dsc_cb(const lv_font_t * font, lv_font_glyph_dsc_t * dsc_out, uint32_t unicode_letter,
                                 uint32_t unicode_letter_next)
{
    if(unicode_letter < 0x20 ||
       unicode_letter == 0xf8ff ||
       unicode_letter == 0x200c) {
        dsc_out->box_w = 0;
        dsc_out->adv_w = 0;
        dsc_out->box_h = 0;
        dsc_out->ofs_x = 0;
        dsc_out->ofs_y = 0;
        dsc_out->bpp = 0;
        dsc_out->is_placeholder = false;
        return true;
    }

    g_dsc_total_calls++;

    ttf_font_desc_t * dsc = (ttf_font_desc_t *)font->dsc;

    /* === DSC L2 cache lookup: avoid repeated PSRAM reads + FP math === */
    {
        uint32_t l2_hash = (unicode_letter ^ ((uint32_t)dsc >> 3)) % DSC_L2_CACHE_SIZE;
        dsc_l2_entry_t *e = &g_dsc_l2_cache[l2_hash];
        if(e->valid && e->unicode == unicode_letter && e->font_dsc_ptr == (const void *)dsc &&
           e->dsc_result.box_w > 0 && e->dsc_result.box_h > 0) {
            g_dsc_l2_hits++;
            *dsc_out = e->dsc_result;
            return true;
        }
    }

    /* ASCII fast path: use pre-filled ascii_metrics_cache (zero IO) */
    if(unicode_letter >= ASCII_METRICS_START && unicode_letter <= ASCII_METRICS_END) {
        ttf_metrics_entry_t *amc = dsc->ascii_metrics_cache ? dsc->ascii_metrics_cache : g_shared_ascii_metrics_cache;
        if(amc) {
            uint32_t aidx = unicode_letter - ASCII_METRICS_START;
            if(amc[aidx].valid == 1 &&
               (amc[aidx].box_w_raw > 0 || amc[aidx].box_h_raw > 0)) {
                g_dsc_cache_hits++;
                dsc_out->adv_w = (uint16_t)(amc[aidx].adv_w_raw * dsc->scale);
                int raw_x0 = amc[aidx].ofs_x_raw;
                int raw_x1 = raw_x0 + amc[aidx].box_w_raw - 1;
                int raw_y0 = amc[aidx].ofs_y_raw;
                int raw_y1 = raw_y0 + amc[aidx].box_h_raw - 1;
                float s = dsc->scale;
                int rx1 = STBTT_ifloor(raw_x0 * s);
                int rx2 = STBTT_iceil(raw_x1 * s);
                int ry1 = STBTT_ifloor(-raw_y1 * s);
                int ry2 = STBTT_iceil(-raw_y0 * s);
                dsc_out->box_w = (uint16_t)(rx2 - rx1 + 1);
                dsc_out->box_h = (uint16_t)(ry2 - ry1 + 1);
                dsc_out->ofs_x = (int16_t)rx1;
                dsc_out->ofs_y = (int16_t)(-ry2);
                dsc_out->bpp = 8;
                dsc_out->is_placeholder = false;
                /* Store to L2 cache */
                { uint32_t _h = (unicode_letter ^ ((uint32_t)dsc >> 3)) % DSC_L2_CACHE_SIZE;
                  g_dsc_l2_cache[_h].unicode = unicode_letter; g_dsc_l2_cache[_h].font_dsc_ptr = (const void *)dsc;
                  g_dsc_l2_cache[_h].dsc_result = *dsc_out; g_dsc_l2_cache[_h].valid = 1; }
                return true;
            }
            if(amc[aidx].valid == 2) return false;
        }
        /* ASCII not in cache, fall through to stbtt with PSRAM cache */
    }

    if(unicode_letter >= 0xFF00 && unicode_letter <= 0xFFEF) {
        return false;
    }

    if(unicode_letter >= CJK_METRICS_START && unicode_letter <= CJK_METRICS_END) {
        uint32_t idx = unicode_letter - CJK_METRICS_START;

        // 所有字体共享同一份 metrics_cache，直接查（bbox 为 0 表示 glyf 未进 L1，走 stbtt）
        if(dsc->metrics_cache && dsc->metrics_cache[idx].valid == 1 &&
           (dsc->metrics_cache[idx].box_w_raw > 0 || dsc->metrics_cache[idx].box_h_raw > 0)) {
            g_dsc_cache_hits++;
            dsc_out->adv_w = (uint16_t)(dsc->metrics_cache[idx].adv_w_raw * dsc->scale);
            
            // 从存储的原始值直接用几何运算计算缩放后尺寸（0 PSRAM 访问）
            int raw_x0 = dsc->metrics_cache[idx].ofs_x_raw;
            int raw_x1 = raw_x0 + dsc->metrics_cache[idx].box_w_raw - 1;
            int raw_y0 = dsc->metrics_cache[idx].ofs_y_raw;
            int raw_y1 = raw_y0 + dsc->metrics_cache[idx].box_h_raw - 1;

            float s = dsc->scale;
            int rx1 = STBTT_ifloor(raw_x0 * s);
            int rx2 = STBTT_iceil(raw_x1 * s);
            int ry1 = STBTT_ifloor(-raw_y1 * s);
            int ry2 = STBTT_iceil(-raw_y0 * s);

            dsc_out->box_w = (uint16_t)(rx2 - rx1 + 1);
            dsc_out->box_h = (uint16_t)(ry2 - ry1 + 1);
            dsc_out->ofs_x = (int16_t)rx1;
            dsc_out->ofs_y = (int16_t)(-ry2);
            dsc_out->bpp = 8;
            dsc_out->is_placeholder = false;
            /* Store to L2 cache */
            { uint32_t _h = (unicode_letter ^ ((uint32_t)dsc >> 3)) % DSC_L2_CACHE_SIZE;
              g_dsc_l2_cache[_h].unicode = unicode_letter; g_dsc_l2_cache[_h].font_dsc_ptr = (const void *)dsc;
              g_dsc_l2_cache[_h].dsc_result = *dsc_out; g_dsc_l2_cache[_h].valid = 1; }
            
#if TTF_DEBUG_METRICS_VERBOSE
            static int debug_cjk_count = 0;
            if(debug_cjk_count < 5) {
                printf("[CJK_METRICS] U+%04X scale=%.4f adv_raw=%u adv=%u box=%ux%u ofs=%d,%d\n",
                       unicode_letter, dsc->scale,
                       dsc->metrics_cache[idx].adv_w_raw, dsc_out->adv_w,
                       dsc_out->box_w, dsc_out->box_h,
                       dsc_out->ofs_x, dsc_out->ofs_y);
                debug_cjk_count++;
            }
#endif
            return true;
        }
        if(dsc->metrics_cache && dsc->metrics_cache[idx].valid == 2) {
            return false;
        }
    }

    // 未命中缓存，后备：stbtt 从 PSRAM 数据渲染
    g_dsc_stbtt_fallback++;
    uint32_t st0 = xTaskGetTickCount();

    /* 记录非Level1的CJK字符（唯一unicode列表 + 每100个打一次） */
    if(unicode_letter >= CJK_METRICS_START && unicode_letter <= CJK_METRICS_END) {
        /* 追踪唯一miss */
        int found = 0;
        for(int j = 0; j < g_dsc_miss_unique_count; j++) {
            if(g_dsc_miss_uniques[j] == unicode_letter) { found = 1; break; }
        }
        if(!found && g_dsc_miss_unique_count < 200) {
            g_dsc_miss_uniques[g_dsc_miss_unique_count++] = unicode_letter;
        }
        static int miss_log_count = 0;
        miss_log_count++;
        if(miss_log_count <= 20 || (miss_log_count % 100) == 0) {
            uint8_t utf8[4] = {0};
            utf8[0] = 0xE0 | ((unicode_letter >> 12) & 0x0F);
            utf8[1] = 0x80 | ((unicode_letter >> 6) & 0x3F);
            utf8[2] = 0x80 | (unicode_letter & 0x3F);
            printf("[MISS_L1] #%d U+%04X (%s) phase=%d unique_miss=%d\n",
                   miss_log_count, unicode_letter, utf8, g_dsc_phase, g_dsc_miss_unique_count);
        }
    }

    int g1 = stbtt_FindGlyphIndex(&dsc->info, (int)unicode_letter);
    if(g1 == 0) {
        if(unicode_letter >= CJK_METRICS_START && unicode_letter <= CJK_METRICS_END) {
            ttf_metrics_entry_t *cache_ptr = dsc->metrics_cache ? dsc->metrics_cache : g_shared_metrics_cache;
            if(cache_ptr) cache_ptr[unicode_letter - CJK_METRICS_START].valid = 2;
        }
        return false;
    }
    int x1, y1, x2, y2;
    stbtt_GetGlyphBitmapBox(&dsc->info, g1, dsc->scale, dsc->scale, &x1, &y1, &x2, &y2);
    int g2 = 0;
    if(unicode_letter_next != 0) {
        g2 = stbtt_FindGlyphIndex(&dsc->info, (int)unicode_letter_next);
    }
    int advw, lsb;
    stbtt_GetGlyphHMetrics(&dsc->info, g1, &advw, &lsb);
    int k = stbtt_GetGlyphKernAdvance(&dsc->info, g1, g2);
    dsc_out->adv_w = (uint16_t)((advw + k) * dsc->scale);
    dsc_out->box_w = x2 - x1 + 1;
    dsc_out->box_h = y2 - y1 + 1;
    dsc_out->ofs_x = x1;
    dsc_out->ofs_y = -y2;
    dsc_out->bpp = 8;
    dsc_out->is_placeholder = false;

    // 填充缓存（存储未缩放的 raw 值，与预填充路径一致）
    {
        int rx0, ry0, rx1, ry1;
        stbtt_GetGlyphBitmapBox(&dsc->info, g1, 1.0f, 1.0f, &rx0, &ry0, &rx1, &ry1);
        if(unicode_letter >= CJK_METRICS_START && unicode_letter <= CJK_METRICS_END) {
            ttf_metrics_entry_t *cache_ptr = dsc->metrics_cache ? dsc->metrics_cache : g_shared_metrics_cache;
            if(cache_ptr) {
                uint32_t idx = unicode_letter - CJK_METRICS_START;
                cache_ptr[idx].adv_w_raw = (uint16_t)(advw + k);
                cache_ptr[idx].box_w_raw = (uint16_t)((rx1 - rx0 + 1) > 0 ? (uint16_t)(rx1 - rx0 + 1) : 0);
                cache_ptr[idx].box_h_raw = (uint16_t)((ry1 - ry0 + 1) > 0 ? (uint16_t)(ry1 - ry0 + 1) : 0);
                cache_ptr[idx].ofs_x_raw = rx0;
                cache_ptr[idx].ofs_y_raw = (int16_t)(-ry1);
                cache_ptr[idx].glyph_index = (uint16_t)g1;
                cache_ptr[idx].valid = 1;
            }
        } else if(unicode_letter >= ASCII_METRICS_START && unicode_letter <= ASCII_METRICS_END) {
            ttf_metrics_entry_t *amc = dsc->ascii_metrics_cache ? dsc->ascii_metrics_cache : g_shared_ascii_metrics_cache;
            if(amc) {
                uint32_t aidx = unicode_letter - ASCII_METRICS_START;
                amc[aidx].adv_w_raw = (uint16_t)(advw + k);
                amc[aidx].box_w_raw = (uint16_t)((rx1 - rx0 + 1) > 0 ? (uint16_t)(rx1 - rx0 + 1) : 0);
                amc[aidx].box_h_raw = (uint16_t)((ry1 - ry0 + 1) > 0 ? (uint16_t)(ry1 - ry0 + 1) : 0);
                amc[aidx].ofs_x_raw = rx0;
                amc[aidx].ofs_y_raw = (int16_t)(-ry1);
                amc[aidx].glyph_index = (uint16_t)g1;
                amc[aidx].valid = 1;
            }
        }
    }

    /* Store to L2 cache */
    { uint32_t _h = (unicode_letter ^ ((uint32_t)dsc >> 3)) % DSC_L2_CACHE_SIZE;
      g_dsc_l2_cache[_h].unicode = unicode_letter; g_dsc_l2_cache[_h].font_dsc_ptr = (const void *)dsc;
      g_dsc_l2_cache[_h].dsc_result = *dsc_out; g_dsc_l2_cache[_h].valid = 1; }

    return true;
}

// === EPD gamma correction LUT for 1-bit EPD threshold fix ===
// ROOT CAUSE: LVGL lv_color_mix() for LV_COLOR_DEPTH==1 uses hard threshold:
//   ret.full = mix > LV_OPA_50(127) ? c1.full : c2.full;
// Old gamma=0.5 was INSUFFICIENT: val=2→23, val=50→113, val=113→170
//   val=113 still below 127 → pixel becomes WHITE (invisible on EPD!)
// FIX: floor=128 ensures ANY non-zero pixel survives the 127 threshold:
//   output = 128 + sqrt(i/255) * 127
//   val=1  → 128+8=136  (survives!)
//   val=50 → 128+56=184 (survives!)
//   val=255→ 128+127=255
static uint8_t epd_gamma_lut[256];
static int epd_gamma_lut_ready = 0;

static void epd_gamma_lut_init(void)
{
    if(epd_gamma_lut_ready) return;
    for(int i = 0; i < 256; i++) {
        if(i == 0) { epd_gamma_lut[i] = 0; continue; }
        /* floor=128 + sqrt curve: ensures every non-zero pixel > 127 threshold */
        int r = 128 + (int)(sqrt((double)i / 255.0) * 127.0);
        epd_gamma_lut[i] = (r > 255) ? 255 : (uint8_t)r;
    }
    epd_gamma_lut_ready = 1;
    printf("[EPD_GAMMA] LUT initialized (floor=128, ensures non-zero pixels survive 1-bit threshold)\n");
}

// === Multi-slot bitmap cache ===
#define BITMAP_CACHE_SLOTS 512
typedef struct {
    uint32_t unicode;
    const void * font_dsc;  /* font->dsc to distinguish different font instances/sizes */
    uint8_t *bitmap;
    uint32_t szb;
    uint32_t tick;
} bitmap_cache_slot_t;

static bitmap_cache_slot_t g_bm_cache[BITMAP_CACHE_SLOTS];
static uint32_t g_bm_cache_tick = 0;
static int g_bm_cache_hits = 0;
static int g_bm_cache_misses = 0;
static int g_bm_total_render_ms = 0;
static int g_bm_page_render_start = 0;

void lv_tiny_ttf_bitmap_cache_reset(void)
{
    for(int i = 0; i < BITMAP_CACHE_SLOTS; i++) {
        if(g_bm_cache[i].bitmap) {
            psram_free(g_bm_cache[i].bitmap);
            g_bm_cache[i].bitmap = NULL;
        }
        g_bm_cache[i].unicode = 0;
        g_bm_cache[i].font_dsc = NULL;
        g_bm_cache[i].szb = 0;
        g_bm_cache[i].tick = 0;
    }
    g_bm_cache_tick = 0;
    g_bm_cache_hits = 0;
    g_bm_cache_misses = 0;
    g_bm_total_render_ms = 0;
    printf("[BM_CACHE] Reset, all slots cleared\n");
}

void lv_tiny_ttf_bitmap_page_start(void)
{
    g_bm_page_render_start = (int)xTaskGetTickCount();
    g_bm_cache_hits = 0;
    g_bm_cache_misses = 0;
    g_bm_total_render_ms = 0;
}

void lv_tiny_ttf_bitmap_page_end(void)
{
    int total_ms = (int)xTaskGetTickCount() - g_bm_page_render_start;
    int used_slots = 0;
    for(int i = 0; i < BITMAP_CACHE_SLOTS; i++) {
        if(g_bm_cache[i].bitmap) used_slots++;
    }
    printf("[BM_PAGE] total=%dms render=%dms hits=%d misses=%d used_slots=%d/%d\n",
           total_ms, g_bm_total_render_ms, g_bm_cache_hits, g_bm_cache_misses,
           used_slots, BITMAP_CACHE_SLOTS);
}

static const uint8_t s_space_bitmap[1] = { 0 };

static const uint8_t * ttf_get_glyph_bitmap_cb(const lv_font_t * font, uint32_t unicode_letter)
{
    ttf_font_desc_t * dsc = (ttf_font_desc_t *)font->dsc;
    if(unicode_letter == 0x0020) {
        return s_space_bitmap;
    }
    if(unicode_letter >= 0xFF00 && unicode_letter <= 0xFFEF) {
        return NULL;
    }
    if(unicode_letter >= CJK_METRICS_START && unicode_letter <= CJK_METRICS_END && g_shared_level1_loaded) {
        uint32_t idx = unicode_letter - CJK_METRICS_START;
        int valid = 0;
        if(dsc->metrics_cache && dsc->metrics_cache[idx].valid == 1) valid = 1;
        if(!valid && g_shared_metrics_cache && g_shared_metrics_cache[idx].valid == 1) valid = 1;
        if(!valid) {
            return NULL; /* glyph not in level1 cache */
        }
    }
    /* ASCII: check ascii_metrics_cache validity */
    if(unicode_letter >= ASCII_METRICS_START && unicode_letter <= ASCII_METRICS_END) {
        ttf_metrics_entry_t *amc = dsc->ascii_metrics_cache ? dsc->ascii_metrics_cache : g_shared_ascii_metrics_cache;
        if(amc && amc[unicode_letter - ASCII_METRICS_START].valid == 2) {
            return NULL; /* glyph not in font */
        }
        /* valid==1: proceed to render via stbtt+PSRAM glyf cache */
    }

    // === Multi-slot bitmap cache lookup (key = unicode + font_dsc) ===
    g_bm_cache_tick++;
    const void * my_dsc = (const void *)dsc;
    uint32_t hash = (unicode_letter ^ ((uint32_t)my_dsc >> 4)) % BITMAP_CACHE_SLOTS;
    // Open addressing: check slot and next few slots
    for(int probe = 0; probe < 4; probe++) {
        uint32_t slot = (hash + probe) % BITMAP_CACHE_SLOTS;
        if(g_bm_cache[slot].unicode == unicode_letter && g_bm_cache[slot].font_dsc == my_dsc && g_bm_cache[slot].bitmap) {
            g_bm_cache[slot].tick = g_bm_cache_tick;
            g_bm_cache_hits++;
#if TTF_DEBUG_CACHE_HIT_VERBOSE
            /* Print full FB_AREA for specific chars even on cache hit */
            if(unicode_letter == 0x4EBA || unicode_letter == 0x7684) {
                printf("[FB_CACHE_HIT] U+%04X sz=%u scale=%.4f dsc=%p\n", 
                       unicode_letter, g_bm_cache[slot].szb, dsc->scale, my_dsc);
                /* Estimate w/h from bitmap size (sqrt approx for square glyphs) */
                int cw = (dsc->scale > 0.018f) ? 22 : 16;
                int ch = cw;
                printf("[FB_AREA] U+%04X %dx%d (cached):\n", unicode_letter, cw, ch);
                for(int row = 0; row < ch && row * cw < (int)g_bm_cache[slot].szb; row++) {
                    printf("[FB] ");
                    for(int col = 0; col < cw && row * cw + col < (int)g_bm_cache[slot].szb; col++) {
                        uint8_t val = g_bm_cache[slot].bitmap[row * cw + col];
                        if(val == 0) printf(".");
                        else if(val < 64) printf("-");
                        else if(val < 128) printf("+");
                        else if(val < 200) printf("#");
                        else printf("@");
                    }
                    printf("\n");
                }
            }
#endif
            return g_bm_cache[slot].bitmap;
        }
        if(g_bm_cache[slot].bitmap == NULL) break;
    }

    // Cache miss - need to render
    g_bm_cache_misses++;
    int t0 = (int)xTaskGetTickCount();

    int g1 = stbtt_FindGlyphIndex(&dsc->info, (int)unicode_letter);
    if(g1 == 0) {
        return NULL;
    }
    int x1, y1, x2, y2;
    stbtt_GetGlyphBitmapBox(&dsc->info, g1, dsc->scale, dsc->scale, &x1, &y1, &x2, &y2);
    int w, h;
    w = x2 - x1 + 1;
    h = y2 - y1 + 1;
    uint32_t stride = w;

    size_t szb = h * stride;
    uint8_t * buffer = psram_malloc(szb);
    if(!buffer) {
        LV_LOG_ERROR("failed to allocate bitmap buffer");
        return NULL;
    }
    lv_memset(buffer, 0, szb);
    stbtt_MakeGlyphBitmap(&dsc->info, buffer, w, h, stride, dsc->scale, dsc->scale, g1);

    /* EPD gamma correction: boost low anti-aliasing values so thin strokes
     * survive the final 1-bit binarization on e-paper display */
    epd_gamma_lut_init();
    for(uint32_t gi = 0; gi < szb; gi++) {
        buffer[gi] = epd_gamma_lut[buffer[gi]];
    }

    int render_ms = (int)xTaskGetTickCount() - t0;
    g_bm_total_render_ms += render_ms;

    // Debug: print slow renders and every 50th miss
    static int miss_log_count = 0;
    miss_log_count++;
    if(render_ms > 20 || miss_log_count <= 10 || (miss_log_count % 50) == 0) {
        printf("[BM_RENDER] #%d U+%04X g=%d %dx%d sz=%u render=%dms total_render=%dms\n",
               miss_log_count, unicode_letter, g1, w, h, (unsigned)szb, render_ms, g_bm_total_render_ms);
    }

    // Find a slot to store (evict LRU if needed)
    uint32_t best_slot = hash % BITMAP_CACHE_SLOTS;
    uint32_t oldest_tick = g_bm_cache[hash % BITMAP_CACHE_SLOTS].tick;
    for(int probe = 0; probe < 4; probe++) {
        uint32_t slot = (hash + probe) % BITMAP_CACHE_SLOTS;
        if(g_bm_cache[slot].bitmap == NULL) {
            best_slot = slot;
            break;
        }
        if(g_bm_cache[slot].tick < oldest_tick) {
            oldest_tick = g_bm_cache[slot].tick;
            best_slot = slot;
        }
    }

    // Evict old entry if needed
    if(g_bm_cache[best_slot].bitmap) {
        psram_free(g_bm_cache[best_slot].bitmap);
    }
    g_bm_cache[best_slot].unicode = unicode_letter;
    g_bm_cache[best_slot].font_dsc = my_dsc;
    g_bm_cache[best_slot].bitmap = buffer;
    g_bm_cache[best_slot].szb = (uint32_t)szb;
    g_bm_cache[best_slot].tick = g_bm_cache_tick;
    
#if TTF_DEBUG_BITMAP_VERBOSE
    /* ONLY capture specific chars for debugging: 人=0x4EBA, 的=0x7684 */
    int dbg_this = (unicode_letter == 0x4EBA || unicode_letter == 0x7684) ? 1 : 0;
    if(dbg_this) {
        int non_zero = 0;
        for(uint32_t i = 0; i < szb; i++) if(buffer[i]) non_zero++;
        printf("[GLYPH_BITMAP] U+%04X w=%d h=%d sz=%u non_zero=%d\n",
               unicode_letter, w, h, (unsigned)szb, non_zero);
        printf("[GLYPH_HEX] ");
        for(int i = 0; i < 64 && i < (int)szb; i++) printf("%02X ", buffer[i]);
        printf("\n");
        /* 2D bitmap visualization: print each row as ASCII art */
        printf("[FB_AREA] U+%04X %dx%d:\n", unicode_letter, w, h);
        for(int row = 0; row < h; row++) {
            printf("[FB] ");
            for(int col = 0; col < w; col++) {
                uint8_t val = buffer[row * w + col];
                if(val == 0) printf(".");
                else if(val < 64) printf("-");
                else if(val < 128) printf("+");
                else if(val < 200) printf("#");
                else printf("@");
            }
            printf("\n");
        }
    }
#endif

#if TTF_DEBUG_MISMATCH_VERBOSE
    static int debug_mismatch_count = 0;
    if(debug_mismatch_count < 10 && unicode_letter >= CJK_METRICS_START) {
        uint32_t idx = unicode_letter - CJK_METRICS_START;
        ttf_metrics_entry_t *cache_ptr = dsc->metrics_cache ? dsc->metrics_cache : g_shared_metrics_cache;
        if(cache_ptr && cache_ptr[idx].valid == 1) {
            int raw_x0 = cache_ptr[idx].ofs_x_raw;
            int raw_x1 = raw_x0 + cache_ptr[idx].box_w_raw - 1;
            int raw_y0 = cache_ptr[idx].ofs_y_raw;
            int raw_y1 = raw_y0 + cache_ptr[idx].box_h_raw - 1;
            float s = dsc->scale;
            int exp_x1 = STBTT_ifloor(raw_x0 * s);
            int exp_x2 = STBTT_iceil(raw_x1 * s);
            int exp_y1 = STBTT_ifloor(-raw_y1 * s);
            int exp_y2 = STBTT_iceil(-raw_y0 * s);
            int exp_w = exp_x2 - exp_x1 + 1;
            int exp_h = exp_y2 - exp_y1 + 1;
            if(exp_w != w || exp_h != h || exp_x1 != x1 || exp_y1 != y1 || exp_y2 != y2) {
                printf("[MISMATCH] U+%04X exp=(%dx%d)%+d,%d got=(%dx%d)%+d,%d raw_ofs=%d box=%dx%d\n",
                       unicode_letter, exp_w, exp_h, exp_x1, exp_y1, w, h, x1, y1,
                       raw_x0, raw_x1, raw_y0, raw_y1);
                debug_mismatch_count++;
            }
        }
    }
#endif
    
    return buffer;
}

static void ttf_set_font_size_cb(lv_font_t * font, lv_coord_t line_height)
{
    ttf_font_desc_t * dsc = (ttf_font_desc_t *)font->dsc;
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&dsc->info, &ascent, &descent, &line_gap);
    /* Use EM-based scaling instead of (ascent-descent) to match built-in font pixel size.
     * For AlibabaPuHuiTi: EM=1000 vs metrics_span=1400,
     * EM-based: scale=16/1000=0.016 (CJK ~16px) vs old: 16/1400=0.01143 (CJK ~11px) */
    dsc->scale = stbtt_ScaleForMappingEmToPixels(&dsc->info, (float)line_height);
    dsc->ascent = (int)(ascent * dsc->scale);
    dsc->descent = (int)(-descent * dsc->scale);
    font->base_line = dsc->descent;
    font->line_height = line_height;
    font->get_glyph_dsc = ttf_get_glyph_dsc_cb;
    font->get_glyph_bitmap = ttf_get_glyph_bitmap_cb;
    
    static int debug_size_count = 0;
    if(debug_size_count < 2) {
        printf("[SET_SIZE] font=%p line_height=%d ascent=%d descent=%d line_gap=%d scale=%.6f\n",
               (void*)font, line_height, ascent, descent, line_gap, dsc->scale);
        debug_size_count++;
    }
}

lv_font_t * lv_tiny_ttf_create(const char * path, const void * data, size_t data_size, lv_coord_t font_size,
                                      size_t cache_size_unused)
{
    LV_UNUSED(cache_size_unused);
    
    if((path == NULL && data == NULL) || 0 >= font_size) {
        LV_LOG_ERROR("tiny_ttf: invalid argument\n");
        return NULL;
    }
    ttf_font_desc_t * dsc = (ttf_font_desc_t *)TTF_MALLOC(sizeof(ttf_font_desc_t));
    if(dsc == NULL) {
        LV_LOG_ERROR("tiny_ttf: out of memory\n");
        return NULL;
    }
    lv_memset(dsc, 0, sizeof(ttf_font_desc_t));
    
#if LV_TINY_TTF_FILE_SUPPORT
    if(path != NULL) {
        strncpy(dsc->file_path, path, sizeof(dsc->file_path) - 1);
        if(LV_FS_RES_OK != lv_fs_open(&dsc->file, path, LV_FS_MODE_RD)) {
            LV_LOG_ERROR("tiny_ttf: unable to open %s\n", path);
            goto err_after_dsc;
        }
        dsc->stream.file = &dsc->file;
        dsc->stream.position = 0;
        dsc->stream.dsc = dsc;
        printf("[DSC_INIT] dsc=%p &file=%p &stream=%p stream.file=%p stream.dsc=%p sizeof(desc)=%zu\n",
               (void*)dsc, (void*)&dsc->file, (void*)&dsc->stream,
               (void*)dsc->stream.file, (void*)dsc->stream.dsc,
               sizeof(ttf_font_desc_t));
    }
    else {
        dsc->stream.file = NULL;
        dsc->stream.data = (const uint8_t *)data;
        dsc->stream.size = data_size;
        dsc->stream.position = 0;
        dsc->stream.dsc = dsc;
    }
    if(0 == stbtt_InitFont(&dsc->info, &dsc->stream, stbtt_GetFontOffsetForIndex(&dsc->stream, 0))) {
        LV_LOG_ERROR("tiny_ttf: init failed\n");
        goto err_after_dsc;
    }
#else
    dsc->stream = (const uint8_t *)data;
    LV_UNUSED(data_size);
    if(0 == stbtt_InitFont(&dsc->info, dsc->stream, stbtt_GetFontOffsetForIndex(dsc->stream, 0))) {
        LV_LOG_ERROR("tiny_ttf: init failed\n");
        goto err_after_dsc;
    }
#endif

    if(g_shared_metrics_cache) {
        dsc->metrics_cache = g_shared_metrics_cache;
    }
    /* metrics_cache 推迟到 Level1 glyf 预加载之后分配，为 ~1.5MB glyf 块腾出 psram */

    if(g_shared_ascii_metrics_cache) {
        dsc->ascii_metrics_cache = g_shared_ascii_metrics_cache;
    }

    if(g_shared_level1_loaded) {
        dsc->table_cache = g_shared_table_cache;
        dsc->level1_loaded = 1;
        dsc->level1_glyphs = g_shared_level1_glyphs;
        dsc->level1_glyph_count = g_shared_level1_glyph_count;
        dsc->level1_glyf_data = g_shared_level1_glyf_data;
        dsc->level1_glyf_total_size = g_shared_level1_glyf_size;
        printf("[TTF] Reusing shared Level1 cache: glyphs=%u table_cache.active=%d\n",
               g_shared_level1_glyph_count, g_shared_table_cache.active);
    }

    lv_font_t * out_font = (lv_font_t *)TTF_MALLOC(sizeof(lv_font_t));
    if(out_font == NULL) {
        LV_LOG_ERROR("tiny_ttf: out of memory\n");
        goto err_after_dsc;
    }
    lv_memset(out_font, 0, sizeof(lv_font_t));
    out_font->dsc = dsc;
    out_font->fallback = NULL;
    lv_tiny_ttf_set_size(out_font, font_size);
    return out_font;
err_after_dsc:
    TTF_FREE(dsc);
    return NULL;
}

lv_font_t * lv_tiny_ttf_create_file_ex(const char * path, lv_coord_t font_size)
{
    return lv_tiny_ttf_create(path, NULL, 0, font_size, 0);
}

lv_font_t * lv_tiny_ttf_create_file(const char * path, lv_coord_t font_size)
{
    return lv_tiny_ttf_create_file_ex(path, font_size);
}

lv_font_t * lv_tiny_ttf_create_data_ex(const void * data, size_t data_size, lv_coord_t font_size, size_t cache_size_unused)
{
    return lv_tiny_ttf_create(NULL, data, data_size, font_size, 0);
}

lv_font_t * lv_tiny_ttf_create_data(const void * data, size_t data_size, lv_coord_t font_size)
{
    return lv_tiny_ttf_create_data_ex(data, data_size, font_size, 0);
}

void lv_tiny_ttf_set_size(lv_font_t * font, lv_coord_t line_height)
{
    if(font != NULL && font->dsc != NULL) {
        ttf_set_font_size_cb(font, line_height);
    }
}

void lv_tiny_ttf_release_reader_cache(void)
{
    if(g_shared_level1_glyf_data) {
        psram_free(g_shared_level1_glyf_data);
        g_shared_level1_glyf_data = NULL;
        g_shared_level1_glyf_size = 0;
    }
    if(g_shared_level1_glyphs) {
        ttf_free_level1_glyphs(g_shared_level1_glyphs);
        g_shared_level1_glyphs = NULL;
        g_shared_level1_glyph_count = 0;
        g_level1_glyphs_is_dma = 0;
    }
    if(g_glyf_lookup) {
        ttf_meta_free(g_glyf_lookup, g_glyf_lookup_is_dma);
        g_glyf_lookup = NULL;
        g_glyf_lookup_count = 0;
        g_glyf_lookup_is_dma = 0;
    }
    if(g_shared_metrics_cache) {
        ttf_meta_free(g_shared_metrics_cache, g_metrics_cache_is_dma);
        g_shared_metrics_cache = NULL;
        g_metrics_cache_is_dma = 0;
    }
    if(g_shared_ascii_metrics_cache) {
        ttf_meta_free(g_shared_ascii_metrics_cache, g_ascii_metrics_cache_is_dma);
        g_shared_ascii_metrics_cache = NULL;
        g_ascii_metrics_cache_is_dma = 0;
    }
    if(g_shared_level1_loaded) {
        ttf_free_table_cache(&g_shared_table_cache);
        memset(&g_shared_table_cache, 0, sizeof(g_shared_table_cache));
        g_shared_level1_loaded = 0;
    }
    lv_tiny_ttf_bitmap_cache_reset();
    lv_tiny_ttf_reset_dsc_l2_cache();
    lv_tiny_ttf_reset_dsc_stats();
    printf("[TTF] Reader shared cache released, psram_heap free=%lu\n",
           (unsigned long)psram_GetFreeHeapSize());
}

int lv_tiny_ttf_level1_ready(void)
{
    return g_shared_level1_loaded ? 1 : 0;
}

void lv_tiny_ttf_destroy(lv_font_t * font)
{
    if(font != NULL) {
        if(font->dsc != NULL) {
            ttf_font_desc_t * dsc = (ttf_font_desc_t *)font->dsc;
#if LV_TINY_TTF_FILE_SUPPORT
            if(dsc->stream.file != NULL) {
                lv_fs_close(&dsc->file);
            }
#endif
            /* Don't free shared caches - they persist across font size changes.
             * Only detach pointers so the new font can reuse them. */
            if(dsc->metrics_cache) {
                if(dsc->metrics_cache != g_shared_metrics_cache) {
                    ttf_meta_free(dsc->metrics_cache, g_metrics_cache_is_dma);
                }
                dsc->metrics_cache = NULL;
            }
            if(dsc->ascii_metrics_cache) {
                if(dsc->ascii_metrics_cache != g_shared_ascii_metrics_cache) {
                    ttf_meta_free(dsc->ascii_metrics_cache, g_ascii_metrics_cache_is_dma);
                }
                dsc->ascii_metrics_cache = NULL;
            }
            if(dsc->level1_glyphs) {
                if(dsc->level1_glyphs != g_shared_level1_glyphs) {
                    ttf_free_level1_glyphs(dsc->level1_glyphs);
                }
                dsc->level1_glyphs = NULL;
            }
            if(dsc->level1_glyf_data) {
                if(dsc->level1_glyf_data != g_shared_level1_glyf_data) {
                    psram_free(dsc->level1_glyf_data);
                }
                dsc->level1_glyf_data = NULL;
            }
            /* Clear bitmap cache on font destroy (glyphs rendered for old size are invalid) */
            lv_tiny_ttf_bitmap_cache_reset();
            TTF_FREE(dsc);
        }
        TTF_FREE(font);
    }
}

int lv_tiny_ttf_load_level1_glyphs(lv_font_t *font, const uint32_t *unicode_list, uint16_t count)
{
    if(font == NULL || font->dsc == NULL) {
        return -1;
    }
    ttf_font_desc_t *dsc = (ttf_font_desc_t *)font->dsc;
    return ttf_load_level1_glyphs(dsc, unicode_list, count);
}

uint8_t * lv_tiny_ttf_get_cached_glyph_data(lv_font_t *font, uint16_t glyph_index, uint32_t *out_size)
{
    if(font == NULL || font->dsc == NULL) {
        return NULL;
    }
    ttf_font_desc_t *dsc = (ttf_font_desc_t *)font->dsc;
    return ttf_get_cached_glyph_data(dsc, glyph_index, out_size);
}

#endif
