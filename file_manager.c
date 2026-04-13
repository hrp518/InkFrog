/**
 * @file file_manager.c
 * @brief 文件管理器实现 - LVGL文件系统GUI组件
 * 
 * 功能：
 * - 目录浏览（基于lv_list）
 * - 文件操作（打开文本、重命名、删除）
 * 
 * 硬件：240x415 单色墨水屏 (EPD_3IN52)
 */

#include "file_manager.h"
#include "lvgl/lvgl.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "fs/fatfs/ff.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Tiny-TTF支持 - 用于从SD卡加载TTF字体 */
#if LV_USE_TINY_TTF
#include "lvgl/src/extra/libs/tiny_ttf/lv_tiny_ttf.h"
#include "sys/sys_heap.h"
#endif

/*====================
 *    全局变量
 *====================*/

static char current_path[256] = "/";
static lv_obj_t *fm_screen = NULL;
static lv_obj_t *fm_list = NULL;
static lv_obj_t *path_label = NULL;
static lv_obj_t *viewer_screen = NULL;
static lv_obj_t *viewer_textarea = NULL;
static char selected_filepath[512] = {0};

/*====================
 *    分页相关变量
 *====================*/

#define FILES_PER_PAGE 10      /* 每页显示的文件数 */
#define MAX_FILE_ENTRIES 500   /* 最大支持的文件条目数 */

typedef struct {
    char name[256];     /* 文件名 */
    char full_path[512];/* 完整路径 */
    uint8_t is_dir;     /* 是否为目录 */
} FileEntry;

static FileEntry *file_entries = NULL;  /* 文件条目数组 */
static int total_file_count = 0;          /* 总文件数 */
static int current_page = 0;              /* 当前页码 (从0开始) */
static int total_pages = 0;               /* 总页数 */
static lv_obj_t *page_indicator = NULL;   /* 页码指示器容器 */
static lv_obj_t **page_dots = NULL;       /* 页码圆点数组 */
static lv_obj_t *prev_page_btn = NULL;    /* 上一页按钮 */
static lv_obj_t *next_page_btn = NULL;    /* 下一页按钮 */

/*====================
 *   字体懒加载变量
 *====================*/

#if LV_USE_TINY_TTF
static lv_font_t *custom_ttf_font = NULL;     /* 动态加载的TTF字体 */
static bool ttf_load_attempted = false;        /* 是否已尝试加载TTF */
static char ttf_file_path[256] = {0};      /* 选中的TTF文件路径 */
static uint8_t *s_ttf_data = NULL;             /* 缓存TTF文件数据（PSRAM） */
static size_t s_ttf_data_size = 0;             /* TTF数据大小 */
#endif

/*====================
 *   函数声明
 *====================*/

static void refresh_file_list(const char *dir_path);
static void epd_disable_all_animations_recursive(lv_obj_t *obj);  /* 前向声明 */
static void show_file_action_menu(const char *filename);
static void open_text_viewer(const char *filepath);
static void show_delete_confirm(const char *filepath);
static void close_action_dialog(void);
static void show_rename_keyboard(void *user_data);
static void rename_confirm_cb(lv_event_t *e);
static void rename_cancel_cb(lv_event_t *e);
void file_manager_close(void);

/*====================
 *   分页功能函数声明
 *====================*/

static void init_pagination(void);
static void deinit_pagination(void);
static void load_file_entries(const char *dir_path);
static void display_current_page(void);
static void create_page_indicator(void);
static void update_page_indicator(void);
static void prev_page_cb(lv_event_t *e);
static void next_page_cb(lv_event_t *e);

/*====================
 *   触摸滑动相关变量（已迁移到lv_port_indev.c驱动层）
 *====================*/

// touch_start_y, touch_moved 已迁移到驱动层，不再需要
#define SWIPE_THRESHOLD 8           /* 滑动阈值（像素） */

/*====================
 *   回调函数
 *====================*/

/* 滑动区域回调 - 由驱动层在释放时调用（lv_port_indev.c）
 * delta_y > 0: 上滑（下一页）, delta_y < 0: 下滑（上一页）
 */
static void swipe_handler(int32_t delta_y)
{
    printf("[FM] Swipe: delta=%d, page=%d/%d\n", delta_y, current_page, total_pages);
    
    if (delta_y > SWIPE_THRESHOLD) {
        if (current_page < total_pages - 1) {
            current_page++;
            epd_set_content_dirty();
            display_current_page();
        }
    } else if (delta_y < -SWIPE_THRESHOLD) {
        if (current_page > 0) {
            current_page--;
            epd_set_content_dirty();
            display_current_page();
        }
    }
}

/* 触摸事件回调 - 注册到fm_screen，处理触摸区域检测和按钮点击
 * 滑动检测已移至驱动层(swipe_handler)，此处只做区域判定
 */
static void touch_event_cb(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target(e);
    
    /* 遍历祖先链，检查触摸起点是否在fm_list区域 */
    lv_obj_t *current = target;
    uint8_t in_fm_list = 0;
    while (current != NULL) {
        if (current == fm_list) {
            in_fm_list = 1;
            break;
        }
        current = lv_obj_get_parent(current);
    }
    
    /* 设置滑动区域边界：只有起点在fm_list区域内才启用滑动检测 */
    if (in_fm_list) {
        lv_area_t a;
        lv_obj_get_coords(fm_list, &a);
        touch_set_swipe_area(a.y1, a.y2);
    } else {
        touch_set_swipe_area(0, 0);  // 禁用滑动
    }
    
    /* 点击事件由lv_list按钮自己的回调处理（dir_btn_cb/file_btn_cb），
     * 此处不做额外处理
     */
}

