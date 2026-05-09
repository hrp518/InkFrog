#include "lv_tiny_ttf.h"

#if LV_USE_TINY_TTF
#include <stdio.h>
#include <string.h>
#include "../../../misc/lv_lru.h"
#include "sys/sys_heap.h"
#include <sys/dma_heap.h>
#define STB_RECT_PACK_IMPLEMENTATION
#define STBRP_STATIC
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_HEAP_FACTOR_SIZE_32 50
#define STBTT_HEAP_FACTOR_SIZE_128 20
#define STBTT_HEAP_FACTOR_SIZE_DEFAULT 10
#define STBTT_malloc(x, u) ((void)(u), _dma_malloc(x, DMAHEAP_PSRAM))
#define STBTT_free(x, u) ((void)(u), _dma_free(x, DMAHEAP_PSRAM))
#define TTF_MALLOC(x) (_dma_malloc(x, DMAHEAP_PSRAM))
#define TTF_FREE(x) (_dma_free(x, DMAHEAP_PSRAM))

#define CJK_METRICS_START 0x4E00u
#define CJK_METRICS_END   0x9FFFu
#define CJK_METRICS_COUNT (CJK_METRICS_END - CJK_METRICS_START + 1u)

#define LEVEL1_GLYPH_CACHE_SIZE 3500

