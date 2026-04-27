#include "font_priority_loader.h"
#include "level1_data.h"
#include <stdio.h>
#include "lvgl/lvgl.h"
#include "lvgl/src/extra/libs/tiny_ttf/lv_tiny_ttf.h"

static int g_initialized = 0;
static lv_font_t *g_ttf_font = NULL;

int font_priority_loader_init(void)
{
    if (g_initialized) {
        return 0;
    }
    printf("[FONT_LOADER] Level1 chars: %d\n", level1_chars_count);
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
    
    printf("[FONT_LOADER] Loading Level1 glyph data to PSRAM (%d chars)...\n", level1_chars_count);
    
    int result = lv_tiny_ttf_load_level1_glyphs(g_ttf_font, level1_chars, level1_chars_count);
    
    if (result > 0) {
        printf("[FONT_LOADER] Level1 glyphs loaded: %d chars\n", result);
    } else {
        printf("[FONT_LOADER] Failed to load Level1 glyphs\n");
    }
    
    return result;
}

font_cache_stats_t font_priority_loader_get_stats(void)
{
    font_cache_stats_t stats = {0};
    return stats;
}