static void physical_back_btn_handler(void)
{
    // 物理返回按键处理 - 抬手触发
    if (strcmp(current_path, "/") == 0) {
        // 已在根目录，退出文件管理器返回首页
        printf("[FM] Physical back: at root, closing file manager\n");
        file_manager_close();
    } else {
        // 返回上级目录
        printf("[FM] Physical back: going to parent directory\n");
        char *last_slash = strrchr(current_path, '/');
        if (last_slash != current_path) {
            *last_slash = '\0';
        } else {
            current_path[1] = '\0';
        }
        refresh_file_list(current_path);
        char path_text[128];
        snprintf(path_text, sizeof(path_text), "当前: %s", current_path);
        lv_label_set_text(path_label, path_text);
        epd_mark_refresh_pending();
    }
}

static void close_btn_cb(lv_event_t *e)
{
    (void)e;
    file_manager_close();
}

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    char *last_slash = strrchr(current_path, '/');
    if (last_slash != current_path) {
        *last_slash = '\0';
    } else {
        current_path[1] = '\0';
    }
    refresh_file_list(current_path);
    char path_text[128];
    snprintf(path_text, sizeof(path_text), "当前: %s", current_path);
    lv_label_set_text(path_label, path_text);
}

static void dir_btn_cb(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target(e);
    char *path = (char*)lv_obj_get_user_data(target);
    if (path) {
        strncpy(current_path, path, sizeof(current_path) - 1);
        current_path[sizeof(current_path) - 1] = '\0';
        refresh_file_list(current_path);
        char path_text[128];
        snprintf(path_text, sizeof(path_text), "当前: %s", current_path);
        lv_label_set_text(path_label, path_text);
        free(path);
    }
}

static void file_btn_cb(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target(e);
    char *path = (char*)lv_obj_get_user_data(target);
    if (path) {
        strncpy(selected_filepath, path, sizeof(selected_filepath) - 1);
        selected_filepath[sizeof(selected_filepath) - 1] = '\0';
        const char *fname = strrchr(path, '/');
        if (fname) fname++; else fname = path;
        show_file_action_menu(fname);
        free(path);
    }
}

static void viewer_back_cb(lv_event_t *e)
{
    (void)e;
    if (viewer_textarea) {
        lv_obj_del(viewer_textarea);
        viewer_textarea = NULL;
    }
    if (viewer_screen) {
        lv_obj_del(viewer_screen);
        viewer_screen = NULL;
    }
    lv_disp_load_scr(fm_screen);
    epd_mark_refresh_pending();
}

static void delete_confirm_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    // 检查是哪个按钮被点击 - 通过user_data传递索引
    uint32_t btn_idx = (uint32_t)lv_obj_get_user_data(btn);
    if (btn_idx == 0) {
        // 确认删除
        FRESULT res = f_unlink(selected_filepath);
        if (res == FR_OK) {
            printf("[FM] File deleted: %s\n", selected_filepath);
            refresh_file_list(current_path);
        } else {
            printf("[FM] Delete failed: error %d\n", res);
        }
    }
    epd_mark_refresh_pending();
}

static void action_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    uint32_t btn_idx = (uint32_t)lv_obj_get_user_data(btn);
    
    // 关闭对话框
    close_action_dialog();
    
    if (btn_idx == 0) {
        // 打开文本
        open_text_viewer(selected_filepath);
    } else if (btn_idx == 1) {
        // 删除
        show_delete_confirm(selected_filepath);
    } else if (btn_idx == 2) {
        // 重命名 - 使用lv_async_call延时创建键盘，确保action_dialog被彻底销毁
        lv_async_call(show_rename_keyboard, (void*)selected_filepath);
    }
    // 取消按钮不做任何操作
}

/*====================
 *   重命名功能
 *====================*/

static lv_obj_t *rename_screen = NULL;
static lv_obj_t *rename_ta = NULL;
static lv_obj_t *rename_kb = NULL;
static char rename_old_path[512] = {0};

/**
 * @brief 打印LVGL堆内存状态的Debug探针
 */
static void print_mem_debug(const char *tag)
{
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    printf("[FM MEM] %s - Free: %d bytes, Max contig: %d bytes, Used: %d%%\r\n",
           tag, (int)mon.free_size, (int)mon.max_free, (int)mon.used_pct);
}

/**
 * @brief 异步创建重命名键盘的回调
 * @details 使用lv_async_call确保action_dialog被彻底销毁后再创建键盘
 */
