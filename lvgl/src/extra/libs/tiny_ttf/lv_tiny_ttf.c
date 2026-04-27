#include "lv_tiny_ttf.h"

#if LV_USE_TINY_TTF
#include <stdio.h>
#include <string.h>
#include "../../../misc/lv_lru.h"
#include "sys/sys_heap.h"

extern void *psram_malloc(size_t size);
extern void psram_free(void *ptr);

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

#define CJK_METRICS_START 0x4E00u
#define CJK_METRICS_END   0x9FFFu
#define CJK_METRICS_COUNT (CJK_METRICS_END - CJK_METRICS_START + 1u)

#define LEVEL1_GLYPH_CACHE_SIZE 3500

typedef struct {
    uint8_t adv_w;
    uint8_t box_w;
    uint8_t box_h;
    int8_t  ofs_x;
    int8_t  ofs_y;
    uint8_t valid;
    uint8_t pad;
} ttf_metrics_entry_t;

#if LV_TINY_TTF_FILE_SUPPORT
typedef struct ttf_cb_stream {
    lv_fs_file_t * file;
    const void * data;
    size_t size;
    size_t position;
} ttf_cb_stream_t;

static void ttf_cb_stream_read(ttf_cb_stream_t * stream, void * data, size_t to_read)
{
    if(stream->file != NULL) {
        uint32_t br;
        lv_fs_read(stream->file, data, to_read, &br);
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

// TTF表缓存结构
typedef struct {
    uint8_t *cmap_data;      // cmap表数据
    uint32_t cmap_size;      // cmap表大小
    uint8_t *loca_data;      // loca表数据
    uint32_t loca_size;      // loca表大小
    uint16_t num_glyphs;     // glyph总数
    uint16_t index_to_loc_format;  // loca表格式（0=short, 1=long）
} ttf_table_cache_t;

typedef struct {
    uint16_t unicode;
    uint16_t glyph_index;
    uint32_t glyf_offset;    // 在PSRAM缓存中的偏移
    uint16_t glyf_size;
    uint8_t cached;
} level1_glyph_info_t;

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
} ttf_font_desc_t;

// 从TTF文件中查找特定表的位置和大小
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
    printf("[TTF] find_table: font_start=%lu, signature=0x%08lX\n", font_start, signature);
    if(signature != 0x00010000 && signature != 0x4F54544F) {
        printf("[TTF] Invalid TTF signature\n");
        return 0;
    }
    
    // numTables (bytes 4-5, big-endian uint16)
    num_tables = (buf[4] << 8) | buf[5];
    printf("[TTF] num_tables=%u\n", num_tables);
    
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
        
        printf("[TTF] Table[%d]: '%s' offset=%lu length=%lu\n", i, name, offset, length);
        
        if(strcmp(name, table_name) == 0) {
            *out_offset = offset + font_start;
            *out_length = length;
            printf("[TTF] Found table '%s' at file offset %lu\n", table_name, *out_offset);
            return 1;
        }
    }
    
    printf("[TTF] Table '%s' not found\n", table_name);
    return 0;
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
    // cmap format 12 解析（大端序）
    uint32_t num_groups = ((uint32_t)cmap_data[12] << 24) | ((uint32_t)cmap_data[13] << 16) |
                          ((uint32_t)cmap_data[14] << 8) | cmap_data[15];
    
    uint8_t *groups_ptr = cmap_data + 16;
    
    for(uint32_t i = 0; i < num_groups; i++) {
        // 手动解析大端序uint32
        uint32_t start_char = ((uint32_t)groups_ptr[0] << 24) | ((uint32_t)groups_ptr[1] << 16) |
                             ((uint32_t)groups_ptr[2] << 8) | groups_ptr[3];
        uint32_t end_char = ((uint32_t)groups_ptr[4] << 24) | ((uint32_t)groups_ptr[5] << 16) |
                           ((uint32_t)groups_ptr[6] << 8) | groups_ptr[7];
        uint32_t start_glyph = ((uint32_t)groups_ptr[8] << 24) | ((uint32_t)groups_ptr[9] << 16) |
                              ((uint32_t)groups_ptr[10] << 8) | groups_ptr[11];
        
        if(unicode >= start_char && unicode <= end_char) {
            return (uint16_t)((uint32_t)start_glyph + (unicode - start_char));
        }
        
        groups_ptr += 12;
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
    // 先读取cmap表
    uint32_t cmap_size;
    lv_fs_file_t temp_file;
    if(LV_FS_RES_OK != lv_fs_open(&temp_file, dsc->file_path, LV_FS_MODE_RD)) {
        return -1;
    }
    uint8_t *cmap_data = read_table_to_psram(&temp_file, dsc->info.fontstart, "cmap", &cmap_size);
    lv_fs_close(&temp_file);
    
    if(!cmap_data) {
        printf("[TTF] Failed to read cmap table\n");
        return -1;
    }
    
    int valid_count = 0;
    int not_found_count = 0;
    
    for(uint16_t i = 0; i < count; i++) {
        uint32_t unicode = unicode_list[i];
        uint16_t glyph_index = lookup_glyph_in_cmap(cmap_data, unicode);
        
        results[i].unicode = unicode;
        results[i].glyph_index = glyph_index;
        results[i].cached = 0;
        results[i].glyf_size = 0;
        results[i].glyf_offset = 0;
        
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
    
    psram_free(cmap_data);
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
    lv_fs_file_t temp_file;
    if(LV_FS_RES_OK != lv_fs_open(&temp_file, dsc->file_path, LV_FS_MODE_RD)) {
        return -1;
    }
    
    uint32_t loca_size;
    uint8_t *loca_data = read_table_to_psram(&temp_file, dsc->info.fontstart, "loca", &loca_size);
    if(!loca_data) {
        lv_fs_close(&temp_file);
        return -1;
    }
    
    uint32_t glyf_offset, glyf_size;
    if(!find_table_location(&temp_file, dsc->info.fontstart, "glyf", &glyf_offset, &glyf_size)) {
        psram_free(loca_data);
        lv_fs_close(&temp_file);
        return -1;
    }
    
    printf("[TTF] indexToLocFormat=%d (0=short, 1=long)\n", dsc->info.indexToLocFormat);
    printf("[TTF] glyf_offset=%lu, loca_size=%lu\n", glyf_offset, loca_size);
    
    glyf_sort_entry_t *sort_entries = psram_malloc(count * sizeof(glyf_sort_entry_t));
    if(!sort_entries) {
        psram_free(loca_data);
        lv_fs_close(&temp_file);
        return -1;
    }
    
    uint32_t total_glyf_size = 0;
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
                uint16_t idx4 = idx * 4;
                uint32_t off1 = (loca_data[idx4] << 24) | (loca_data[idx4 + 1] << 16) | (loca_data[idx4 + 2] << 8) | loca_data[idx4 + 3];
                uint32_t off2 = (loca_data[(idx + 1) * 4] << 24) | (loca_data[(idx + 1) * 4 + 1] << 16) | (loca_data[(idx + 1) * 4 + 2] << 8) | loca_data[(idx + 1) * 4 + 3];
                g1 = glyf_offset + off1;
                g2 = glyf_offset + off2;
            }

            if(g1 != g2) {
                sort_entries[valid_count].glyph_index = idx;
                sort_entries[valid_count].orig_index = i;
                sort_entries[valid_count].file_offset = g1;
                sort_entries[valid_count].size = g2 - g1;
                valid_count++;
                total_glyf_size += (g2 - g1);
            }
        }
    }
    
    if(valid_count == 0) {
        psram_free(sort_entries);
        psram_free(loca_data);
        lv_fs_close(&temp_file);
        return 0;
    }
    
    printf("[TTF] Total glyf data size: %lu bytes for %u valid glyphs\n", total_glyf_size, valid_count);
    
    qsort(sort_entries, valid_count, sizeof(glyf_sort_entry_t), compare_glyf_entries);
    printf("[TTF] Sorted %u glyphs by file offset\n", valid_count);
    
    uint8_t *glyf_data = psram_malloc(total_glyf_size);
    if(!glyf_data) {
        psram_free(sort_entries);
        psram_free(loca_data);
        lv_fs_close(&temp_file);
        printf("[TTF] Failed to allocate %lu bytes for glyf data\n", total_glyf_size);
        return -1;
    }
    
    uint32_t data_offset = 0;
    uint32_t read_ops = 0;
    uint32_t merged_reads = 0;
    
    uint16_t i = 0;
    while(i < valid_count) {
        uint32_t start_offset = sort_entries[i].file_offset;
        uint32_t end_offset = start_offset + sort_entries[i].size;
        uint16_t start_idx = i;
        
        while(i + 1 < valid_count) {
            uint32_t next_offset = sort_entries[i + 1].file_offset;
            uint32_t gap = next_offset - end_offset;
            
            if(gap <= 64) {
                end_offset = next_offset + sort_entries[i + 1].size;
                i++;
            } else {
                break;
            }
        }
        
        uint32_t read_size = end_offset - start_offset;
        lv_fs_seek(&temp_file, start_offset, LV_FS_SEEK_SET);
        
        uint8_t *temp_buf = psram_malloc(read_size);
        if(temp_buf) {
            uint32_t br;
            lv_fs_read(&temp_file, temp_buf, read_size, &br);
            
            for(uint16_t j = start_idx; j <= i; j++) {
                uint16_t orig_idx = sort_entries[j].orig_index;
                uint32_t local_offset = sort_entries[j].file_offset - start_offset;
                
                glyphs[orig_idx].glyf_offset = data_offset;
                glyphs[orig_idx].glyf_size = sort_entries[j].size;
                glyphs[orig_idx].cached = 1;
                
                lv_memcpy(glyf_data + data_offset, temp_buf + local_offset, sort_entries[j].size);
                data_offset += sort_entries[j].size;
            }
            
            psram_free(temp_buf);
        } else {
            for(uint16_t j = start_idx; j <= i; j++) {
                uint16_t orig_idx = sort_entries[j].orig_index;
                
                lv_fs_seek(&temp_file, sort_entries[j].file_offset, LV_FS_SEEK_SET);
                uint32_t br;
                lv_fs_read(&temp_file, glyf_data + data_offset, sort_entries[j].size, &br);
                
                glyphs[orig_idx].glyf_offset = data_offset;
                glyphs[orig_idx].glyf_size = sort_entries[j].size;
                glyphs[orig_idx].cached = 1;
                data_offset += sort_entries[j].size;
                read_ops++;
            }
        }
        
        read_ops++;
        merged_reads += (i - start_idx + 1);
        i++;
    }
    
    printf("[TTF] Read ops: %lu (merged %lu glyphs, saved %lu seeks)\n", 
           read_ops, merged_reads, merged_reads - read_ops);
    
    psram_free(sort_entries);
    psram_free(loca_data);
    lv_fs_close(&temp_file);
    
    *out_data = glyf_data;
    *out_size = total_glyf_size;
    return 1;
}

