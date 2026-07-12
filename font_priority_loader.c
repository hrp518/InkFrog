#include "font_priority_loader.h"
#include "level1_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl/lvgl.h"
#include "lvgl/src/extra/libs/tiny_ttf/lv_tiny_ttf.h"

/* ASCII printable range: 0x20 (space) to 0x7E (tilde) = 95 chars */
#define ASCII_START  0x0020
#define ASCII_END    0x007E
#define ASCII_COUNT  (ASCII_END - ASCII_START + 1)

/* 预加载 glyf 预算裁剪时优先保留：数字 → 标点 → 其余 ASCII → CJK 标点 → Level1 汉字 */
static const uint32_t g_prio_cjk_punct[] = {
    0x3002, 0x3001, 0x3000, 0x3010, 0x3011, 0x2014, 0x201C, 0x201D,
    0x2026, 0x00B7, 0xFF0C, 0xFF0E, 0xFF01, 0xFF1F, 0xFF1B, 0xFF1A,
    0xFF08, 0xFF09, 0xFF3B, 0xFF3D, 0x300A, 0x300B, 0x2018, 0x2019,
};
static const uint32_t g_prio_ascii_punct[] = {
    '.', ',', ';', ':', '!', '?', '"', '\'',
    '(', ')', '[', ']', '-', '_', '+', '=',
    '*', '/', '\\', '@', '#', '$', '%', '&',
    '<', '>', '{', '}', '|', '~', '`', '^',
};

static int g_initialized = 0;

/* 3500 CJK + 95 ASCII + 标点余量 */
#define PRELOAD_COMBINED_CAP 3700
static uint32_t s_preload_combined[PRELOAD_COMBINED_CAP]
    __attribute__((section(".psram_bss")));

static int combined_has(const uint32_t *arr, int n, uint32_t cp)
{
    for(int i = 0; i < n; i++) {
        if(arr[i] == cp) return 1;
    }
    return 0;
}

static int combined_add(uint32_t *arr, int *n, int cap, uint32_t cp)
{
    if(combined_has(arr, *n, cp)) return 0;
    if(*n >= cap) return -1;
    arr[(*n)++] = cp;
    return 1;
}
static lv_font_t *g_ttf_font = NULL;

int font_priority_loader_init(void)
{
    if (g_initialized) {
        return 0;
    }
    printf("[FONT_LOADER] Level1 CJK chars: %d + ASCII chars: %d = %d total\n",
           level1_chars_count, ASCII_COUNT, level1_chars_count + ASCII_COUNT);
    g_initialized = 1;
    return 0;
}

void font_priority_loader_set_font(lv_font_t *font)
{
    g_ttf_font = font;
}

int font_priority_loader_preload(void)
{
    if (!g_initialized) {
        font_priority_loader_init();
    }
    
    if (!g_ttf_font) {
        printf("[FONT_LOADER] No font set, skipping preload\n");
        return 0;
    }
    
    /* 按优先级合并字表：数字/符号最先，Level1 汉字最后（预算裁剪时从尾部丢） */
    int cap = PRELOAD_COMBINED_CAP;
    uint32_t * combined = s_preload_combined;

    int n = 0;
    int prio_digits = 0, prio_punct = 0;

    for(uint32_t c = '0'; c <= '9'; c++) {
        if(combined_add(combined, &n, cap, c) == 1) prio_digits++;
    }
    for(uint32_t c = 0xFF10; c <= 0xFF19; c++) {
        if(combined_add(combined, &n, cap, c) == 1) prio_digits++;
    }

    for(size_t i = 0; i < sizeof(g_prio_ascii_punct) / sizeof(g_prio_ascii_punct[0]); i++) {
        if(combined_add(combined, &n, cap, g_prio_ascii_punct[i]) == 1) prio_punct++;
    }
    for(size_t i = 0; i < sizeof(g_prio_cjk_punct) / sizeof(g_prio_cjk_punct[0]); i++) {
        if(combined_add(combined, &n, cap, g_prio_cjk_punct[i]) == 1) prio_punct++;
    }

    for(uint32_t c = ASCII_START; c <= ASCII_END; c++) {
        combined_add(combined, &n, cap, c);
    }

    for(int i = 0; i < level1_chars_count; i++) {
        combined_add(combined, &n, cap, level1_chars[i]);
    }

    int total_count = n;
    printf("[FONT_LOADER] Priority preload list: digits=%d punct_extra=%d total=%d (CJK level1=%d)\n",
           prio_digits, prio_punct, total_count, level1_chars_count);
    printf("[FONT_LOADER] Loading glyphs to PSRAM (digits/symbols first)...\n");
    
    int result = lv_tiny_ttf_load_level1_glyphs(g_ttf_font, combined, total_count);
    
    if (result > 0) {
        if (result < total_count) {
            printf("[FONT_LOADER] Partial L1 preload: %d/%d glyphs cached (rest use stbtt streaming)\n",
                   result, total_count);
        } else {
            printf("[FONT_LOADER] Level1+ASCII glyphs loaded: %d chars\n", result);
        }
    } else {
        printf("[FONT_LOADER] Failed to load Level1+ASCII glyphs\n");
    }
    
    return result;
}

font_cache_stats_t font_priority_loader_get_stats(void)
{
    font_cache_stats_t stats = {0};
    return stats;
}