static void show_rename_keyboard(void *user_data)
{
    const char *filepath = (const char*)user_data;
    if (!filepath) return;
    
    // 记录原文件路径
    strncpy(rename_old_path, filepath, sizeof(rename_old_path) - 1);
    rename_old_path[sizeof(rename_old_path) - 1] = '\0';
    
    // Debug探针1: 创建rename_screen之前
    print_mem_debug("Before create rename_screen");
    
    // 创建重命名界面
    rename_screen = lv_obj_create(NULL);
    lv_obj_set_size(rename_screen, 240, 415);
    lv_obj_set_style_bg_color(rename_screen, lv_color_white(), 0);
    epd_disable_all_animations_recursive(rename_screen);
    
    // 标题
    lv_obj_t *title = lv_label_create(rename_screen);
    lv_label_set_text(title, "重命名");
    lv_obj_set_style_text_font(title, get_reader_font(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // 提取文件名
    const char *fname = strrchr(filepath, '/');
    if (fname) fname++; else fname = filepath;
    
    // 文本输入框
    rename_ta = lv_textarea_create(rename_screen);
    lv_obj_set_size(rename_ta, 200, 40);
    lv_obj_align(rename_ta, LV_ALIGN_TOP_MID, 0, 50);
    lv_textarea_set_text(rename_ta, fname);
    lv_textarea_set_cursor_pos(rename_ta, strlen(fname));
    lv_obj_set_style_text_font(rename_ta, get_reader_font(), 0);
    epd_disable_all_animations_recursive(rename_ta);
    
    // Debug探针2: 创建kb之前
    print_mem_debug("Before create keyboard");
    
    // 键盘 - 使用lv_timer_handler确保action_dialog内存释放完成
    for (int i = 0; i < 5; i++) {
        lv_timer_handler();
    }
    
    print_mem_debug("After lv_timer_handler before kb create");
    
    // 创建键盘
    rename_kb = lv_keyboard_create(rename_screen);
    lv_obj_set_size(rename_kb, 230, 180);
    lv_obj_align(rename_kb, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_keyboard_set_textarea(rename_kb, rename_ta);
    epd_disable_all_animations_recursive(rename_kb);
    
    // Debug探针3: 创建kb之后
    print_mem_debug("After create keyboard");
    
    // 确认按钮
    lv_obj_t *confirm_btn = lv_btn_create(rename_screen);
    lv_obj_set_size(confirm_btn, 60, 30);
    lv_obj_align(confirm_btn, LV_ALIGN_TOP_RIGHT, -10, 50);
    epd_disable_all_animations_recursive(confirm_btn);
    lv_obj_set_user_data(confirm_btn, (void*)1);
    lv_obj_add_event_cb(confirm_btn, rename_confirm_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *confirm_label = lv_label_create(confirm_btn);
    lv_label_set_text(confirm_label, "确认");
    lv_obj_set_style_text_font(confirm_label, get_reader_font(), 0);
    lv_obj_center(confirm_label);
    
    // 取消按钮
    lv_obj_t *cancel_btn = lv_btn_create(rename_screen);
    lv_obj_set_size(cancel_btn, 60, 30);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 10, 50);
    epd_disable_all_animations_recursive(cancel_btn);
    lv_obj_set_user_data(cancel_btn, (void*)0);
    lv_obj_add_event_cb(cancel_btn, rename_cancel_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "取消");
    lv_obj_set_style_text_font(cancel_label, get_reader_font(), 0);
    lv_obj_center(cancel_label);
    
    lv_disp_load_scr(rename_screen);
    epd_mark_refresh_pending();
    
    printf("[FM] Rename keyboard shown for: %s\n", filepath);
}

static void rename_confirm_cb(lv_event_t *e)
{
    (void)e;
    const char *new_name = lv_textarea_get_text(rename_ta);
    if (!new_name || strlen(new_name) == 0) {
        printf("[FM] Rename failed: empty name\n");
        return;
    }
    
    // 提取目录路径
    char dir_path[512] = {0};
    strncpy(dir_path, rename_old_path, sizeof(dir_path) - 1);
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) *last_slash = '\0';
    
    // 构建新路径
    char new_path[512];
    snprintf(new_path, sizeof(new_path), "%s/%s", dir_path, new_name);
    
    // 执行重命名
    FRESULT res = f_rename(rename_old_path, new_path);
    if (res == FR_OK) {
        printf("[FM] File renamed: %s -> %s\n", rename_old_path, new_path);
        refresh_file_list(current_path);
    } else {
        printf("[FM] Rename failed: error %d\n", res);
    }
    
    // 清理并返回
    if (rename_kb) { lv_obj_del(rename_kb); rename_kb = NULL; }
    if (rename_ta) { lv_obj_del(rename_ta); rename_ta = NULL; }
    if (rename_screen) { lv_obj_del(rename_screen); rename_screen = NULL; }
    lv_disp_load_scr(fm_screen);
    epd_mark_refresh_pending();
}

static void rename_cancel_cb(lv_event_t *e)
{
    (void)e;
    // 清理并返回
    if (rename_kb) { lv_obj_del(rename_kb); rename_kb = NULL; }
    if (rename_ta) { lv_obj_del(rename_ta); rename_ta = NULL; }
    if (rename_screen) { lv_obj_del(rename_screen); rename_screen = NULL; }
    lv_disp_load_scr(fm_screen);
    epd_mark_refresh_pending();
}

/*====================
 *   辅助函数
 *====================*/

static void epd_disable_animations(lv_obj_t *obj)
{
    lv_obj_set_style_transition(obj, NULL, LV_STATE_ANY);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    /* 禁用textarea光标闪烁动画，防止每400ms触发一次6x6小区域重绘 */
    if(lv_obj_check_type(obj, &lv_textarea_class)) {
        /* 删除该textarea上所有动画（防止del后timer仍访问已删除对象） */
        lv_anim_del(obj, NULL);
        /* 设置anim_time=0防止后续样式变化重建动画 */
        lv_obj_set_style_anim_time(obj, 0, LV_PART_CURSOR);
    }
}

static void epd_disable_all_animations_recursive(lv_obj_t *obj)
{
    epd_disable_animations(obj);
    uint32_t child_cnt = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        if (child) epd_disable_all_animations_recursive(child);
    }
}

static const char* get_file_icon(const char *filename, uint8_t is_dir)
{
    if (is_dir) return "[DIR]";
    const char *ext = strrchr(filename, '.');
    if (ext) {
        if (strcasecmp(ext, ".txt") == 0 || strcasecmp(ext, ".md") == 0) return "[TXT]";
        if (strcasecmp(ext, ".bmp") == 0 || strcasecmp(ext, ".jpg") == 0) return "[IMG]";
    }
    return "[FILE]";
}

/*====================
 *   分页功能实现
 *====================*/

/**
 * @brief 初始化分页系统
 */
static void init_pagination(void)
{
    /* 分配文件条目数组（使用PSRAM） */
    file_entries = (FileEntry *)psram_malloc(MAX_FILE_ENTRIES * sizeof(FileEntry));
    if (!file_entries) {
        printf("[FM] Failed to allocate file_entries array from PSRAM\n");
        return;
    }
    
    total_file_count = 0;
    current_page = 0;
    total_pages = 0;
    
    /* 分配页码圆点数组（最多显示5个圆点） */
    page_dots = (lv_obj_t **)malloc(5 * sizeof(lv_obj_t *));
    if (!page_dots) {
        printf("[FM] Failed to allocate page_dots array\n");
        psram_free(file_entries);
        file_entries = NULL;
        return;
    }
    
    printf("[FM] Pagination initialized (PSRAM)\n");
}

/**
 * @brief 释放分页系统资源
 */
static void deinit_pagination(void)
{
    if (file_entries) {
        psram_free(file_entries);
        file_entries = NULL;
    }
    if (page_dots) {
        free(page_dots);
        page_dots = NULL;
    }
    total_file_count = 0;
    current_page = 0;
    total_pages = 0;
}

/**
 * @brief 加载所有文件条目到内存
 */
static void load_file_entries(const char *dir_path)
{
    FRESULT res;
    FILINFO fno;
    DIR dir;
    int index = 0;
    
    if (!file_entries) return;
    
    total_file_count = 0;
    current_page = 0;
    
    printf("[FM] Loading file entries for: %s\n", dir_path);
    
    /* 如果不是根目录，添加返回按钮条目 */
    if (strcmp(dir_path, "/") != 0) {
        strncpy(file_entries[index].name, "[BACK] ...", sizeof(file_entries[index].name) - 1);
        file_entries[index].name[sizeof(file_entries[index].name) - 1] = '\0';
        file_entries[index].full_path[0] = '\0';
        file_entries[index].is_dir = 0xFF;  /* 特殊标记表示返回按钮 */
        index++;
    }
    
    /* 打开目录 */
    res = f_opendir(&dir, dir_path);
    if (res != FR_OK) {
        printf("[FM] Failed to open dir: %s, error: %d\n", dir_path, res);
        return;
    }
    
    /* 遍历目录 */
    for (;;) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) break;
        if (fno.fname[0] == '.') continue;
        if (index >= MAX_FILE_ENTRIES) {
            printf("[FM] Too many files, truncating at %d\n", MAX_FILE_ENTRIES);
            break;
        }
        
        /* 保存文件信息 */
        strncpy(file_entries[index].name, fno.fname, sizeof(file_entries[index].name) - 1);
        file_entries[index].name[sizeof(file_entries[index].name) - 1] = '\0';
        
        snprintf(file_entries[index].full_path, sizeof(file_entries[index].full_path), 
                 "%s/%s", dir_path, fno.fname);
        
        file_entries[index].is_dir = (fno.fattrib & AM_DIR) ? 1 : 0;
        
        index++;
    }
    
    f_closedir(&dir);
    total_file_count = index;
    
    /* 计算总页数 */
    if (total_file_count > 0) {
        total_pages = (total_file_count + FILES_PER_PAGE - 1) / FILES_PER_PAGE;
    } else {
        total_pages = 0;
    }
    
    printf("[FM] Loaded %d files, %d pages\n", total_file_count, total_pages);
}