static int ttf_load_level1_glyphs(ttf_font_desc_t *dsc, const uint32_t *unicode_list, uint16_t count)
{
    printf("[TTF] Loading Level1 glyphs for %d chars...\n", count);
    
    // 释放旧数据
    if(dsc->level1_glyphs != NULL) {
        psram_free(dsc->level1_glyphs);
        dsc->level1_glyphs = NULL;
    }
    if(dsc->level1_glyf_data != NULL) {
        psram_free(dsc->level1_glyf_data);
        dsc->level1_glyf_data = NULL;
    }
    
    // 步骤1：批量查找glyph index（使用优化的查找函数，只读一次cmap表）
    printf("[TTF] Step 1: Looking up glyph indices...\n");
    level1_glyph_info_t *glyphs = psram_malloc(count * sizeof(level1_glyph_info_t));
    if(!glyphs) {
        printf("[TTF] Failed to allocate glyph info array\n");
        return -1;
    }
    
    int valid_count = batch_lookup_glyph_indices(dsc, unicode_list, count, glyphs);
    if(valid_count < 0) {
        psram_free(glyphs);
        return -1;
    }
    
    printf("[TTF] Found %d valid glyphs\n", valid_count);
    
    // 步骤2：批量读取glyf数据
    printf("[TTF] Step 2: Reading glyf data...\n");
    uint8_t *glyf_data = NULL;
    uint32_t glyf_size = 0;
    
    if(batch_read_glyf_data(dsc, glyphs, count, &glyf_data, &glyf_size) < 0) {
        psram_free(glyphs);
        return -1;
    }
    
    printf("[TTF] Loaded %lu bytes of glyf data\n", glyf_size);
    
    // 步骤3：保存结果
    dsc->level1_glyphs = glyphs;
    dsc->level1_glyf_data = glyf_data;
    dsc->level1_glyf_total_size = glyf_size;
    dsc->level1_glyph_count = count;
    dsc->level1_loaded = 1;
    
    printf("[TTF] Level1 glyphs loaded successfully\n");
    return valid_count;
}

