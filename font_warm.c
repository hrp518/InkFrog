#include "font_warm.h"
#include "font_priority_loader.h"
#include "settings_screen.h"
#include "settings_storage.h"
#include "http_server.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/extra/libs/tiny_ttf/lv_tiny_ttf.h"
#include "fs/fatfs/ff.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define FONT_WARM_BOOT_DELAY_MS  5000
#define FONT_WARM_RETRY_MS       3000
#define FONT_WARM_PRELOAD_SIZE   16

static char s_cached_path[256];
static char s_pending_path[256];
static lv_timer_t * s_warm_timer;
static volatile int s_warm_busy;

static void font_warm_normalize_path(char * path)
{
    if(!path || path[0] == '\0') return;
    if(strncmp(path, "0:Font/", 7) == 0) {
        memmove(path + 2, path + 1, strlen(path));
        path[1] = '/';
    }
}

static int font_warm_stat_file(const char * path)
{
    FILINFO fno;
    return (f_stat(path, &fno) == FR_OK) ? 0 : -1;
}

void font_warm_l1glyf_path_for_ttf(const char * ttf_path, char * out, size_t out_sz)
{
    if(!ttf_path || !out || out_sz == 0) return;
    out[0] = '\0';

    const char * slash = strrchr(ttf_path, '/');
    const char * base = slash ? (slash + 1) : ttf_path;
    char stem[128];
    strncpy(stem, base, sizeof(stem) - 1);
    stem[sizeof(stem) - 1] = '\0';
    char * dot = strrchr(stem, '.');
    if(dot) *dot = '\0';

    snprintf(out, out_sz, "0:/Font/.l1glyf/%s.l1glyf", stem);
}

int font_warm_l1glyf_exists(const char * ttf_path)
{
    char cache_path[256];
    if(!ttf_path || ttf_path[0] == '\0') return 0;

    font_warm_l1glyf_path_for_ttf(ttf_path, cache_path, sizeof(cache_path));
    if(font_warm_stat_file(cache_path) == 0) return 1;

    char sidecar[256];
    strncpy(sidecar, ttf_path, sizeof(sidecar) - 1);
    sidecar[sizeof(sidecar) - 1] = '\0';
    font_warm_normalize_path(sidecar);
    char * dot = strrchr(sidecar, '.');
    if(dot) {
        strcpy(dot, ".l1glyf");
    } else {
        strncat(sidecar, ".l1glyf", sizeof(sidecar) - strlen(sidecar) - 1);
    }
    return font_warm_stat_file(sidecar) == 0;
}

int font_warm_path_matches(const char * ttf_path)
{
    if(!ttf_path || s_cached_path[0] == '\0') return 0;
    char a[256], b[256];
    strncpy(a, ttf_path, sizeof(a) - 1);
    a[sizeof(a) - 1] = '\0';
    strncpy(b, s_cached_path, sizeof(b) - 1);
    b[sizeof(b) - 1] = '\0';
    font_warm_normalize_path(a);
    font_warm_normalize_path(b);
    return strcasecmp(a, b) == 0;
}

const char * font_warm_cached_path(void)
{
    return s_cached_path[0] ? s_cached_path : NULL;
}

int font_warm_is_ready(void)
{
    return s_cached_path[0] != '\0';
}

static int font_warm_scan_smallest(char * out, size_t out_sz)
{
    DIR dir;
    FILINFO fno;
    FSIZE_t best = (FSIZE_t)-1;
    char best_name[128] = "";

    if(f_opendir(&dir, "0:/Font") != FR_OK) {
        return -1;
    }
    for(;;) {
        if(f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) break;
        if(fno.fattrib & AM_DIR) continue;
        const char * ext = strrchr(fno.fname, '.');
        if(!ext || strcasecmp(ext, ".ttf") != 0) continue;
        if(fno.fsize < best) {
            best = fno.fsize;
            strncpy(best_name, fno.fname, sizeof(best_name) - 1);
            best_name[sizeof(best_name) - 1] = '\0';
        }
    }
    f_closedir(&dir);
    if(best_name[0] == '\0') return -1;
    snprintf(out, out_sz, "0:/Font/%s", best_name);
    return 0;
}