/**
 * @brief 显示当前页的文件
 */
static void display_current_page(void)
{
    int start_idx = current_page * FILES_PER_PAGE;
    int end_idx = start_idx + FILES_PER_PAGE;
    
    if (end_idx > total_file_count) {
        end_idx = total_file_count;
    }
    
    printf("[FM] Displaying page %d/%d, files %d-%d\n", 
           current_page + 1, total_pages, start_idx + 1, end_idx);
    
    /* 清空列表 */
    lv_obj_clean(fm_list);
    
     /* 显示当前页的文件 */
     for (int i = start_idx; i < end_idx; i++) {
         FileEntry *entry = &file_entries[i];
         
         /* 处理返回按钮 */
         if (entry->is_dir == 0xFF) {
        lv_obj_t *btn = lv_list_add_btn(fm_list, NULL, entry->name);
        epd_disable_all_animations_recursive(btn);
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (lbl) lv_obj_set_style_text_font(lbl, get_reader_font(), 0);
        lv_obj_add_event_cb(btn, back_btn_cb, LV_EVENT_CLICKED, NULL);
        continue;
         }
         
         /* 普通文件/目录 */
         const char *icon = get_file_icon(entry->name, entry->is_dir);
         static char item_text[280];
         snprintf(item_text, sizeof(item_text), "%s %s", icon, entry->name);
         
         lv_obj_t *btn = lv_list_add_btn(fm_list, NULL, item_text);
        epd_disable_all_animations_recursive(btn);
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (lbl) lv_obj_set_style_text_font(lbl, get_reader_font(), 0);
        
        char *path_copy = malloc(strlen(entry->full_path) + 1);
        if (path_copy) {
            strcpy(path_copy, entry->full_path);
            lv_obj_set_user_data(btn, path_copy);
        }
        
        if (entry->is_dir) {
            lv_obj_add_event_cb(btn, dir_btn_cb, LV_EVENT_CLICKED, NULL);
        } else {
            lv_obj_add_event_cb(btn, file_btn_cb, LV_EVENT_CLICKED, NULL);
        }
    }
    
    /* 更新页码指示器 */
    update_page_indicator();
}