static uint8_t * ttf_get_cached_glyph_data(ttf_font_desc_t *dsc, uint16_t glyph_index, uint32_t *out_size)
{
    if(dsc->level1_loaded && dsc->level1_glyphs != NULL) {
        for(uint16_t i = 0; i < dsc->level1_glyph_count; i++) {
            if(dsc->level1_glyphs[i].glyph_index == glyph_index && dsc->level1_glyphs[i].cached) {
                *out_size = dsc->level1_glyphs[i].glyf_size;
                return dsc->level1_glyf_data + dsc->level1_glyphs[i].glyf_offset;
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

    ttf_font_desc_t * dsc = (ttf_font_desc_t *)font->dsc;

    if(unicode_letter >= CJK_METRICS_START && unicode_letter <= CJK_METRICS_END && dsc->metrics_cache) {
        uint32_t idx = unicode_letter - CJK_METRICS_START;
        ttf_metrics_entry_t *entry = &dsc->metrics_cache[idx];
        if(entry->valid == 1) {
            dsc_out->adv_w = entry->adv_w;
            dsc_out->box_w = entry->box_w;
            dsc_out->box_h = entry->box_h;
            dsc_out->ofs_x = entry->ofs_x;
            dsc_out->ofs_y = entry->ofs_y;
            dsc_out->bpp = 8;
            dsc_out->is_placeholder = false;
            return true;
        }
        else if(entry->valid == 2) {
            return false;
        }
    }

    int g1 = stbtt_FindGlyphIndex(&dsc->info, (int)unicode_letter);
    if(g1 == 0) {
        if(unicode_letter >= CJK_METRICS_START && unicode_letter <= CJK_METRICS_END && dsc->metrics_cache) {
            dsc->metrics_cache[unicode_letter - CJK_METRICS_START].valid = 2;
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

    if(unicode_letter >= CJK_METRICS_START && unicode_letter <= CJK_METRICS_END && dsc->metrics_cache) {
        uint32_t idx = unicode_letter - CJK_METRICS_START;
        dsc->metrics_cache[idx].adv_w = dsc_out->adv_w;
        dsc->metrics_cache[idx].box_w = dsc_out->box_w;
        dsc->metrics_cache[idx].box_h = dsc_out->box_h;
        dsc->metrics_cache[idx].ofs_x = x1;
        dsc->metrics_cache[idx].ofs_y = -y2;
        dsc->metrics_cache[idx].valid = 1;
    }

    return true;
}

static const uint8_t * ttf_get_glyph_bitmap_cb(const lv_font_t * font, uint32_t unicode_letter)
{
    ttf_font_desc_t * dsc = (ttf_font_desc_t *)font->dsc;
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
    return buffer;
}

static void ttf_set_font_size_cb(lv_font_t * font, lv_coord_t line_height)
{
    ttf_font_desc_t * dsc = (ttf_font_desc_t *)font->dsc;
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&dsc->info, &ascent, &descent, &line_gap);
    dsc->scale = (float)line_height / (ascent - descent);
    dsc->ascent = (int)(ascent * dsc->scale);
    dsc->descent = (int)(-descent * dsc->scale);
    font->base_line = dsc->descent;
    font->line_height = line_height;
    font->get_glyph_dsc = ttf_get_glyph_dsc_cb;
    font->get_glyph_bitmap = ttf_get_glyph_bitmap_cb;
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
    }
    else {
        dsc->stream.file = NULL;
        dsc->stream.data = (const uint8_t *)data;
        dsc->stream.size = data_size;
        dsc->stream.position = 0;
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

    dsc->metrics_cache = (ttf_metrics_entry_t *)psram_malloc(CJK_METRICS_COUNT * sizeof(ttf_metrics_entry_t));
    if(dsc->metrics_cache) {
        memset(dsc->metrics_cache, 0, CJK_METRICS_COUNT * sizeof(ttf_metrics_entry_t));
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
                psram_free(dsc->metrics_cache);
                dsc->metrics_cache = NULL;
            }
            if(dsc->level1_glyphs) {
                psram_free(dsc->level1_glyphs);
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
