#ifndef FONT_PRIORITY_LOADER_H
#define FONT_PRIORITY_LOADER_H

#include <stdint.h>
#include "lvgl/lvgl.h"

typedef struct {
    int total_loaded;
} font_cache_stats_t;

int font_priority_loader_init(void);
void font_priority_loader_set_font(lv_font_t *font);
int font_priority_loader_preload(void);
font_cache_stats_t font_priority_loader_get_stats(void);

#endif
