/**
 * @file lv_tiny_ttf.h
 *
 */

#ifndef LV_TINY_TTF_H
#define LV_TINY_TTF_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "../../../lvgl.h"

#if LV_USE_TINY_TTF

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/

#if LV_TINY_TTF_FILE_SUPPORT
/* create a font from the specified file or path with the specified line height.*/
lv_font_t * lv_tiny_ttf_create_file(const char * path, lv_coord_t font_size);

/* create a font from the specified file or path with the specified line height with the specified cache size.*/
lv_font_t * lv_tiny_ttf_create_file_ex(const char * path, lv_coord_t font_size);
#endif /*LV_TINY_TTF_FILE_SUPPORT*/

/* create a font from the specified data pointer with the specified line height.*/
lv_font_t * lv_tiny_ttf_create_data(const void * data, size_t data_size, lv_coord_t font_size);

/* create a font from the specified data pointer with the specified line height and the specified cache size.*/
lv_font_t * lv_tiny_ttf_create_data_ex(const void * data, size_t data_size, lv_coord_t font_size, size_t cache_size_unused);

/* set the size of the font to a new font_size*/
void lv_tiny_ttf_set_size(lv_font_t * font, lv_coord_t font_size);

/* Debug: track dsc_cb call phases and stats */
void lv_tiny_ttf_set_dsc_phase(int phase); /* 1=layout, 2=render */
int lv_tiny_ttf_get_dsc_stats(int *total, int *hits, int *misses, int *non_cjk);
void lv_tiny_ttf_reset_dsc_stats(void);

/* destroy a font previously created with lv_tiny_ttf_create_xxxx()*/
void lv_tiny_ttf_destroy(lv_font_t * font);

/* load Level1 glyphs to PSRAM cache */
int lv_tiny_ttf_load_level1_glyphs(lv_font_t *font, const uint32_t *unicode_list, uint16_t count);

/* get cached glyph data from PSRAM */
uint8_t * lv_tiny_ttf_get_cached_glyph_data(lv_font_t *font, uint16_t glyph_index, uint32_t *out_size);

/* bitmap cache: reset all cached bitmaps (call on EPUB close) */
void lv_tiny_ttf_bitmap_cache_reset(void);

/* bitmap cache: mark start of page render for timing */
void lv_tiny_ttf_bitmap_page_start(void);

/* bitmap cache: mark end of page render, print timing stats */
void lv_tiny_ttf_bitmap_page_end(void);

/**********************
 *      MACROS
 **********************/

#endif /*LV_USE_TINY_TTF*/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_TINY_TTF_H*/
