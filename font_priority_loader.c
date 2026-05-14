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

static int g_initialized = 0;
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
    
    /* Combine ASCII + Level1 CJK into a single array for batch preload.
     * This ensures ASCII glyph data is also cached in PSRAM,
     * eliminating SD card reads during first page layout. */
    int total_count = level1_chars_count + ASCII_COUNT;
    uint32_t *combined = (uint32_t *)malloc(total_count * sizeof(uint32_t));
    if (!combined) {
        printf("[FONT_LOADER] Warning: malloc failed, loading CJK only\n");
        return lv_tiny_ttf_load_level1_glyphs(g_ttf_font, level1_chars, level1_chars_count);
    }
    
    /* ASCII chars first (0x20-0x7E) */
    for (int i = 0; i < ASCII_COUNT; i++) {
        combined[i] = ASCII_START + i;
    }
    /* Then Level1 CJK chars */
    memcpy(combined + ASCII_COUNT, level1_chars, level1_chars_count * sizeof(uint32_t));
    
    printf("[FONT_LOADER] Loading Level1+ASCII glyphs to PSRAM (%d CJK + %d ASCII = %d total)...\n",
           level1_chars_count, ASCII_COUNT, total_count);
    
    int result = lv_tiny_ttf_load_level1_glyphs(g_ttf_font, combined, total_count);
    free(combined);
    
    if (result > 0) {
        printf("[FONT_LOADER] Level1+ASCII glyphs loaded: %d chars\n", result);
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