/**
 * @brief 创建页码指示器（竖排放置在左侧，仅显示圆点）
 */
static void create_page_indicator(void)
{
    /* 创建指示器容器 - 竖排放置在左侧 */
    page_indicator = lv_obj_create(fm_screen);
    lv_obj_set_size(page_indicator, 15, 300);  /* 窄长条 */
    lv_obj_align(page_indicator, LV_ALIGN_LEFT_MID, 2, 0);  /* 左侧居中 */
    lv_obj_set_style_bg_color(page_indicator, lv_color_white(), 0);
    lv_obj_set_style_border_width(page_indicator, 0, 0);
    lv_obj_set_style_pad_all(page_indicator, 0, 0);
    /* 禁用点击和滚动 */
    lv_obj_clear_flag(page_indicator, LV_OBJ_FLAG_CLICKABLE);
    epd_disable_animations(page_indicator);
    
    /* 初始化圆点数组 */
    for (int i = 0; i < 5; i++) {
        page_dots[i] = NULL;
    }
    
    prev_page_btn = NULL;  /* 不再使用按钮翻页 */
    next_page_btn = NULL;
}

/**
 * @brief 更新页码指示器（竖排放置）
 */
static void update_page_indicator(void)
{
    if (!page_indicator || !page_dots) return;
    
    /* 删除旧的圆点 */
    for (int i = 0; i < 5; i++) {
        if (page_dots[i]) {
            lv_obj_del(page_dots[i]);
            page_dots[i] = NULL;
        }
    }
    
    if (total_pages <= 0) return;
    
    /* 计算要显示的页码范围 */
    int start_dot = 0;
    int dots_to_show = total_pages;
    
    /* 如果页数超过5，只显示5个圆点 */
    if (total_pages > 5) {
        dots_to_show = 5;
        if (current_page < 2) {
            start_dot = 0;
        } else if (current_page >= total_pages - 3) {
            start_dot = total_pages - 5;
        } else {
            start_dot = current_page - 2;
        }
    }
    
    /* 创建圆点（竖排放置在中间） */
    int dot_size = 8;
    int spacing = 10;
    int total_height = dots_to_show * dot_size + (dots_to_show - 1) * spacing;
    int start_y = (300 - total_height) / 2;  /* 在300高度的容器中居中 */
    
    for (int i = 0; i < dots_to_show; i++) {
        int page_num = start_dot + i;
        bool is_current = (page_num == current_page);
        
        page_dots[i] = lv_obj_create(page_indicator);
        lv_obj_set_size(page_dots[i], dot_size, dot_size);
        lv_obj_set_pos(page_dots[i], 3, start_y + i * (dot_size + spacing));
        lv_obj_set_style_radius(page_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(page_dots[i], is_current ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_border_color(page_dots[i], lv_color_black(), 0);
        lv_obj_set_style_border_width(page_dots[i], 1, 0);
        epd_disable_animations(page_dots[i]);
    }
}

/**
 * @brief 上一页按钮回调
 */
static void prev_page_cb(lv_event_t *e)
{
    (void)e;
    if (current_page > 0) {
        current_page--;
        display_current_page();
        epd_mark_refresh_pending();
    }
}

/**
 * @brief 下一页按钮回调
 */
static void next_page_cb(lv_event_t *e)
{
    (void)e;
    if (current_page < total_pages - 1) {
        current_page++;
        display_current_page();
        epd_mark_refresh_pending();
    }
}

/*====================
 *   刷新文件列表
 *====================*/

static void refresh_file_list(const char *dir_path)
{
    printf("[FM] refresh_file_list called for: %s\n", dir_path);
    
    /* 加载所有文件条目 */
    load_file_entries(dir_path);
    
    /* 显示第一页 */
    current_page = 0;
    display_current_page();
    
    printf("[FM] refresh_file_list done\n");
}

/*====================
 *   文件操作菜单
 *====================*/

static lv_obj_t *action_dialog = NULL;

static void close_action_dialog(void)
{
    if (action_dialog) {
        lv_obj_del(action_dialog);
        action_dialog = NULL;
    }
}

static void action_btn_cb(lv_event_t *e);

static void show_file_action_menu(const char *filename)
{
    close_action_dialog();
    
    action_dialog = lv_obj_create(fm_screen);
    lv_obj_set_size(action_dialog, 200, 150);
    lv_obj_align(action_dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(action_dialog, lv_color_white(), 0);
    epd_disable_all_animations_recursive(action_dialog);
    
    lv_obj_t *title = lv_label_create(action_dialog);
    lv_label_set_text(title, "文件操作");
    lv_obj_set_style_text_font(title, get_reader_font(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);
    
    lv_obj_t *fname_label = lv_label_create(action_dialog);
    lv_label_set_text(fname_label, filename);
    lv_obj_set_style_text_font(fname_label, get_reader_font(), 0);
    lv_label_set_long_mode(fname_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(fname_label, 180);
    lv_obj_align(fname_label, LV_ALIGN_TOP_MID, 0, 25);
    
    // 打开文本按钮
    lv_obj_t *btn1 = lv_btn_create(action_dialog);
    lv_obj_set_size(btn1, 80, 30);
    lv_obj_align(btn1, LV_ALIGN_TOP_LEFT, 10, 55);
    epd_disable_all_animations_recursive(btn1);
    lv_obj_set_user_data(btn1, (void*)0);
    lv_obj_add_event_cb(btn1, action_btn_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *btn1_label = lv_label_create(btn1);
    lv_label_set_text(btn1_label, "打开文本");
    lv_obj_set_style_text_font(btn1_label, get_reader_font(), 0);
    lv_obj_center(btn1_label);
    
    // 删除按钮
    lv_obj_t *btn2 = lv_btn_create(action_dialog);
    lv_obj_set_size(btn2, 80, 30);
    lv_obj_align(btn2, LV_ALIGN_TOP_RIGHT, -10, 55);
    epd_disable_all_animations_recursive(btn2);
    lv_obj_set_user_data(btn2, (void*)1);
    lv_obj_add_event_cb(btn2, action_btn_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *btn2_label = lv_label_create(btn2);
    lv_label_set_text(btn2_label, "删除");
    lv_obj_set_style_text_font(btn2_label, get_reader_font(), 0);
    lv_obj_center(btn2_label);
    
    // 取消按钮
    lv_obj_t *btn3 = lv_btn_create(action_dialog);
    lv_obj_set_size(btn3, 80, 30);
    lv_obj_align(btn3, LV_ALIGN_BOTTOM_MID, 0, -10);
    epd_disable_all_animations_recursive(btn3);
    lv_obj_set_user_data(btn3, (void*)3);
    lv_obj_add_event_cb(btn3, action_btn_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *btn3_label = lv_label_create(btn3);
    lv_label_set_text(btn3_label, "取消");
    lv_obj_set_style_text_font(btn3_label, get_reader_font(), 0);
    lv_obj_center(btn3_label);
    
    // 重命名按钮
    lv_obj_t *btn4 = lv_btn_create(action_dialog);
    lv_obj_set_size(btn4, 80, 30);
    lv_obj_align(btn4, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    epd_disable_all_animations_recursive(btn4);
    lv_obj_set_user_data(btn4, (void*)2);
    lv_obj_add_event_cb(btn4, action_btn_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *btn4_label = lv_label_create(btn4);
    lv_label_set_text(btn4_label, "重命名");
    lv_obj_set_style_text_font(btn4_label, get_reader_font(), 0);
    lv_obj_center(btn4_label);
    
    epd_mark_refresh_pending();
}

/*====================
 *   文本查看器
 *====================*/

static void open_text_viewer(const char *filepath)
{
    FRESULT res;
    FIL fp;
    UINT br;
    static char text_content[2048];
    
    res = f_open(&fp, filepath, FA_READ);
    if (res != FR_OK) {
        printf("[FM] Failed to open file: %s, error: %d\n", filepath, res);
        return;
    }
    
    memset(text_content, 0, sizeof(text_content));
    res = f_read(&fp, text_content, sizeof(text_content) - 1, &br);
    f_close(&fp);
    
    if (res != FR_OK) {
        printf("[FM] Failed to read file, error: %d\n", res);
        return;
    }
    
    viewer_screen = lv_obj_create(NULL);
    
    lv_obj_t *back_btn = lv_btn_create(viewer_screen);
    lv_obj_set_size(back_btn, 60, 30);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 5, 5);
    epd_disable_all_animations_recursive(back_btn);
    
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "返回");
    lv_obj_set_style_text_font(back_label, get_reader_font(), 0);
    lv_obj_add_event_cb(back_btn, viewer_back_cb, LV_EVENT_CLICKED, NULL);
    
    const char *fname = strrchr(filepath, '/');
    if (fname) fname++; else fname = filepath;
    
    lv_obj_t *title = lv_label_create(viewer_screen);
    lv_label_set_text(title, fname);
    lv_obj_set_style_text_font(title, get_reader_font(), 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, 160);
    lv_obj_align_to(title, back_btn, LV_ALIGN_OUT_RIGHT_MID, 5, 0);
    
    viewer_textarea = lv_textarea_create(viewer_screen);
    lv_obj_set_size(viewer_textarea, 230, 340);
    lv_obj_align(viewer_textarea, LV_ALIGN_BOTTOM_MID, 0, -10);
    epd_disable_all_animations_recursive(viewer_textarea);
    
    lv_textarea_set_text(viewer_textarea, text_content);
    lv_textarea_set_cursor_pos(viewer_textarea, 0);
    lv_obj_set_style_text_font(viewer_textarea, get_reader_font(), 0);
    lv_obj_add_flag(viewer_textarea, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_disp_load_scr(viewer_screen);
    epd_mark_refresh_pending();
}

/*====================
 *   删除确认对话框
 *====================*/

static void show_delete_confirm(const char *filepath)
{
    const char *fname = strrchr(filepath, '/');
    if (fname) fname++; else fname = filepath;
    
    static const char *btns[] = {"确认删除", "取消", ""};
    char confirm_text[128];
    snprintf(confirm_text, sizeof(confirm_text), "删除 %s ?", fname);
    
    lv_obj_t *mbox = lv_msgbox_create(fm_screen, "删除确认", confirm_text, btns, false);
    epd_disable_all_animations_recursive(mbox);
    lv_obj_set_style_text_font(lv_msgbox_get_title(mbox), get_reader_font(), 0);
    lv_obj_set_style_text_font(lv_msgbox_get_text(mbox), get_reader_font(), 0);
    lv_obj_set_width(mbox, 220);
    lv_obj_align(mbox, LV_ALIGN_CENTER, 0, 0);
    
    lv_obj_add_event_cb(mbox, delete_confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
    epd_mark_refresh_pending();
}

/*====================
 *   关闭文件管理器
 *====================*/

void file_manager_close(void)
{
    /* 先清空滑动回调，防止驱动层在对象销毁后访问 */
    touch_clear_swipe_callback();
    
    if (viewer_textarea) { lv_obj_del(viewer_textarea); viewer_textarea = NULL; }
    if (viewer_screen) { lv_obj_del(viewer_screen); viewer_screen = NULL; }
    if (fm_list) { lv_obj_del(fm_list); fm_list = NULL; }
    if (fm_screen) { lv_obj_del(fm_screen); fm_screen = NULL; }
    
    /* 清理分页系统资源 */
    page_indicator = NULL;
    prev_page_btn = NULL;
    next_page_btn = NULL;
    deinit_pagination();
    
    printf("[FM] File Manager closed\n");
}

/*====================
 *   初始化文件管理器
 *====================*/

void file_manager_init(void)
{
    printf("[FM] File Manager initializing...\n");
    
    // 注册物理返回按键回调（抬手触发）
    touch_register_back_btn_callback(physical_back_btn_handler);
    
    printf("[FM] Creating fm_screen...\n");
    fm_screen = lv_obj_create(NULL);
    lv_obj_set_size(fm_screen, 240, 415);
    /* 禁用滚动和弹性 */
    lv_obj_clear_flag(fm_screen, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN);
    epd_disable_all_animations_recursive(fm_screen);
    printf("[FM] fm_screen created: %p\n", fm_screen);
    
    printf("[FM] Creating header...\n");
    lv_obj_t *header = lv_obj_create(fm_screen);
    lv_obj_set_size(header, 240, 30);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_white(), 0);
    epd_disable_animations(header);
    printf("[FM] Header created\n");
    
    printf("[FM] Creating title label...\n");
    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "文件管理器");
    printf("[FM] Getting font...\n");
    lv_font_t *font = get_reader_font();
    printf("[FM] Title font: %p\n", font);
    printf("[FM] Applying font to title...\n");
    lv_obj_set_style_text_font(title, font, 0);
    lv_obj_center(title);
    printf("[FM] Title created and font applied\n");
    
    path_label = lv_label_create(fm_screen);
    lv_label_set_text(path_label, "当前: /");
    lv_obj_set_style_text_font(path_label, get_reader_font(), 0);
    lv_obj_align(path_label, LV_ALIGN_TOP_MID, 0, 35);
    epd_disable_animations(path_label);
    
    lv_obj_t *close_btn = lv_btn_create(fm_screen);
    lv_obj_set_size(close_btn, 50, 25);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -5, 5);
    epd_disable_animations(close_btn);
    
    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "关闭");
    lv_obj_set_style_text_font(close_label, get_reader_font(), 0);
    lv_obj_add_event_cb(close_btn, close_btn_cb, LV_EVENT_CLICKED, NULL);
    
    fm_list = lv_list_create(fm_screen);
    lv_obj_set_size(fm_list, 215, 365);  /* 减小高度为底部留空间 */
    lv_obj_align(fm_list, LV_ALIGN_TOP_LEFT, 20, 40);  /* 靠右对齐 */
    lv_obj_set_style_pad_row(fm_list, 2, 0);
    /* 关闭所有滚动相关标志 */
    lv_obj_clear_flag(fm_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN);
    epd_disable_all_animations_recursive(fm_list);
    printf("[FM] fm_list created\n");
    
    /* 初始化分页系统 */
    init_pagination();
    
    /* 创建页码指示器（竖排放在左侧） */
    create_page_indicator();
    
    strncpy(current_path, "/", sizeof(current_path) - 1);
    current_path[sizeof(current_path) - 1] = '\0';
    printf("[FM] About to call refresh_file_list...\n");
    refresh_file_list(current_path);
    printf("[FM] refresh_file_list done, loading screen...\n");
    lv_disp_load_scr(fm_screen);
    printf("[FM] Screen loaded, marking refresh...\n");
    epd_mark_refresh_pending();
    
    /* 注册滑动回调到驱动层 - 由lv_port_indev.c在释放时调用，与LVGL事件解耦 */
    touch_register_swipe_callback(swipe_handler);
    printf("[FM] Swipe handler registered to driver layer\n");
    
    /* 注册触摸事件到fm_screen - 只负责设置滑动区域 */
    lv_obj_add_event_cb(fm_screen, touch_event_cb, LV_EVENT_ALL, NULL);
    printf("[FM] File Manager initialized\n");
}

/*====================
 *   字体懒加载功能
 *====================*/

#if LV_USE_TINY_TTF

/**
 * @brief 扫描SD卡Font目录，找到最小的.ttf文件
 * 
 * @return 成功返回0，失败返回-1
 */
static int find_smallest_ttf_font(void)
{
    FRESULT res;
    FILINFO fno;
    DIR dir;
    FSIZE_t smallest_size = (FSIZE_t)-1;  /* 初始化为最大值 */
    
    printf("[FONT] Scanning Font directory for TTF files...\n");
    
    /* 打开Font目录 */
    res = f_opendir(&dir, "0:/Font");
    if (res != FR_OK) {
        printf("[FONT] Failed to open Font directory, error: %d\n", res);
        return -1;
    }
    
    /* 遍历Font目录 */
    for (;;) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) {
            break;  /* 目录读取结束 */
        }
        
        /* 跳过目录和隐藏文件 */
        if (fno.fattrib & AM_DIR) {
            continue;
        }
        
        /* 检查是否为.ttf文件 */
        const char *ext = strrchr(fno.fname, '.');
        if (ext && strcasecmp(ext, ".ttf") == 0) {
            printf("[FONT] Found TTF file: %s (size: %lu bytes)\n", 
                   fno.fname, (unsigned long)fno.fsize);
            
            /* 检查是否为最小的文件 */
            if (fno.fsize < smallest_size) {
                smallest_size = fno.fsize;
                snprintf(ttf_file_path, sizeof(ttf_file_path), 
                        "0:/Font/%s", fno.fname);
                printf("[FONT] New smallest font: %s\n", ttf_file_path);
            }
        }
    }
    
    f_closedir(&dir);
    
    /* 检查是否找到TTF文件 */
    if (smallest_size == (FSIZE_t)-1) {
        printf("[FONT] No TTF files found in Font directory\n");
        return -1;
    }
    
    printf("[FONT] Selected font: %s\n", ttf_file_path);
    return 0;
}

/**
 * @brief 将TTF文件从SD卡读取到PSRAM
 * 
 * @return 成功返回0，失败返回-1
 */
static int read_ttf_file_to_psram(void)
{
    FIL fil;
    FRESULT res;
    UINT bytes_read;
    
    printf("[FONT] Reading TTF file to PSRAM: %s\n", ttf_file_path);
    
    /* 打开TTF文件 */
    res = f_open(&fil, ttf_file_path, FA_READ);
    if (res != FR_OK) {
        printf("[FONT] Failed to open TTF file, error: %d\n", res);
        return -1;
    }
    
    /* 获取文件大小 */
    FSIZE_t file_size = f_size(&fil);
    printf("[FONT] TTF file size: %lu bytes\n", (unsigned long)file_size);
    
    /* 在PSRAM中分配内存 */
    s_ttf_data = (uint8_t *)psram_malloc((size_t)file_size);
    if (s_ttf_data == NULL) {
        printf("[FONT] Failed to allocate PSRAM memory for TTF data\n");
        f_close(&fil);
        return -1;
    }
    
    /* 读取整个文件到内存 */
    printf("[FONT] Reading TTF file into memory...\n");
    res = f_read(&fil, s_ttf_data, (UINT)file_size, &bytes_read);
    if (res != FR_OK || bytes_read != (UINT)file_size) {
        printf("[FONT] Failed to read TTF file, error: %d, read: %u/%lu\n", 
               res, bytes_read, (unsigned long)file_size);
        free(s_ttf_data);
        s_ttf_data = NULL;
        f_close(&fil);
        return -1;
    }
    
    s_ttf_data_size = (size_t)file_size;
    printf("[FONT] TTF file successfully read to PSRAM: %u bytes\n", (unsigned int)s_ttf_data_size);
    
    f_close(&fil);
    return 0;
}

/**
 * @brief 获取字体（支持从SD卡自动加载TTF字体）
 * 
 * 该函数会自动扫描SD卡Font目录下的.ttf文件，
 * 并使用最小的文件作为字体。如果没有找到TTF文件，
 * 则使用编译内置的lv_font_misans_16作为回退。
 * 
 * @return 字体指针
 */
lv_font_t *get_reader_font(void)
{
    /* 如果已经加载过TTF字体，直接返回 */
    if (custom_ttf_font != NULL) {
        return custom_ttf_font;
    }
    
    /* 首次调用：尝试加载TTF字体 */
    if (!ttf_load_attempted) {
        ttf_load_attempted = true;
        
        printf("[FONT] Attempting to load TTF font from SD card...\n");
        
        /* 查找最小的TTF文件 */
        if (find_smallest_ttf_font() == 0) {
            /* 先将TTF文件读取到PSRAM */
            if (read_ttf_file_to_psram() == 0) {
                /* 使用Tiny-TTF从内存数据加载字体 */
                printf("[FONT] Loading TTF from PSRAM (size: %u bytes)\n", (unsigned int)s_ttf_data_size);
                printf("[FONT] ABOUT TO CALL lv_tiny_ttf_create_data()...\n");
                
                custom_ttf_font = lv_tiny_ttf_create_data(s_ttf_data, s_ttf_data_size, 16);
                
                printf("[FONT] lv_tiny_ttf_create_data() returned: %p\n", custom_ttf_font);
                
                if (custom_ttf_font != NULL) {
                    printf("[FONT] TTF font loaded successfully: %p\n", custom_ttf_font);
                    printf("[FONT] Font line height: %d\n", custom_ttf_font->line_height);
                    return custom_ttf_font;
                } else {
                    printf("[FONT] Failed to load TTF font, using fallback\n");
                }
            }
        }
        
        /* 加载失败，回退到内置字体C */
        printf("[FONT] Using built-in misans font as fallback\n");
    }
    
    /* 返回内置字体 */
    printf("[FONT] Returning built-in font: %p\n", &lv_font_misans_16);
    return &lv_font_misans_16;
}

#endif /* LV_USE_TINY_TTF */