int font_warm_resolve_reader_path(char * out, size_t out_sz)
{
    char saved[256] = "";
    if(settings_get_string("font", "path", saved, sizeof(saved)) == 0 && saved[0]) {
        font_warm_normalize_path(saved);
        if(font_warm_stat_file(saved) == 0) {
            strncpy(out, saved, out_sz - 1);
            out[out_sz - 1] = '\0';
            return 0;
        }
    }
    const char * sel = settings_get_selected_font();
    if(sel && sel[0]) {
        strncpy(saved, sel, sizeof(saved) - 1);
        saved[sizeof(saved) - 1] = '\0';
        font_warm_normalize_path(saved);
        if(font_warm_stat_file(saved) == 0) {
            strncpy(out, saved, out_sz - 1);
            out[out_sz - 1] = '\0';
            return 0;
        }
    }
    return font_warm_scan_smallest(out, out_sz);
}

static void font_warm_run_locked(const char * path)
{
    uint32_t t0 = lv_tick_get();
    printf("[FONT_WARM] Start L1 preload: %s\n", path);

    lv_font_t * font = lv_tiny_ttf_create_file_ex(path, FONT_WARM_PRELOAD_SIZE);
    if(!font) {
        printf("[FONT_WARM] create_file_ex failed\n");
        return;
    }
    font_priority_loader_init();
    font_priority_loader_set_font(font);
    int loaded = font_priority_loader_preload();
    font_priority_loader_set_font(NULL);
    lv_tiny_ttf_destroy(font);

    if(loaded > 0) {
        strncpy(s_cached_path, path, sizeof(s_cached_path) - 1);
        s_cached_path[sizeof(s_cached_path) - 1] = '\0';
        font_warm_normalize_path(s_cached_path);
        printf("[FONT_WARM] Done: %d glyphs in %lums path=%s\n",
               loaded, (unsigned long)(lv_tick_get() - t0), s_cached_path);
    } else {
        printf("[FONT_WARM] Preload failed (%d)\n", loaded);
    }
}

static void font_warm_timer_cb(lv_timer_t * timer)
{
    (void)timer;
    s_warm_timer = NULL;

    if(http_server_is_running()) {
        printf("[FONT_WARM] HTTP active, retry in %dms\n", FONT_WARM_RETRY_MS);
        s_warm_timer = lv_timer_create(font_warm_timer_cb, FONT_WARM_RETRY_MS, NULL);
        lv_timer_set_repeat_count(s_warm_timer, 1);
        return;
    }
    if(s_warm_busy) return;
    s_warm_busy = 1;

    char path[256];
    if(s_pending_path[0]) {
        strncpy(path, s_pending_path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        s_pending_path[0] = '\0';
    } else if(font_warm_resolve_reader_path(path, sizeof(path)) != 0) {
        printf("[FONT_WARM] No TTF to warm\n");
        s_warm_busy = 0;
        return;
    }
    font_warm_normalize_path(path);

    if(font_warm_is_ready() && font_warm_path_matches(path)) {
        printf("[FONT_WARM] Already warm for %s\n", path);
        s_warm_busy = 0;
        return;
    }

    if(font_warm_is_ready()) {
        lv_tiny_ttf_release_reader_cache();
        s_cached_path[0] = '\0';
    }

    font_warm_run_locked(path);
    s_warm_busy = 0;
}

static void font_warm_schedule_internal(uint32_t delay_ms)
{
    if(s_warm_timer) {
        lv_timer_del(s_warm_timer);
        s_warm_timer = NULL;
    }
    s_warm_timer = lv_timer_create(font_warm_timer_cb, delay_ms, NULL);
    lv_timer_set_repeat_count(s_warm_timer, 1);
}

void font_warm_schedule_boot(void)
{
    printf("[FONT_WARM] Scheduled boot warm in %dms\n", FONT_WARM_BOOT_DELAY_MS);
    font_warm_schedule_internal(FONT_WARM_BOOT_DELAY_MS);
}

void font_warm_request(const char * ttf_path)
{
    if(ttf_path && ttf_path[0]) {
        strncpy(s_pending_path, ttf_path, sizeof(s_pending_path) - 1);
        s_pending_path[sizeof(s_pending_path) - 1] = '\0';
        font_warm_normalize_path(s_pending_path);
    } else {
        s_pending_path[0] = '\0';
    }
    font_warm_schedule_internal(500);
}