typedef struct {
    uint16_t adv_w_raw;   // unscaled advance width from hmtx
    uint16_t box_w_raw;   // unscaled bbox width (xMax - xMin + 1)
    uint16_t box_h_raw;   // unscaled bbox height (yMax - yMin + 1)
    int16_t  ofs_x_raw;   // unscaled bbox x offset (xMin)
    int16_t  ofs_y_raw;   // unscaled bbox y offset (-yMax)
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
    static int stream_read_count = 0;
    stream_read_count++;
    if(stream_read_count <= 5 || stream->file != (void*)stream->dsc) {
        printf("[STREAM_READ] #%d file=%p dsc=%p pos=%lu to_read=%lu\n",
               stream_read_count, (void*)stream->file, (void*)stream->dsc,
               (uint32_t)stream->position, (uint32_t)to_read);
    }
    if(stream->file != NULL) {
        if(ttf_stream_read_from_cache(stream->dsc, stream->position, data, to_read)) {
            stream->position += to_read;
            return;
        }
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
        lv_fs_seek(stream->file, position, LV_FS_SEEK_SET);
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

static int ttf_stream_read_from_cache(ttf_font_desc_t *dsc, size_t pos, void *out, size_t to_read)
{
    // 统计
    static int cache_hit = 0, cache_miss = 0;
    cache_miss++;
    
    // 先检查当前字体的缓存
    if(dsc && dsc->table_cache.active) {
        ttf_table_cache_t *tc = &dsc->table_cache;
        ttf_cached_table_t *tables[] = {&tc->cmap, &tc->loca, &tc->hmtx, &tc->head, &tc->hhea, &tc->os2, NULL};
        for(int i = 0; tables[i] != NULL; i++) {
            ttf_cached_table_t *t = tables[i];
            if(t->data && pos >= t->file_offset && (pos + to_read) <= (t->file_offset + t->size)) {
                lv_memcpy(out, t->data + (pos - t->file_offset), to_read);
                cache_hit++; 
                
                // 调试：每1000次命中打印一次表类型
                if(cache_hit % 1000 == 0) {
                    const char *table_name = (t == &tc->cmap) ? "cmap" : 
                                            (t == &tc->loca) ? "loca" :
                                            (t == &tc->hmtx) ? "hmtx" :
                                            (t == &tc->head) ? "head" :
                                            (t == &tc->hhea) ? "hhea" :
                                            (t == &tc->os2) ? "os2" : "unknown";
                    printf("[CACHE_HIT_DEBUG] %s: pos=%lu-%lu size=%lu cached=%lu-%lu\n", 
                           table_name, pos, pos + to_read, to_read, t->file_offset, t->file_offset + t->size);
                }
                
                return 1;
            }
        }
        // glyf compact cache: 通过二分查找翻译文件偏移到compact偏移
        if(g_glyf_lookup && g_shared_level1_glyf_data) {
            if(pos >= g_glyf_table_file_offset && 
               pos < g_glyf_table_file_offset + g_glyf_table_size) {
                uint32_t rel_pos = pos - g_glyf_table_file_offset;
                // 二分查找：找到包含rel_pos的glyph条目
                int lo = 0, hi = g_glyf_lookup_count - 1;
                while(lo <= hi) {
                    int mid = (lo + hi) / 2;
                    glyf_cache_entry_t *e = &g_glyf_lookup[mid];
                    if(rel_pos < e->glyf_rel_offset) {
                        hi = mid - 1;
                    } else if(rel_pos >= e->glyf_rel_offset + e->glyf_size) {
                        lo = mid + 1;
                    } else {
                        // 命中！计算在compact buffer中的位置
                        uint32_t in_glyph_off = rel_pos - e->glyf_rel_offset;
                        if(in_glyph_off + to_read <= e->glyf_size) {
                            lv_memcpy(out, g_shared_level1_glyf_data + e->compact_offset + in_glyph_off, to_read);
                            cache_hit++; 
                            return 1;
                        }
                        break; // 跨glyph边界读取，罕见，回退SD卡
                    }
                }
            }
        }
        // 再尝试当前字体实例的level1_glyf_data（非共享路径fallback）
        if(dsc && dsc->level1_glyf_data && g_glyf_lookup) {
            // 利用同样的全局查找表，但从dsc的数据中读取
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
                    if(in_glyph_off + to_read <= e->glyf_size) {
                        lv_memcpy(out, dsc->level1_glyf_data + e->compact_offset + in_glyph_off, to_read);
                        cache_hit++; 
                        return 1;
                    }
                    break;
                }
            }
        }
    }
    // 再检查全局共享缓存
    if(g_shared_level1_loaded) {
        ttf_cached_table_t *tables[] = {&g_shared_table_cache.cmap, &g_shared_table_cache.loca,
                                        &g_shared_table_cache.hmtx, &g_shared_table_cache.head,
                                        &g_shared_table_cache.hhea, &g_shared_table_cache.os2, NULL};
        for(int i = 0; tables[i] != NULL; i++) {
            ttf_cached_table_t *t = tables[i];
            if(t->data && pos >= t->file_offset && (pos + to_read) <= (t->file_offset + t->size)) {
                lv_memcpy(out, t->data + (pos - t->file_offset), to_read);
                cache_hit++; return 1;
            }
        }
        // 全局共享缓存的glyf，已由上面的二分查找统一处理
        // （上面的g_glyf_lookup + g_shared_level1_glyf_data已经覆盖了共享路径）
    }
    // 每 5000 次未命中打印一次
    if((cache_miss & 0x1FFF) == 0) {
        printf("[CACHE_STAT] hits=%d misses=%d (hit_rate=%d%%)\n", cache_hit, cache_miss, cache_hit * 100 / (cache_hit + cache_miss));
        // 打印当前缓存状态
        if(dsc) {
            printf("[CACHE_STATE] dsc=%p table_cache.active=%d\n", (void*)dsc, dsc->table_cache.active);
            printf("[CACHE_STATE] cmap: data=%p off=%lu size=%lu\n", dsc->table_cache.cmap.data, 
                   (uint32_t)dsc->table_cache.cmap.file_offset, (uint32_t)dsc->table_cache.cmap.size);
            printf("[CACHE_STATE] loca: data=%p off=%lu size=%lu\n", dsc->table_cache.loca.data,
                   (uint32_t)dsc->table_cache.loca.file_offset, (uint32_t)dsc->table_cache.loca.size);
            printf("[CACHE_STATE] hmtx: data=%p off=%lu size=%lu\n", dsc->table_cache.hmtx.data,
                   (uint32_t)dsc->table_cache.hmtx.file_offset, (uint32_t)dsc->table_cache.hmtx.size);
            printf("[CACHE_STATE] glyf: off=%lu size=%lu level1_data=%p\n",
                   (uint32_t)dsc->table_cache.glyf.file_offset, (uint32_t)dsc->table_cache.glyf.size,
                   dsc->level1_glyf_data);
        }
        printf("[CACHE_MISS_DEBUG] pos=%lu to_read=%lu\n", pos, to_read);
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
    uint8_t *data = _dma_malloc(length, DMAHEAP_PSRAM);
    if(!data) return NULL;
    lv_memcpy(data, ttf_data + offset, length);
    *out_size = length;
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
    
    uint8_t *data = _dma_malloc(length, DMAHEAP_PSRAM);
    if(data == NULL) {
        printf("[TTF] Failed to allocate %lu bytes for table '%s'\n", length, table_name);
        return NULL;
    }
    
    if(LV_FS_RES_OK != lv_fs_seek(file, offset, LV_FS_SEEK_SET)) {
        _dma_free(data, DMAHEAP_PSRAM);
        return NULL;
    }
    
    uint32_t bytes_read = 0;
    uint8_t *ptr = data;
    while(bytes_read < length) {
        uint32_t to_read = length - bytes_read;
        if(to_read > 4096) to_read = 4096;
        uint32_t br;
        if(LV_FS_RES_OK != lv_fs_read(file, ptr + bytes_read, to_read, &br)) {
            _dma_free(data, DMAHEAP_PSRAM);
            return NULL;
        }
        if(br == 0) break;
        bytes_read += br;
    }
    
    *out_size = bytes_read;
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
    
    // 优先从PSRAM内存数据读取，其次从文件读取
    if(dsc->stream.data != NULL) {
        printf("[TTF_DBG] batch: mem path stream.data=%p\n", (void*)dsc->stream.data);
        cmap_data = read_table_from_memory((const uint8_t *)dsc->stream.data, dsc->info.fontstart, "cmap", &cmap_size);
        if(!cmap_data) printf("[TTF_DBG] batch: read_table_from_memory FAILED\n");
    }
    if(!cmap_data) {
        lv_fs_file_t temp_file;
        printf("[TTF_DBG] batch: file path %s\n", dsc->file_path);
        if(LV_FS_RES_OK != lv_fs_open(&temp_file, dsc->file_path, LV_FS_MODE_RD)) {
            printf("[TTF_DBG] batch: lv_fs_open FAILED\n");
            return -1;
        }
        printf("[TTF_DBG] batch: lv_fs_open OK\n");
        if(!find_table_location(&temp_file, dsc->info.fontstart, "cmap", &cmap_file_offset, &cmap_size)) {
            printf("[TTF_DBG] batch: find_table_location FAILED\n");
            lv_fs_close(&temp_file);
            return -1;
        }
        lv_fs_seek(&temp_file, cmap_file_offset, LV_FS_SEEK_SET);
        printf("[TTF_DBG] batch: cmap ofs=%lu sz=%lu\n", (unsigned long)cmap_file_offset, (unsigned long)cmap_size);
        cmap_data = _dma_malloc(cmap_size, DMAHEAP_PSRAM);
        printf("[TTF_DBG] batch: _dma_malloc(%lu, DMAHEAP_PSRAM)=%p\n", (unsigned long)cmap_size, (void*)cmap_data);
        if(cmap_data) {
            uint32_t bytes_read = 0;
            while(bytes_read < cmap_size) {
                uint32_t to_read = cmap_size - bytes_read;
                if(to_read > 4096) to_read = 4096;
                uint32_t br;
                lv_fs_read(&temp_file, cmap_data + bytes_read, to_read, &br);
                if(br == 0) { printf("[TTF_DBG] batch: read 0 at %lu/%lu\n", (unsigned long)bytes_read, (unsigned long)cmap_size); break; }
                bytes_read += br;
            }
            if(bytes_read < cmap_size) {
                printf("[TTF_DBG] batch: read incomplete %lu/%lu\n", (unsigned long)bytes_read, (unsigned long)cmap_size);
                _dma_free(cmap_data, DMAHEAP_PSRAM);
                cmap_data = NULL;
            }
        }
        lv_fs_close(&temp_file);
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
    
    dsc->table_cache.cmap.data = cmap_data;
    dsc->table_cache.cmap.file_offset = cmap_file_offset;
    dsc->table_cache.cmap.size = cmap_size;
    
    return valid_count;
}

typedef struct {
    uint16_t glyph_index;    // glyph索引
    uint16_t orig_index;     // 原数组索引
    uint32_t file_offset;    // 文件中的偏移
    uint32_t size;           // 数据大小
} glyf_sort_entry_t;

static int compare_glyf_entries(const void *a, const void *b)
{
    const glyf_sort_entry_t *ga = (const glyf_sort_entry_t *)a;
    const glyf_sort_entry_t *gb = (const glyf_sort_entry_t *)b;
    if(ga->file_offset < gb->file_offset) return -1;
    if(ga->file_offset > gb->file_offset) return 1;
    return 0;
}

static int batch_read_glyf_data(ttf_font_desc_t *dsc,
                                level1_glyph_info_t *glyphs,
                                uint16_t count,
                                uint8_t **out_data,
                                uint32_t *out_size)
{
    uint32_t loca_file_offset, loca_size;
    uint8_t *loca_data = NULL;
    uint32_t glyf_offset, glyf_size;
    const uint8_t *ttf_mem = (dsc->stream.data != NULL) ? (const uint8_t *)dsc->stream.data : NULL;
    lv_fs_file_t temp_file;
    int file_opened = 0;
    
    if(ttf_mem) {
        if(!find_table_location_in_memory(ttf_mem, dsc->info.fontstart, "loca", &loca_file_offset, &loca_size)) return -1;
        loca_data = _dma_malloc(loca_size, DMAHEAP_PSRAM);
        if(!loca_data) return -1;
        lv_memcpy(loca_data, ttf_mem + loca_file_offset, loca_size);
        if(!find_table_location_in_memory(ttf_mem, dsc->info.fontstart, "glyf", &glyf_offset, &glyf_size)) {
            _dma_free(loca_data, DMAHEAP_PSRAM); return -1;
        }
    } else {
        if(LV_FS_RES_OK != lv_fs_open(&temp_file, dsc->file_path, LV_FS_MODE_RD)) return -1;
        file_opened = 1;
        if(!find_table_location(&temp_file, dsc->info.fontstart, "loca", &loca_file_offset, &loca_size)) {
            lv_fs_close(&temp_file); return -1;
        }
        lv_fs_seek(&temp_file, loca_file_offset, LV_FS_SEEK_SET);
        loca_data = _dma_malloc(loca_size, DMAHEAP_PSRAM);
        if(!loca_data) { lv_fs_close(&temp_file); return -1; }
        {
            uint32_t bytes_read = 0;
            while(bytes_read < loca_size) {
                uint32_t to_read = loca_size - bytes_read;
                if(to_read > 4096) to_read = 4096;
                uint32_t br;
                lv_fs_read(&temp_file, loca_data + bytes_read, to_read, &br);
                if(br == 0) break;
                bytes_read += br;
            }
        }
        if(!find_table_location(&temp_file, dsc->info.fontstart, "glyf", &glyf_offset, &glyf_size)) {
            _dma_free(loca_data, DMAHEAP_PSRAM); lv_fs_close(&temp_file); return -1;
        }
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
        _dma_free(loca_data, DMAHEAP_PSRAM);
        if(file_opened) lv_fs_close(&temp_file);
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
    
    uint8_t *glyf_data = psram_malloc(total_glyf_size);
    if(!glyf_data) {
        _dma_free(loca_data, DMAHEAP_PSRAM);
        if(file_opened) lv_fs_close(&temp_file);
        printf("[TTF] Failed to allocate %lu bytes for compact glyf cache\n", total_glyf_size);
        return -1;
    }
    
    // 构建排序数组，按文件偏移排序以合并相邻读取
    glyf_sort_entry_t *sort_entries = _dma_malloc(valid_count * sizeof(glyf_sort_entry_t), DMAHEAP_PSRAM);
    if(!sort_entries) {
        psram_free(glyf_data);
        _dma_free(loca_data, DMAHEAP_PSRAM);
        if(file_opened) lv_fs_close(&temp_file);
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
    
    // 紧凑排列：按排序后合并相邻/重叠的glyph，批量读取减少IO次数
    uint32_t compact_pos = 0;
    uint32_t read_ops = 0;
    
    uint16_t i = 0;
    while(i < valid_count) {
        // 找出连续可合并的glyph区间
        uint32_t merge_start = sort_entries[i].file_offset;
        uint32_t merge_end = merge_start + sort_entries[i].size;
        uint16_t j = i + 1;
        while(j < valid_count) {
            uint32_t next_start = sort_entries[j].file_offset;
            uint32_t next_end = next_start + sort_entries[j].size;
            // 如果下一个glyph与当前区间相邻或重叠（gap=0），合并
            if(next_start > merge_end) break;  // 有gap，不能合并
            if(next_end > merge_end) merge_end = next_end;
            j++;
        }
        
        uint32_t merge_len = merge_end - merge_start;
        
        // 一次性读取整个合并区间
        if(ttf_mem) {
            lv_memcpy(glyf_data + compact_pos, ttf_mem + merge_start, merge_len);
        } else {
            lv_fs_seek(&temp_file, merge_start, LV_FS_SEEK_SET);
            uint32_t br;
            lv_fs_read(&temp_file, glyf_data + compact_pos, merge_len, &br);
            read_ops++;
        }
        
        // 为区间内每个glyph设置compact_offset
        for(uint16_t k = i; k < j; k++) {
            uint16_t orig_i = sort_entries[k].orig_index;
            uint32_t glyph_in_merge = sort_entries[k].file_offset - merge_start;
            glyphs[orig_i].compact_offset = compact_pos + glyph_in_merge;
        }
        
        compact_pos += merge_len;
        i = j;
    }
    
    printf("[TTF] Merged %u glyph reads into %lu batch read ops (saved %u ops, %u%% reduction)\n",
           valid_count, read_ops, valid_count - (uint16_t)read_ops,
           (valid_count > 0) ? (valid_count - (uint16_t)read_ops) * 100 / valid_count : 0);
    
    _dma_free(sort_entries, DMAHEAP_PSRAM);
    
    printf("[TTF] Glyf loaded: %u glyphs, %lu read ops, compact size=%lu\n",
           valid_count, read_ops, total_glyf_size);
    
    dsc->table_cache.loca.data = loca_data;
    dsc->table_cache.loca.file_offset = loca_file_offset;
    dsc->table_cache.loca.size = loca_size;
    
    // glyf表缓存：存储真实的偏移和大小供全局查找表使用
    dsc->table_cache.glyf.file_offset = glyf_offset;
    dsc->table_cache.glyf.size = glyf_size;
    dsc->table_cache.glyf.data = NULL;
    
    // 构建glyf查找表（全局共享，用于二分查找）
    if(g_glyf_lookup) {
        _dma_free(g_glyf_lookup, DMAHEAP_PSRAM);
        g_glyf_lookup = NULL;
    }
    g_glyf_lookup = _dma_malloc(valid_count * sizeof(glyf_cache_entry_t), DMAHEAP_PSRAM);
    if(g_glyf_lookup) {
        g_glyf_lookup_count = valid_count;
        g_glyf_table_file_offset = glyf_offset;  // glyf表的绝对文件偏移
        g_glyf_table_size = glyf_size;           // glyf表总大小
        
        uint16_t lk_idx = 0;
        for(uint16_t i = 0; i < count; i++) {
            if(glyphs[i].cached) {
                g_glyf_lookup[lk_idx].glyf_rel_offset = glyphs[i].file_glyf_offset;
                g_glyf_lookup[lk_idx].compact_offset = glyphs[i].compact_offset;
                g_glyf_lookup[lk_idx].glyf_size = glyphs[i].glyf_size;
                lk_idx++;
            }
        }
        // 按glyf_rel_offset排序，用于二分查找
        qsort(g_glyf_lookup, valid_count, sizeof(glyf_cache_entry_t), compare_cache_entries);
        printf("[TTF] Glyf lookup table built: %u entries, %lu bytes\n", valid_count, (uint32_t)(valid_count * sizeof(glyf_cache_entry_t)));
    } else {
        g_glyf_lookup_count = 0;
        printf("[TTF] WARNING: Failed to allocate glyf lookup table, glyph cache disabled\n");
    }
    
    if(file_opened) lv_fs_close(&temp_file);
    
    *out_data = glyf_data;
    *out_size = total_glyf_size;
    return 1;
}

static int ttf_load_level1_glyphs(ttf_font_desc_t *dsc, const uint32_t *unicode_list, uint16_t count)
{
    printf("[TTF] Loading Level1 glyphs for %d chars...\n", count);
    
    // 释放旧数据
    if(dsc->level1_glyphs != NULL) {
        _dma_free(dsc->level1_glyphs, DMAHEAP_PSRAM);
        dsc->level1_glyphs = NULL;
    }
    if(dsc->level1_glyf_data != NULL) {
        psram_free(dsc->level1_glyf_data);
        dsc->level1_glyf_data = NULL;
    }
    
    // 步骤1：批量查找glyph index（使用优化的查找函数，只读一次cmap表）
    printf("[TTF] Step 1: Looking up glyph indices...\n");
    level1_glyph_info_t *glyphs = _dma_malloc(count * sizeof(level1_glyph_info_t), DMAHEAP_PSRAM);
    if(!glyphs) {
        printf("[TTF] Failed to allocate glyph info array\n");
        return -1;
    }
    
    int valid_count = batch_lookup_glyph_indices(dsc, unicode_list, count, glyphs);
    if(valid_count < 0) {
        _dma_free(glyphs, DMAHEAP_PSRAM);
        return -1;
    }
    
    printf("[TTF] Found %d valid glyphs\n", valid_count);
    
    // 步骤2：批量读取glyf数据
    printf("[TTF] Step 2: Reading glyf data...\n");
    uint8_t *glyf_data = NULL;
    uint32_t glyf_size = 0;
    
    if(batch_read_glyf_data(dsc, glyphs, count, &glyf_data, &glyf_size) < 0) {
        _dma_free(glyphs, DMAHEAP_PSRAM);
        return -1;
    }
    
    printf("[TTF] Loaded %lu bytes of glyf data\n", glyf_size);

    printf("[TTF] Step 3: Pre-filling metrics_cache...\n");

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
                dsc->table_cache.head.data = _dma_malloc(tbl_sz, DMAHEAP_PSRAM);
                if(dsc->table_cache.head.data) { lv_memcpy(dsc->table_cache.head.data, ttf_mem_cfg + tbl_off, tbl_sz); dsc->table_cache.head.file_offset = tbl_off; dsc->table_cache.head.size = tbl_sz; }
            }
            if(find_table_location_in_memory(ttf_mem_cfg, dsc->info.fontstart, "hhea", &tbl_off, &tbl_sz)) {
                dsc->table_cache.hhea.data = _dma_malloc(tbl_sz, DMAHEAP_PSRAM);
                if(dsc->table_cache.hhea.data) { lv_memcpy(dsc->table_cache.hhea.data, ttf_mem_cfg + tbl_off, tbl_sz); dsc->table_cache.hhea.file_offset = tbl_off; dsc->table_cache.hhea.size = tbl_sz; }
            }
            if(find_table_location_in_memory(ttf_mem_cfg, dsc->info.fontstart, "OS/2", &tbl_off, &tbl_sz)) {
                dsc->table_cache.os2.data = _dma_malloc(tbl_sz, DMAHEAP_PSRAM);
                if(dsc->table_cache.os2.data) { lv_memcpy(dsc->table_cache.os2.data, ttf_mem_cfg + tbl_off, tbl_sz); dsc->table_cache.os2.file_offset = tbl_off; dsc->table_cache.os2.size = tbl_sz; }
            }
        } else if(dsc->file_path[0]) {
            lv_fs_file_t tbl_file;
            if(LV_FS_RES_OK == lv_fs_open(&tbl_file, dsc->file_path, LV_FS_MODE_RD)) {
                dsc->table_cache.head.data = read_table_to_psram(&tbl_file, dsc->info.fontstart, "head", &dsc->table_cache.head.size);
                if(dsc->table_cache.head.data) find_table_location(&tbl_file, dsc->info.fontstart, "head", &dsc->table_cache.head.file_offset, &dsc->table_cache.head.size);
                dsc->table_cache.hhea.data = read_table_to_psram(&tbl_file, dsc->info.fontstart, "hhea", &dsc->table_cache.hhea.size);
                if(dsc->table_cache.hhea.data) find_table_location(&tbl_file, dsc->info.fontstart, "hhea", &dsc->table_cache.hhea.file_offset, &dsc->table_cache.hhea.size);
                dsc->table_cache.os2.data = read_table_to_psram(&tbl_file, dsc->info.fontstart, "OS/2", &dsc->table_cache.os2.size);
                if(dsc->table_cache.os2.data) find_table_location(&tbl_file, dsc->info.fontstart, "OS/2", &dsc->table_cache.os2.file_offset, &dsc->table_cache.os2.size);
                lv_fs_close(&tbl_file);
            }
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
    } else {
        lv_fs_file_t temp_file2;
        if(LV_FS_RES_OK != lv_fs_open(&temp_file2, dsc->file_path, LV_FS_MODE_RD)) {
            printf("[TTF] Failed to open file for hmtx read\n");
            _dma_free(glyphs, DMAHEAP_PSRAM);
            psram_free(glyf_data);
            return -1;
        }
        if(!find_table_location(&temp_file2, dsc->info.fontstart, "hmtx", &hmtx_file_offset, &hmtx_size)) {
            lv_fs_close(&temp_file2);
        } else {
            lv_fs_seek(&temp_file2, hmtx_file_offset, LV_FS_SEEK_SET);
            hmtx_data = _dma_malloc(hmtx_size, DMAHEAP_PSRAM);
            if(hmtx_data) {
                uint32_t bytes_read = 0;
                while(bytes_read < hmtx_size) {
                    uint32_t to_read = hmtx_size - bytes_read;
                    if(to_read > 4096) to_read = 4096;
                    uint32_t br;
                    lv_fs_read(&temp_file2, hmtx_data + bytes_read, to_read, &br);
                    if(br == 0) break;
                    bytes_read += br;
                }
            }
        }
        lv_fs_close(&temp_file2);
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
        
        uint16_t filled = 0, not_found = 0;
        static int debug_prefill_count = 0;
        for(uint16_t i = 0; i < count; i++) {
            uint32_t unicode = unicode_list[i];
            uint16_t glyph_idx = glyphs[i].glyph_index;
            if(unicode < CJK_METRICS_START || unicode > CJK_METRICS_END) continue;
            uint32_t cache_idx = unicode - CJK_METRICS_START;
            ttf_metrics_entry_t *entry = &dsc->metrics_cache[cache_idx];
            if(glyph_idx == 0) {
                entry->valid = 2;
                not_found++;
                continue;
            }
            if(glyph_idx < num_glyphs) {
                // 正确处理 hmtx 表：根据 numberOfHMetrics 来决定读取方式
                uint8_t *hmtx_ptr = NULL;
                if(glyph_idx < numberOfHMetrics) {
                    // 使用 4 字节格式 (advanceWidth, leftSideBearing)
                    hmtx_ptr = hmtx_data + glyph_idx * 4;
                } else {
                    // 使用最后一个有效度量的 advanceWidth + 单独的 leftSideBearing 数组
                    hmtx_ptr = hmtx_data + (numberOfHMetrics - 1) * 4;  // 最后一个 advanceWidth
                }
                
                int16_t adv_w_raw = (int16_t)((hmtx_ptr[0] << 8) | hmtx_ptr[1]);
                entry->adv_w_raw = (uint16_t)(adv_w_raw > 0 ? adv_w_raw : 0);
                
                // 调试：打印前5个汉字的完整计算过程
                if(debug_prefill_count < 5) {
                    printf("[PREFILL] U+%04X glyph_idx=%u hmtx_offset=%u hmtx_bytes=%02X%02X%02X%02X adv_raw=%d\n",
                           unicode, glyph_idx, (unsigned)(hmtx_ptr - hmtx_data),
                           hmtx_ptr[0], hmtx_ptr[1], hmtx_ptr[2], hmtx_ptr[3],
                           adv_w_raw);
                    debug_prefill_count++;
                }
            }
            if(glyph_idx > 0) {
                // === 修复2: 直接从PSRAM compact glyf数据读header前10字节获取bbox ===
                // glyf header: numberOfContours(2) + xMin(2) + yMin(2) + xMax(2) + yMax(2)
                // 与stbtt_GetGlyphBitmapBox(scale=1.0)完全等价，但零IO、零轮廓解析
                if(glyphs[i].cached && glyphs[i].glyf_size >= 10) {
                    uint8_t *gh = glyf_data + glyphs[i].compact_offset;
                    int16_t xMin = (int16_t)((gh[2] << 8) | gh[3]);
                    int16_t yMin = (int16_t)((gh[4] << 8) | gh[5]);
                    int16_t xMax = (int16_t)((gh[6] << 8) | gh[7]);
                    int16_t yMax = (int16_t)((gh[8] << 8) | gh[9]);
                    // stbtt_GetGlyphBitmapBox(scale=1.0): bbx0=xMin, bby0=-yMax, bbx1=xMax, bby1=-yMin
                    entry->box_w_raw = (uint16_t)((xMax - xMin + 1) > 0 ? (xMax - xMin + 1) : 0);
                    entry->box_h_raw = (uint16_t)((yMax - yMin + 1) > 0 ? (yMax - yMin + 1) : 0);
                    entry->ofs_x_raw = xMin;
                    entry->ofs_y_raw = yMin;  // = -bby1 (bby1=-yMin)
                } else {
                    // 空glyph或数据不足，使用默认值
                    entry->box_w_raw = 0;
                    entry->box_h_raw = 0;
                    entry->ofs_x_raw = 0;
                    entry->ofs_y_raw = 0;
                }
                entry->glyph_index = glyph_idx;
            }
            entry->valid = 1;
            filled++;
        }
        printf("[TTF] Metrics pre-filled: %u valid, %u not found\n", filled, not_found);
        dsc->table_cache.hmtx.data = hmtx_data;
        dsc->table_cache.hmtx.file_offset = hmtx_file_offset;
        dsc->table_cache.hmtx.size = hmtx_size;
    }

    // 缓存 head/hhea/OS2 等小表
    const uint8_t *ttf_mem3 = (dsc->stream.data != NULL) ? (const uint8_t *)dsc->stream.data : NULL;
    if(ttf_mem3) {
        uint32_t tbl_off, tbl_sz;
        if(find_table_location_in_memory(ttf_mem3, dsc->info.fontstart, "head", &tbl_off, &tbl_sz)) {
            dsc->table_cache.head.data = _dma_malloc(tbl_sz, DMAHEAP_PSRAM);
            if(dsc->table_cache.head.data) { lv_memcpy(dsc->table_cache.head.data, ttf_mem3 + tbl_off, tbl_sz); dsc->table_cache.head.file_offset = tbl_off; dsc->table_cache.head.size = tbl_sz; }
        }
        if(find_table_location_in_memory(ttf_mem3, dsc->info.fontstart, "hhea", &tbl_off, &tbl_sz)) {
            dsc->table_cache.hhea.data = _dma_malloc(tbl_sz, DMAHEAP_PSRAM);
            if(dsc->table_cache.hhea.data) { lv_memcpy(dsc->table_cache.hhea.data, ttf_mem3 + tbl_off, tbl_sz); dsc->table_cache.hhea.file_offset = tbl_off; dsc->table_cache.hhea.size = tbl_sz; }
        }
        if(find_table_location_in_memory(ttf_mem3, dsc->info.fontstart, "OS/2", &tbl_off, &tbl_sz)) {
            dsc->table_cache.os2.data = _dma_malloc(tbl_sz, DMAHEAP_PSRAM);
            if(dsc->table_cache.os2.data) { lv_memcpy(dsc->table_cache.os2.data, ttf_mem3 + tbl_off, tbl_sz); dsc->table_cache.os2.file_offset = tbl_off; dsc->table_cache.os2.size = tbl_sz; }
        }
    } else if(dsc->file_path[0]) {
        lv_fs_file_t tbl_file;
        if(LV_FS_RES_OK == lv_fs_open(&tbl_file, dsc->file_path, LV_FS_MODE_RD)) {
            dsc->table_cache.head.data = read_table_to_psram(&tbl_file, dsc->info.fontstart, "head", &dsc->table_cache.head.size);
            if(dsc->table_cache.head.data) find_table_location(&tbl_file, dsc->info.fontstart, "head", &dsc->table_cache.head.file_offset, &dsc->table_cache.head.size);
            dsc->table_cache.hhea.data = read_table_to_psram(&tbl_file, dsc->info.fontstart, "hhea", &dsc->table_cache.hhea.size);
            if(dsc->table_cache.hhea.data) find_table_location(&tbl_file, dsc->info.fontstart, "hhea", &dsc->table_cache.hhea.file_offset, &dsc->table_cache.hhea.size);
            dsc->table_cache.os2.data = read_table_to_psram(&tbl_file, dsc->info.fontstart, "OS/2", &dsc->table_cache.os2.size);
            if(dsc->table_cache.os2.data) find_table_location(&tbl_file, dsc->info.fontstart, "OS/2", &dsc->table_cache.os2.file_offset, &dsc->table_cache.os2.size);
            lv_fs_close(&tbl_file);
        }
    }

    g_shared_table_cache = dsc->table_cache;
    g_shared_table_cache.glyf.data = NULL;
    // 全局共享：查找表已经在batch_read_glyf_data中设置好了，无需重复构建
    g_shared_level1_glyf_data = glyf_data;
    g_shared_level1_glyf_size = glyf_size;
    g_shared_level1_glyphs = glyphs;
    g_shared_level1_glyph_count = count;
    g_shared_level1_loaded = 1;
    g_shared_metrics_cache = dsc->metrics_cache;

    printf("[TTF] Level1 glyphs loaded, table_cache active (cmap=%luB loca=%luB hmtx=%luB glyf=%luB)\n",
           dsc->table_cache.cmap.size, dsc->table_cache.loca.size,
           dsc->table_cache.hmtx.size, dsc->table_cache.glyf.size);
    return valid_count;
}

static uint8_t * ttf_get_cached_glyph_data(ttf_font_desc_t *dsc, uint16_t glyph_index, uint32_t *out_size)
{
    if(dsc->level1_loaded && dsc->level1_glyphs != NULL) {
        for(uint16_t i = 0; i < dsc->level1_glyph_count; i++) {
            if(dsc->level1_glyphs[i].glyph_index == glyph_index && dsc->level1_glyphs[i].cached) {
                *out_size = dsc->level1_glyphs[i].glyf_size;
                return dsc->level1_glyf_data + dsc->level1_glyphs[i].compact_offset;
            }
        }
    }
    return NULL;
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

    if(unicode_letter < 0x4E00) {
        return false;
    }

    if(unicode_letter >= 0xFF00 && unicode_letter <= 0xFFEF) {
        return false;
    }

    // 每 200 个 CJK 字打印一次进度（检测卡死）
    static int glyph_dsc_progress = 0;
    if(unicode_letter >= CJK_METRICS_START && unicode_letter <= CJK_METRICS_END) {
        glyph_dsc_progress++;
        if((glyph_dsc_progress & 0xFF) == 0) {
            printf("[GLYPH_ALIVE] processed %d chars, current U+0x%04X\n", glyph_dsc_progress, unicode_letter);
        }
    }

    ttf_font_desc_t * dsc = (ttf_font_desc_t *)font->dsc;

    if(unicode_letter >= CJK_METRICS_START && unicode_letter <= CJK_METRICS_END) {
        uint32_t idx = unicode_letter - CJK_METRICS_START;

        // 所有字体共享同一份 metrics_cache，直接查
        if(dsc->metrics_cache && dsc->metrics_cache[idx].valid == 1) {
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
            
            static int debug_cjk_count = 0;
            if(debug_cjk_count < 5) {
                printf("[CJK_METRICS] U+%04X scale=%.4f adv_raw=%u adv=%u box=%ux%u ofs=%d,%d\n",
                       unicode_letter, dsc->scale,
                       dsc->metrics_cache[idx].adv_w_raw, dsc_out->adv_w,
                       dsc_out->box_w, dsc_out->box_h,
                       dsc_out->ofs_x, dsc_out->ofs_y);
                debug_cjk_count++;
            }
            return true;
        }
        if(dsc->metrics_cache && dsc->metrics_cache[idx].valid == 2) {
            return false;
        }
    }

    // 未命中缓存，后备：stbtt 从 PSRAM 数据渲染
    uint32_t st0 = xTaskGetTickCount();
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
    if(unicode_letter >= CJK_METRICS_START && unicode_letter <= CJK_METRICS_END) {
        ttf_metrics_entry_t *cache_ptr = dsc->metrics_cache ? dsc->metrics_cache : g_shared_metrics_cache;
        if(cache_ptr) {
            uint32_t idx = unicode_letter - CJK_METRICS_START;
            int rx0, ry0, rx1, ry1;
            stbtt_GetGlyphBitmapBox(&dsc->info, g1, 1.0f, 1.0f, &rx0, &ry0, &rx1, &ry1);
            cache_ptr[idx].adv_w_raw = (uint16_t)(advw + k);
            cache_ptr[idx].box_w_raw = (uint16_t)((rx1 - rx0 + 1) > 0 ? (uint16_t)(rx1 - rx0 + 1) : 0);
            cache_ptr[idx].box_h_raw = (uint16_t)((ry1 - ry0 + 1) > 0 ? (uint16_t)(ry1 - ry0 + 1) : 0);
            cache_ptr[idx].ofs_x_raw = rx0;
            cache_ptr[idx].ofs_y_raw = -ry1;
            cache_ptr[idx].glyph_index = (uint16_t)g1;
            cache_ptr[idx].valid = 1;
        }
    }

    return true;
}

static const uint8_t * ttf_get_glyph_bitmap_cb(const lv_font_t * font, uint32_t unicode_letter)
{
    ttf_font_desc_t * dsc = (ttf_font_desc_t *)font->dsc;
    if(unicode_letter >= 0xFF00 && unicode_letter <= 0xFFEF) {
        return NULL;
    }
    if(unicode_letter >= CJK_METRICS_START && unicode_letter <= CJK_METRICS_END && g_shared_level1_loaded) {
        uint32_t idx = unicode_letter - CJK_METRICS_START;
        int valid = 0;
        if(dsc->metrics_cache && dsc->metrics_cache[idx].valid == 1) valid = 1;
        if(!valid && g_shared_metrics_cache && g_shared_metrics_cache[idx].valid == 1) valid = 1;
        if(!valid) {
            uint8_t *buf = _dma_malloc(256, DMAHEAP_PSRAM);
            if(buf) lv_memset(buf, 0, 256);
            return buf;
        }
    }
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
    static uint8_t *cached_bitmap = NULL;
    static uint32_t cached_unicode = 0;
    static size_t cached_sz = 0;
    if(unicode_letter == cached_unicode && cached_bitmap && cached_sz == szb) {
        return cached_bitmap;
    }
    if(cached_bitmap) {
        _dma_free(cached_bitmap, DMAHEAP_PSRAM);
        cached_bitmap = NULL;
    }
    uint8_t * buffer = _dma_malloc(szb, DMAHEAP_PSRAM);
    if(!buffer) {
        LV_LOG_ERROR("failed to allocate bitmap buffer");
        return NULL;
    }
    lv_memset(buffer, 0, szb);
    stbtt_MakeGlyphBitmap(&dsc->info, buffer, w, h, stride, dsc->scale, dsc->scale, g1);
    cached_bitmap = buffer;
    cached_unicode = unicode_letter;
    cached_sz = szb;
    
    static int debug_bitmap_count = 0;
    if(debug_bitmap_count < 5 && unicode_letter >= CJK_METRICS_START) {
        int non_zero = 0;
        for(uint32_t i = 0; i < szb; i++) if(buffer[i]) non_zero++;
        printf("[GLYPH_BITMAP] U+%04X w=%d h=%d sz=%u non_zero=%d\n",
               unicode_letter, w, h, (unsigned)szb, non_zero);
        printf("[GLYPH_HEX] ");
        for(int i = 0; i < 64 && i < (int)szb; i++) printf("%02X ", buffer[i]);
        printf("\n");
        debug_bitmap_count++;
    }

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
    } else {
        dsc->metrics_cache = (ttf_metrics_entry_t *)_dma_malloc(CJK_METRICS_COUNT * sizeof(ttf_metrics_entry_t), DMAHEAP_PSRAM);
        if(dsc->metrics_cache) {
            memset(dsc->metrics_cache, 0, CJK_METRICS_COUNT * sizeof(ttf_metrics_entry_t));
        }
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
            if(dsc->metrics_cache) {
                _dma_free(dsc->metrics_cache, DMAHEAP_PSRAM);
                dsc->metrics_cache = NULL;
            }
            if(dsc->level1_glyphs) {
                _dma_free(dsc->level1_glyphs, DMAHEAP_PSRAM);
                dsc->level1_glyphs = NULL;
            }
            if(dsc->level1_glyf_data) {
                psram_free(dsc->level1_glyf_data);
                dsc->level1_glyf_data = NULL;
            }
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
