/**
 * @file settings_screen.c
 * @brief 设置界面实现 - WiFi连接 + 字体选择
 *
 * WiFi设置：手机风格
 * - 进入自动异步扫描（不阻塞UI）
 * - 实时检测连接状态（查询netif而非内部变量）
 * - 点击AP → 加密网络弹密码输入，开放网络直接连接
 * - 已连接网络显示"已连接"标记
 *
 * 字体选择：FatFs扫描font目录
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "settings_screen.h"
#include "settings_storage.h"
#include "font_warm.h"
#include "wlan_manager.h"
#include "wifi_controller.h"
#include "epd.h"
#include "lv_port_disp.h"  /* for epd_mark_refresh_pending */
#include "fs/fatfs/ff.h"
#include "kernel/os/os.h"
#include "sys/sys_heap.h"

#define SS_DBG 1
#if SS_DBG
#define SS_LOG(fmt, ...) printf("[SETTINGS] " fmt "\r\n", ##__VA_ARGS__)
#else
#define SS_LOG(fmt, ...)
#endif

/* 字体搜索路径（FatFs格式） */
#define FONT_DIR_PATH   "0:Font"
#define MAX_FONT_FILES  10

/* ========= 全局状态 ======== */
static char g_selected_font[256] = "";
static lv_obj_t *g_return_screen = NULL;

/* WiFi扫描结果 - 动态分配，异步扫描完成后由LVGL线程使用 */
static WLAN_ScanResult_t *g_scan_results = NULL;
static int g_scan_count = 0;

/* WiFi界面的UI元素引用 */
static lv_obj_t *g_wifi_list_cont = NULL;      /* AP列表容器 */
static lv_obj_t *g_wifi_scan_btn = NULL;       /* 重新扫描按钮 */
static lv_obj_t *g_wifi_scan_btn_label = NULL; /* 扫描按钮文字 */
static lv_obj_t *g_wifi_screen = NULL;         /* WiFi界面screen */
static lv_obj_t *g_wifi_switch = NULL;         /* 顶部开关 (手机式) */
static lv_obj_t *g_wifi_switch_label = NULL;   /* 开关左侧的描述文字 */
static int g_wifi_scan_in_progress = 0;
static char g_wifi_connecting_hint[WLAN_MGR_MAX_SSID_LEN] = "";

/* 字体文件信息 */
typedef struct {
    char name[64];
    char path[256];
} FontFile_t;

static FontFile_t g_font_files[MAX_FONT_FILES];
static int g_font_count = 0;

/* 声明extern字体 */
extern const lv_font_t lv_font_montserrat_12;
#define UI_FONT &lv_font_montserrat_12

/* ========= 内部函数声明 ======== */
static void create_wifi_screen_ui(lv_obj_t *parent);
static void wifi_start_scan(void);
static void wifi_show_ap_list(int count, WLAN_ScanResult_t *results);
static void create_wifi_password_input(const char *ssid, int is_encrypted);
static void wifi_refresh_list_contents(void);
static void create_font_screen(lv_obj_t *parent);
static void load_settings_from_ini(void);
static void save_settings_to_ini(void);
static void settings_screen_rebuild_main(void);

static void settings_apply_static_btn_style(lv_obj_t *btn, int border_width, int radius)
{
    if (!btn) return;

    lv_obj_set_style_transition(btn, NULL, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_anim_time(btn, 0, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_bg_color(btn, lv_color_white(), LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_border_width(btn, border_width, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_border_color(btn, lv_color_black(), LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_radius(btn, radius, LV_PART_MAIN | LV_STATE_ANY);

    uint32_t child_cnt = lv_obj_get_child_cnt(btn);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(btn, i);
        if (child) {
            lv_obj_set_style_transition(child, NULL, LV_PART_ANY | LV_STATE_ANY);
            lv_obj_set_style_anim_time(child, 0, LV_PART_ANY | LV_STATE_ANY);
            lv_obj_set_style_text_color(child, lv_color_black(), LV_PART_MAIN | LV_STATE_ANY);
        }
    }
}

/* ===================================================
 * 公共接口
 * =================================================== */

void settings_screen_init(void)
{
    SS_LOG("Initializing settings module...");
    load_settings_from_ini();
    SS_LOG("Selected font: %s", g_selected_font[0] ? g_selected_font : "(none)");
}

const char* settings_get_selected_font(void)
{
    if (g_selected_font[0] == '\0') return NULL;
    return g_selected_font;
}

void settings_wifi_scan_open(lv_obj_t *return_screen)
{
    g_return_screen = return_screen;
    lv_obj_t *scr = lv_obj_create(NULL);
    g_wifi_screen = scr;
    create_wifi_screen_ui(scr);
    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_OVER_LEFT, 0, 0, false);

    if (g_wifi.enabled) {
        /* 进入WiFi界面后自动开始异步扫描 */
        wifi_start_scan();
    } else {
        wifi_refresh_list_contents();
    }
}

void settings_font_select_open(lv_obj_t *return_screen)
{
    g_return_screen = return_screen;
    lv_obj_t *scr = lv_obj_create(NULL);
    create_font_screen(scr);
    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_OVER_LEFT, 0, 0, false);
}

/* ===================================================
 * INI 读写
 * =================================================== */

static void load_settings_from_ini(void)
{
    char buf[256] = "";
    if (settings_get_string("font", "path", buf, sizeof(buf)) == 0) {
        if(strncmp(buf, "0:Font/", 7) == 0) {
            memmove(buf + 2, buf + 1, strlen(buf));
            buf[1] = '/';
        }
        strncpy(g_selected_font, buf, sizeof(g_selected_font) - 1);
        g_selected_font[sizeof(g_selected_font) - 1] = '\0';
        SS_LOG("Loaded font from ini: %s", g_selected_font);
    }
}

static void save_settings_to_ini(void)
{
    settings_set_string("font", "path", g_selected_font);
    SS_LOG("Saved font setting: %s", g_selected_font);
}

void settings_set_reader_font_path(const char *path)
{
    if (!path || path[0] == '\0') {
        return;
    }
    strncpy(g_selected_font, path, sizeof(g_selected_font) - 1);
    g_selected_font[sizeof(g_selected_font) - 1] = '\0';
    save_settings_to_ini();
}

/* ===================================================
 * 重建设置主页
 * =================================================== */

extern void main_ui_create(void);

static void settings_screen_rebuild_main(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    main_ui_create();
    epd_mark_refresh_pending();
}

/* ===================================================
 * WiFi 设置界面 - 手机风格
 * ===================================================
 * 
 * 布局（240x415 屏幕）：
 * [0-20]   标题栏: "WiFi 设置" + 返回
 * [20-45]  状态栏: 已连接/未连接 + IP
 * [45-370] AP列表（可滚动）
 * [370-415] 重新扫描按钮
 */

/* WiFi界面返回 */
static void wifi_back_cb(lv_event_t *e)
{
    (void)e;
    g_wifi_screen = NULL;
    g_wifi_list_cont = NULL;
    g_wifi_scan_btn = NULL;
    g_wifi_scan_btn_label = NULL;
    g_wifi_switch = NULL;
    g_wifi_switch_label = NULL;
    g_wifi_scan_in_progress = 0;
    g_wifi_connecting_hint[0] = '\0';
    /* 释放旧的扫描结果 */
    if (g_scan_results) {
        psram_free(g_scan_results);
        g_scan_results = NULL;
    }
    settings_screen_rebuild_main();
}

/* 重新扫描按钮回调 */
static void wifi_rescan_cb(lv_event_t *e)
{
    (void)e;
    wifi_start_scan();
}

static void wifi_set_connecting_hint(const char *ssid)
{
    if (!ssid) ssid = "";
    strncpy(g_wifi_connecting_hint, ssid, sizeof(g_wifi_connecting_hint) - 1);
    g_wifi_connecting_hint[sizeof(g_wifi_connecting_hint) - 1] = '\0';
}

static void wifi_clear_connecting_hint(void)
{
    g_wifi_connecting_hint[0] = '\0';
}

static void wifi_show_placeholder(const char *text)
{
    if (!g_wifi_list_cont) return;

    lv_obj_clean(g_wifi_list_cont);

    lv_obj_t *label = lv_label_create(g_wifi_list_cont);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, UI_FONT, 0);
    lv_obj_set_style_text_color(label, lv_color_make(100, 100, 100), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}

static void wifi_update_scan_button(void)
{
    if (!g_wifi_scan_btn || !g_wifi_scan_btn_label) return;

    if (!g_wifi.enabled) {
        lv_label_set_text(g_wifi_scan_btn_label, "WiFi Off");
        lv_obj_add_state(g_wifi_scan_btn, LV_STATE_DISABLED);
    } else if (g_wifi_scan_in_progress) {
        lv_label_set_text(g_wifi_scan_btn_label, "Scanning...");
        lv_obj_add_state(g_wifi_scan_btn, LV_STATE_DISABLED);
    } else {
        lv_label_set_text(g_wifi_scan_btn_label, LV_SYMBOL_WIFI " Rescan");
        lv_obj_clear_state(g_wifi_scan_btn, LV_STATE_DISABLED);
    }

    lv_obj_center(g_wifi_scan_btn_label);
}

static void wifi_update_switch_label(void)
{
    if (!g_wifi_switch_label) return;

    char text[80];
    if (!g_wifi.enabled) {
        snprintf(text, sizeof(text), "Wi-Fi Off");
    } else if (g_wifi.phase == WLAN_PHASE_CONNECTED) {
        if (g_wifi.ip[0]) {
            snprintf(text, sizeof(text), "%s  %s",
                     g_wifi.ssid[0] ? g_wifi.ssid : "(unknown)",
                     g_wifi.ip);
        } else {
            snprintf(text, sizeof(text), "%s",
                     g_wifi.ssid[0] ? g_wifi.ssid : "Connected");
        }
    } else if (g_wifi_connecting_hint[0] != '\0') {
        snprintf(text, sizeof(text), "Connecting to %s", g_wifi_connecting_hint);
    } else if (g_wifi.phase == WLAN_PHASE_CONNECTING) {
        snprintf(text, sizeof(text), "Connecting...");
    } else if (g_wifi.ssid[0] != '\0') {
        snprintf(text, sizeof(text), "Not Connected");
    } else if (g_wifi_scan_in_progress) {
        snprintf(text, sizeof(text), "Searching...");
    } else {
        snprintf(text, sizeof(text), "Choose a Network");
    }
    lv_label_set_text(g_wifi_switch_label, text);
}

static void wifi_switch_label_click_cb(lv_event_t *e)
{
    (void)e;
    if (!g_wifi.enabled) {
        if (g_wifi_switch) {
            lv_obj_add_state(g_wifi_switch, LV_STATE_CHECKED);
        }
        g_wifi.enabled = 1;
        wifi_clear_connecting_hint();
        wifi_update_switch_label();
        wifi_update_scan_button();
        wifi_controller_request_enable();
        wifi_controller_request_retry();
        wifi_start_scan();
    } else if (!g_wifi_scan_in_progress) {
        wifi_start_scan();
    }
}

static void wifi_switch_event_handler(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool is_on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    if (is_on) {
        if (!g_wifi.enabled) {
            SS_LOG("WiFi switched OFF -> ON");
            g_wifi.enabled = 1;
            wifi_clear_connecting_hint();
            wifi_update_switch_label();
            wifi_update_scan_button();
            wifi_controller_request_enable();
            wifi_controller_request_retry();
            wifi_start_scan();
        } else {
            SS_LOG("WiFi already ON, noop");
        }
    } else {
        SS_LOG("WiFi switched ON -> OFF");
        g_wifi.enabled = 0;
        g_wifi_scan_in_progress = 0;
        wifi_clear_connecting_hint();
        wifi_update_switch_label();
        wifi_update_scan_button();
        wifi_refresh_list_contents();
        wifi_controller_request_disable();
        epd_mark_refresh_pending();
    }
}

/* 构建WiFi界面UI框架 */
static void create_wifi_screen_ui(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_white(), 0);

    /* === 顶部标题栏 === */
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "WiFi");
    lv_obj_set_style_text_font(title, UI_FONT, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    /* 返回按钮（右上角）- 70x35 大按钮, 与顶边对齐 */
    lv_obj_t *btn_back = lv_btn_create(parent);
    lv_obj_set_size(btn_back, 70, 35);
    lv_obj_align(btn_back, LV_ALIGN_TOP_RIGHT, -3, 0);
    settings_apply_static_btn_style(btn_back, 2, 0);
    lv_obj_add_event_cb(btn_back, wifi_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_set_style_text_font(lbl_back, UI_FONT, 0);
    lv_obj_set_style_text_color(lbl_back, lv_color_black(), 0);
    lv_obj_center(lbl_back);

    /* === 开关行 (手机式) - Y=40, 避开右上角 Back 按钮 === */
    /* 左侧描述文字 (可点击, 触发 tap ON 行为) */
    g_wifi_switch_label = lv_label_create(parent);
    lv_obj_set_style_text_font(g_wifi_switch_label, UI_FONT, 0);
    lv_obj_set_style_text_color(g_wifi_switch_label, lv_color_black(), 0);
    lv_obj_set_width(g_wifi_switch_label, 170);
    lv_obj_align(g_wifi_switch_label, LV_ALIGN_TOP_LEFT, 5, 44);
    /* 加一个透明 button 在 label 后面, 方便点击 */
    lv_obj_t *label_btn = lv_btn_create(parent);
    lv_obj_set_size(label_btn, 170, 26);
    lv_obj_align(label_btn, LV_ALIGN_TOP_LEFT, 5, 40);
    lv_obj_set_style_bg_opa(label_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(label_btn, 0, 0);
    lv_obj_set_style_radius(label_btn, 0, 0);
    lv_obj_set_style_transition(label_btn, NULL, LV_PART_MAIN);
    lv_obj_add_event_cb(label_btn, wifi_switch_label_click_cb, LV_EVENT_CLICKED, NULL);

    /* 右侧开关 - 墨水屏优化: 白底椭圆 + 实心黑圆 knob */
    g_wifi_switch = lv_switch_create(parent);
    lv_obj_set_size(g_wifi_switch, 50, 26);
    lv_obj_align(g_wifi_switch, LV_ALIGN_TOP_RIGHT, -5, 40);
    lv_obj_add_event_cb(g_wifi_switch, wifi_switch_event_handler, LV_EVENT_VALUE_CHANGED, NULL);
    /* INDICATOR 透明 — 默认主题在 INDICATOR 上画黑底覆盖白底 MAIN */
    lv_obj_set_style_bg_opa(g_wifi_switch, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_wifi_switch, LV_OPA_TRANSP, LV_STATE_CHECKED | LV_PART_INDICATOR);
    /* Body: 始终白底黑边圆角 (无论 CHECKED 与否) */
    lv_obj_set_style_bg_color(g_wifi_switch, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_wifi_switch, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_wifi_switch, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(g_wifi_switch, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_radius(g_wifi_switch, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_wifi_switch, lv_color_white(), LV_STATE_CHECKED | LV_PART_MAIN);
    lv_obj_set_style_border_color(g_wifi_switch, lv_color_black(), LV_STATE_CHECKED | LV_PART_MAIN);
    /* Knob: 始终实心黑圆 */
    lv_obj_set_style_bg_color(g_wifi_switch, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(g_wifi_switch, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(g_wifi_switch, 0, LV_PART_KNOB);
    lv_obj_set_style_radius(g_wifi_switch, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_wifi_switch, 2, LV_PART_KNOB);
    /* 禁用动画 */
    lv_obj_set_style_transition(g_wifi_switch, NULL, LV_PART_MAIN);
    lv_obj_set_style_transition(g_wifi_switch, NULL, LV_PART_KNOB);

    if (g_wifi.enabled) {
        lv_obj_add_state(g_wifi_switch, LV_STATE_CHECKED);
    }
    wifi_update_switch_label();

    /* === 分隔线 === */
    lv_obj_t *sep1 = lv_obj_create(parent);
    lv_obj_set_size(sep1, 230, 1);
    lv_obj_align(sep1, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_style_bg_color(sep1, lv_color_black(), 0);
    lv_obj_set_style_border_width(sep1, 0, 0);

    /* === AP列表容器（可滚动） === */
    g_wifi_list_cont = lv_obj_create(parent);
    lv_obj_set_size(g_wifi_list_cont, LV_HOR_RES - 4, 268);
    lv_obj_align(g_wifi_list_cont, LV_ALIGN_TOP_MID, 0, 92);
    lv_obj_set_scroll_dir(g_wifi_list_cont, LV_DIR_VER);
    lv_obj_set_style_border_width(g_wifi_list_cont, 0, 0);
    lv_obj_set_style_pad_all(g_wifi_list_cont, 2, 0);

    /* 默认占位，进入页面后会按当前状态刷新 */
    lv_obj_t *scanning_lbl = lv_label_create(g_wifi_list_cont);
    lv_label_set_text(scanning_lbl, "Scanning WiFi...");
    lv_obj_set_style_text_font(scanning_lbl, UI_FONT, 0);
    lv_obj_set_style_text_color(scanning_lbl, lv_color_make(100, 100, 100), 0);
    lv_obj_align(scanning_lbl, LV_ALIGN_CENTER, 0, 0);

    /* === 底部重新扫描按钮 === */
    g_wifi_scan_btn = lv_btn_create(parent);
    lv_obj_set_size(g_wifi_scan_btn, LV_HOR_RES - 20, 35);
    lv_obj_align(g_wifi_scan_btn, LV_ALIGN_BOTTOM_MID, 0, -5);
    settings_apply_static_btn_style(g_wifi_scan_btn, 2, 4);
    lv_obj_add_event_cb(g_wifi_scan_btn, wifi_rescan_cb, LV_EVENT_CLICKED, NULL);
    
    g_wifi_scan_btn_label = lv_label_create(g_wifi_scan_btn);
    lv_label_set_text(g_wifi_scan_btn_label, LV_SYMBOL_WIFI " Rescan");
    lv_obj_set_style_text_font(g_wifi_scan_btn_label, UI_FONT, 0);
    lv_obj_set_style_text_color(g_wifi_scan_btn_label, lv_color_black(), 0);
    lv_obj_center(g_wifi_scan_btn_label);

    wifi_update_scan_button();
}

/* ======== 异步扫描回调（在LVGL线程中执行） ======== */

/* 签名必须匹配 wlan_manager.h:54 的 WLAN_ScanDoneCb_t
 * wlan_manager_poll 按 3 参调用: cb(count, results, user_data).
 * 旧版签名是 (void *arg), 导致 9 个 AP 的 count 被当成指针解引用
 * 读到 0 - 表现为 "扫描成功但 UI 显示 No WiFi found". */
static void wifi_scan_done_cb(int count, WLAN_ScanResult_t *results, void *user_data)
{
    (void)user_data;
    SS_LOG("[SCAN_DONE] count=%d", count);
    g_wifi_scan_in_progress = 0;

    /* 如果用户已经离开了WiFi界面，释放 results 避免泄漏 */
    if (!g_wifi_list_cont) {
        if (results) psram_free(results);
        return;
    }

    /* 释放旧结果 */
    if (g_scan_results) {
        psram_free(g_scan_results);
    }
    g_scan_results = results;
    g_scan_count = count;

    /* 更新开关行描述 */
    wifi_update_switch_label();
    wifi_update_scan_button();

    /* 显示扫描结果 */
    wifi_refresh_list_contents();

    epd_mark_refresh_pending();
}

/* 开始异步扫描 */
static void wifi_start_scan(void)
{
    if (!g_wifi_list_cont) return;

    if (!g_wifi.enabled) {
        g_wifi_scan_in_progress = 0;
        wifi_update_switch_label();
        wifi_update_scan_button();
        wifi_refresh_list_contents();
        epd_mark_refresh_pending();
        return;
    }

    if (g_wifi_scan_in_progress) return;

    SS_LOG("Starting async WiFi scan...");

    g_wifi_scan_in_progress = 1;
    wifi_update_switch_label();
    wifi_update_scan_button();
    /* 始终刷新列表：有缓存结果则立即显示，无缓存则显示"Scanning..." */
    wifi_refresh_list_contents();

    epd_mark_refresh_pending();

    /* 发起异步扫描 */
    wlan_manager_scan_async(wifi_scan_done_cb, NULL);
}

/* ======== AP点击处理 ======== */

static void wifi_ap_clicked_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_scan_count || !g_scan_results) return;

    WLAN_ScanResult_t *ap = &g_scan_results[idx];

    char current_ssid[WLAN_MGR_MAX_SSID_LEN] = "";
    int is_current = (wlan_manager_get_current_ssid(current_ssid, sizeof(current_ssid)) == 0 &&
                      strcmp(current_ssid, ap->ssid) == 0);

    if (is_current) {
        SS_LOG("Current AP tapped: %s", ap->ssid);
    } else if (ap->is_encrypted) {
        /* 加密网络 → 弹密码输入 */
        create_wifi_password_input(ap->ssid, 1);
    } else {
        /* 开放网络 → 写到 INI, 委托给 controller 连 */
        SS_LOG("Connecting to open network: %s", ap->ssid);
        settings_set_string("wifi", "ssid", ap->ssid);
        settings_set_string("wifi", "password", "");
        settings_set_string("wifi_ap", ap->ssid, "");  /* 记住开放网络 */
        g_wifi.enabled = 1;
        wifi_set_connecting_hint(ap->ssid);
        wifi_update_switch_label();
        if (g_wifi_switch) {
            lv_obj_add_state(g_wifi_switch, LV_STATE_CHECKED);
        }
        wifi_update_scan_button();
        wifi_controller_request_enable();
        wifi_controller_request_retry();
        epd_mark_refresh_pending();
    }
}

static void wifi_refresh_list_contents(void)
{
    if (!g_wifi_list_cont) return;

    if (!g_wifi.enabled) {
        wifi_show_placeholder("Wi-Fi is Off");
        return;
    }

    if (g_scan_results && g_scan_count > 0) {
        wifi_show_ap_list(g_scan_count, g_scan_results);
        return;
    }

    if (g_wifi_scan_in_progress) {
        wifi_show_placeholder("Scanning...");
    } else {
        wifi_show_placeholder("No WiFi found");
    }
}

/* 显示扫描结果AP列表（带点击回调） */
static void wifi_show_ap_list(int count, WLAN_ScanResult_t *results)
{
    if (!g_wifi_list_cont) return;
    
    lv_obj_clean(g_wifi_list_cont);
    
    if (count <= 0 || results == NULL) {
        lv_obj_t *no_result = lv_label_create(g_wifi_list_cont);
        lv_label_set_text(no_result, "No WiFi found");
        lv_obj_set_style_text_font(no_result, UI_FONT, 0);
        lv_obj_set_style_text_color(no_result, lv_color_make(100, 100, 100), 0);
        lv_obj_align(no_result, LV_ALIGN_CENTER, 0, 0);
        return;
    }
    
    char current_ssid[WLAN_MGR_MAX_SSID_LEN] = "";
    wlan_manager_get_current_ssid(current_ssid, sizeof(current_ssid));
    
    lv_obj_t *prev_btn = NULL;
    int i;
    for (i = 0; i < count; i++) {
        char btn_text[80];
        int is_current = (strcmp(results[i].ssid, current_ssid) == 0);
        int is_connecting = (g_wifi_connecting_hint[0] != '\0' &&
                             strcmp(results[i].ssid, g_wifi_connecting_hint) == 0);
        
        if (is_current) {
            snprintf(btn_text, sizeof(btn_text), "> %s  %ddBm",
                     results[i].ssid, results[i].rssi);
        } else if (is_connecting) {
            snprintf(btn_text, sizeof(btn_text), "... %s  %ddBm",
                     results[i].ssid, results[i].rssi);
        } else {
            snprintf(btn_text, sizeof(btn_text), "%s  %ddBm%s",
                     results[i].ssid,
                     results[i].rssi,
                     results[i].is_encrypted ? " *" : "");
        }
        
        lv_obj_t *btn = lv_btn_create(g_wifi_list_cont);
        lv_obj_set_size(btn, 228, 30);
        lv_obj_set_style_radius(btn, 2, 0);
        
        if (is_current || is_connecting) {
            lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
            lv_obj_set_style_border_width(btn, 0, 0);
        } else {
            lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
            lv_obj_set_style_border_width(btn, 1, 0);
            lv_obj_set_style_border_color(btn, lv_color_make(180, 180, 180), 0);
        }
        
        /* 添加点击事件 */
        lv_obj_add_event_cb(btn, wifi_ap_clicked_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)i);
        
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, btn_text);
        lv_obj_set_style_text_font(label, UI_FONT, 0);
        if (is_current || is_connecting) {
            lv_obj_set_style_text_color(label, lv_color_white(), 0);
        } else {
            lv_obj_set_style_text_color(label, lv_color_black(), 0);
        }
        lv_obj_center(label);
        
        /* 垂直排列 */
        if (prev_btn) {
            lv_obj_align_to(btn, prev_btn, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
        } else {
            lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 2);
        }
        prev_btn = btn;
    }
}

/* ===================================================
 * WiFi 密码输入界面
 * =================================================== */

static char g_connecting_ssid[WLAN_MGR_MAX_SSID_LEN];
static lv_obj_t *g_pwd_textarea = NULL;

/* 异步连接完成回调 - 不再使用, controller 负责所有连接状态. */
static void wifi_connect_done_cb(int success, void *user_data)
{
    (void)success;
    (void)user_data;
}

/* 连接按钮回调 - 立即 back 到 WiFi 列表页, 不显示 "Connecting to..." 死页.
 * 状态变化由 wifi_controller phase callback 推到 status bar / 开关行描述. */
static void wifi_connect_btn_cb(lv_event_t *e)
{
    (void)e;
    if (!g_pwd_textarea) return;
    const char *pwd = lv_textarea_get_text(g_pwd_textarea);

    SS_LOG("Connecting to %s pwd_len=%d", g_connecting_ssid, (int)strlen(pwd));

    /* 保存到 INI */
    settings_set_string("wifi", "ssid", g_connecting_ssid);
    settings_set_string("wifi", "password", pwd);
    settings_set_string("wifi_ap", g_connecting_ssid, pwd);  /* 多网络密码 */

    /* 立即重建 WiFi 列表页 (不卡在 "Connecting to SSID..." 死页) */
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    g_wifi_screen = scr;
    g_pwd_textarea = NULL;
    create_wifi_screen_ui(scr);

    /* 恢复之前的 AP 列表 (而不是显示 "Scanning...") */
    if (g_scan_results && g_scan_count > 0) {
        wifi_show_ap_list(g_scan_count, g_scan_results);
    } else {
        wifi_refresh_list_contents();
    }

    /* 委托 controller 异步连, phase 变化时自动更新开关行 */
    g_wifi.enabled = 1;
    wifi_set_connecting_hint(g_connecting_ssid);
    wifi_update_switch_label();
    if (g_wifi_switch) {
        lv_obj_add_state(g_wifi_switch, LV_STATE_CHECKED);
    }
    wifi_update_scan_button();
    wifi_controller_request_enable();
    wifi_controller_request_retry();
    epd_mark_refresh_pending();
}

/* 密码界面返回 */
static void wifi_pwd_back_cb(lv_event_t *e)
{
    (void)e;
    /* 返回WiFi设置 - 重建wifi screen */
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    g_wifi_screen = scr;
    g_wifi_list_cont = NULL;
    create_wifi_screen_ui(scr);
    if (g_wifi.enabled) {
        wifi_start_scan();
    } else {
        wifi_refresh_list_contents();
        epd_mark_refresh_pending();
    }
}

/* 键盘ready回调 */
static void wifi_kb_ready_cb(lv_event_t *e)
{
    wifi_connect_btn_cb(e);
}

/* ========== WiFi 密码键盘自定义布局 (240px 屏, 1u = 24px) ==========
 * lv_btnmatrix 宽度字段是整数 1-7. "CAPS"/"ABC"/"?123"/"DEL" 是 ASCII 字符
 * 不依赖字体图标, 墨水屏渲染可靠. */
static const char *kb_map_lc[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    " ", "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    " ", " ", "z", "x", "c", "v", "b", "n", "m", "\n",
    "CAPS", "?123", "DEL", ""
};
static const lv_btnmatrix_ctrl_t kb_ctrl_lc[] = {
    1,1,1,1,1,1,1,1,1,1,
    LV_BTNMATRIX_CTRL_HIDDEN | 1, 1,1,1,1,1,1,1,1,1,
    LV_BTNMATRIX_CTRL_HIDDEN | 1, LV_BTNMATRIX_CTRL_HIDDEN | 1, 1,1,1,1,1,1,1,
    3, 3, 3
};

static const char *kb_map_uc[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    " ", " ", "Z", "X", "C", "V", "B", "N", "M", "\n",
    "CAPS", "?123", "DEL", ""
};
static const lv_btnmatrix_ctrl_t kb_ctrl_uc[] = {
    1,1,1,1,1,1,1,1,1,1,
    LV_BTNMATRIX_CTRL_HIDDEN | 1, 1,1,1,1,1,1,1,1,1,
    LV_BTNMATRIX_CTRL_HIDDEN | 1, LV_BTNMATRIX_CTRL_HIDDEN | 1, 1,1,1,1,1,1,1,
    3, 3, 3
};

static const char *kb_map_num[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    ".", ",", "?", "!", "'", "\"", ":", ";", "-", "_", "\n",
    "+", "=", "*", "/", "\\", "#", "@", "&", "%", "$", "\n",
    "ABC", " ", "DEL", ""
};
static const lv_btnmatrix_ctrl_t kb_ctrl_num[] = {
    1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,
    3, LV_BTNMATRIX_CTRL_HIDDEN | 3, 3
};

/* 自定义键盘事件处理: 完全替代默认 lv_keyboard_def_event_cb
 * 默认 handler 只认识 "abc"/"ABC"/"1#"/LV_SYMBOL_BACKSPACE,
 * 不认识我们的 "CAPS"/"?123"/"DEL", 会把它们当文本打进 textarea */
static void wifi_kb_event_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    lv_keyboard_t *keyboard = (lv_keyboard_t *)kb;
    uint16_t btn_id = lv_btnmatrix_get_selected_btn(kb);
    if (btn_id == LV_BTNMATRIX_BTN_NONE) return;
    const char *txt = lv_btnmatrix_get_btn_text(kb, btn_id);
    if (!txt) return;

    if (strcmp(txt, "CAPS") == 0) {
        lv_keyboard_mode_t mode = lv_keyboard_get_mode(kb);
        if (mode == LV_KEYBOARD_MODE_TEXT_LOWER) {
            lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_UPPER);
        } else {
            lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        }
        return;
    }
    if (strcmp(txt, "?123") == 0) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
        return;
    }
    if (strcmp(txt, "ABC") == 0) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        return;
    }
    if (strcmp(txt, "DEL") == 0) {
        if (keyboard->ta) {
            lv_textarea_del_char(keyboard->ta);
        }
        return;
    }
    if (strcmp(txt, LV_SYMBOL_OK) == 0) {
        lv_event_send(kb, LV_EVENT_READY, NULL);
        if (keyboard->ta) {
            lv_event_send(keyboard->ta, LV_EVENT_READY, NULL);
        }
        return;
    }
    /* 其他文本 → 字母/数字/标点输入 textarea */
    if (keyboard->ta) {
        lv_textarea_add_text(keyboard->ta, txt);
    }
}

static void create_wifi_password_input(const char *ssid, int is_encrypted)
{
    (void)is_encrypted;
    
    strncpy(g_connecting_ssid, ssid, sizeof(g_connecting_ssid) - 1);
    g_connecting_ssid[sizeof(g_connecting_ssid) - 1] = '\0';
    
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    
    /* 标题 */
    char title_text[64];
    snprintf(title_text, sizeof(title_text), "WiFi: %s", ssid);
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, title_text);
    lv_obj_set_style_text_font(title, UI_FONT, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 3);
    
    /* 密码输入框 */
    g_pwd_textarea = lv_textarea_create(scr);
    lv_obj_set_size(g_pwd_textarea, LV_HOR_RES - 20, 30);
    lv_obj_align(g_pwd_textarea, LV_ALIGN_TOP_MID, 0, 20);
    lv_textarea_set_placeholder_text(g_pwd_textarea, "Password");
    lv_textarea_set_text(g_pwd_textarea, "");
    lv_textarea_set_password_mode(g_pwd_textarea, 0);
    lv_textarea_set_one_line(g_pwd_textarea, 1);
    
    /* 从 wifi_ap section 加载保存的密码 */
    char saved_pwd[65] = "";
    if (settings_get_string("wifi_ap", ssid, saved_pwd, sizeof(saved_pwd)) == 0
        && saved_pwd[0] != '\0') {
        lv_textarea_set_text(g_pwd_textarea, saved_pwd);
        SS_LOG("Loaded saved password for %s", ssid);
    }
    
    /* 连接按钮 */
    lv_obj_t *btn_connect = lv_btn_create(scr);
    lv_obj_set_size(btn_connect, LV_HOR_RES - 20, 30);
    lv_obj_align(btn_connect, LV_ALIGN_TOP_MID, 0, 55);
    lv_obj_set_style_bg_color(btn_connect, lv_color_black(), 0);
    lv_obj_set_style_radius(btn_connect, 4, 0);
    lv_obj_add_event_cb(btn_connect, wifi_connect_btn_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *label_conn = lv_label_create(btn_connect);
    lv_label_set_text(label_conn, "Connect");
    lv_obj_set_style_text_font(label_conn, UI_FONT, 0);
    lv_obj_set_style_text_color(label_conn, lv_color_white(), 0);
    lv_obj_center(label_conn);
    
    /* 返回按钮 */
    lv_obj_t *btn_back = lv_btn_create(scr);
    lv_obj_set_size(btn_back, LV_HOR_RES - 20, 25);
    lv_obj_align(btn_back, LV_ALIGN_TOP_MID, 0, 90);
    settings_apply_static_btn_style(btn_back, 1, 3);
    lv_obj_add_event_cb(btn_back, wifi_pwd_back_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *label_back = lv_label_create(btn_back);
    lv_label_set_text(label_back, "Cancel");
    lv_obj_set_style_text_font(label_back, UI_FONT, 0);
    lv_obj_set_style_text_color(label_back, lv_color_black(), 0);
    lv_obj_center(label_back);
    
    /* 虚拟键盘 - 自定义 QWERTY 布局, edge-to-edge 24px 键宽 */
    lv_obj_t *kb = lv_keyboard_create(scr);
    lv_obj_set_size(kb, LV_HOR_RES, LV_VER_RES * 2 / 3);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, kb_map_lc, kb_ctrl_lc);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, kb_map_uc, kb_ctrl_uc);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_NUMBER,     kb_map_num, kb_ctrl_num);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, g_pwd_textarea);
    /* 移除默认 def_event_cb, 它不认识 "DEL"/"?123"/"CAPS" 会当文本打进 textarea */
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    /* 注册自定义 handler 处理所有按键 */
    lv_obj_add_event_cb(kb, wifi_kb_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(kb, wifi_kb_ready_cb, LV_EVENT_READY, NULL);

    /* 按键紧贴 0 间距 - 遍历所有按键 child 改 style */
    uint32_t cnt = lv_obj_get_child_cnt(kb);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *btn = lv_obj_get_child(kb, i);
        if (!btn) continue;
        lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_row(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_column(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_text_font(btn, UI_FONT, LV_PART_MAIN);
    }
    lv_obj_set_style_pad_row(kb, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(kb, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(kb, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_right(kb, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(kb, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(kb, 0, LV_PART_MAIN);

    epd_mark_refresh_pending();
}

/* ===================================================
 * Phase 联动 - 由 main.c on_wifi_phase_change 调用
 * =================================================== */

void settings_wifi_on_phase_change(WLAN_Phase_t phase)
{
    if (!g_wifi_switch_label) return;  /* 不在 WiFi 界面 */

    if (phase != WLAN_PHASE_CONNECTING) {
        wifi_clear_connecting_hint();
    }

    wifi_update_switch_label();
    wifi_update_scan_button();
    if (g_wifi_switch) {
        if (g_wifi.enabled) {
            lv_obj_add_state(g_wifi_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(g_wifi_switch, LV_STATE_CHECKED);
        }
    }
    wifi_refresh_list_contents();
    epd_mark_refresh_pending();
}

/* ===================================================
 * 字体选择界面（保持不变）
 * =================================================== */

static int scan_font_files(void)
{
    FRESULT res;
    FILINFO fno;
    DIR dir;
    
    g_font_count = 0;
    
    res = f_opendir(&dir, FONT_DIR_PATH);
    if (res != FR_OK) {
        SS_LOG("Failed to open font dir: %s, res=%d", FONT_DIR_PATH, res);
        return -1;
    }
    
    while (g_font_count < MAX_FONT_FILES) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) break;
        if (fno.fattrib & AM_DIR) continue;
        
        int len = strlen(fno.fname);
        if (len > 4) {
            const char *ext = fno.fname + len - 4;
            if ((ext[0] == '.' || ext[0] == '.') &&
                (ext[1] == 't' || ext[1] == 'T') &&
                (ext[2] == 't' || ext[2] == 'T') &&
                (ext[3] == 'f' || ext[3] == 'F')) {
                
                snprintf(g_font_files[g_font_count].path,
                         sizeof(g_font_files[g_font_count].path),
                         "0:/Font/%s", fno.fname);
                strncpy(g_font_files[g_font_count].name,
                        fno.fname,
                        sizeof(g_font_files[g_font_count].name) - 1);
                g_font_files[g_font_count].name[sizeof(g_font_files[g_font_count].name) - 1] = '\0';
                SS_LOG("Found font: %s -> %s",
                       g_font_files[g_font_count].name,
                       g_font_files[g_font_count].path);
                g_font_count++;
            }
        }
    }
    
    f_closedir(&dir);
    SS_LOG("Found %d font files", g_font_count);
    return g_font_count;
}

static void font_selected_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_font_count) return;
    
    SS_LOG("Font selected: %s", g_font_files[idx].path);
    
    strncpy(g_selected_font, g_font_files[idx].path, sizeof(g_selected_font) - 1);
    g_selected_font[sizeof(g_selected_font) - 1] = '\0';
    
    save_settings_to_ini();
    font_warm_request(g_selected_font);
    
    settings_screen_rebuild_main();
}

static void font_back_cb(lv_event_t *e)
{
    (void)e;
    settings_screen_rebuild_main();
}

static void create_font_screen(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_white(), 0);
    
    /* 标题 */
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Font Select");
    lv_obj_set_style_text_font(title, UI_FONT, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);
    
    /* 当前字体 */
    char cur_text[320];
    const char *cur_font = g_selected_font[0] ? g_selected_font : "Default(smallest)";
    const char *cache_txt = "n/a";
    if(g_selected_font[0]) {
        cache_txt = font_warm_l1glyf_exists(g_selected_font) ? "l1glyf OK" :
                    (font_warm_is_ready() ? "warm only" : "missing");
    }
    snprintf(cur_text, sizeof(cur_text), "Now: %s\nCache: %s", cur_font, cache_txt);
    
    lv_obj_t *cur_label = lv_label_create(parent);
    lv_label_set_text(cur_label, cur_text);
    lv_obj_set_style_text_font(cur_label, UI_FONT, 0);
    lv_obj_set_style_text_color(cur_label, lv_color_black(), 0);
    lv_obj_align(cur_label, LV_ALIGN_TOP_LEFT, 5, 25);
    
    /* 扫描字体文件 */
    scan_font_files();
    
    /* 字体列表容器 */
    lv_obj_t *list_cont = lv_obj_create(parent);
    lv_obj_set_size(list_cont, LV_HOR_RES - 10, LV_VER_RES - 105);
    lv_obj_align(list_cont, LV_ALIGN_TOP_MID, 0, 45);
    lv_obj_set_scroll_dir(list_cont, LV_DIR_VER);
    lv_obj_set_style_border_width(list_cont, 1, 0);
    
    lv_obj_t *prev_btn = NULL;
    int i;
    for (i = 0; i < g_font_count; i++) {
        lv_obj_t *btn = lv_btn_create(list_cont);
        lv_obj_set_size(btn, LV_HOR_RES - 30, 40);
        
        if (prev_btn)
            lv_obj_align_to(btn, prev_btn, LV_ALIGN_OUT_BOTTOM_MID, 0, 3);
        else
            lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 3);
        
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 3, 0);
        lv_obj_add_event_cb(btn, font_selected_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)i);
        
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, g_font_files[i].name);
        lv_obj_set_style_text_font(label, UI_FONT, 0);
        lv_obj_set_style_text_color(label, lv_color_black(), 0);
        lv_obj_center(label);
        
        prev_btn = btn;
    }
    
    if (g_font_count == 0) {
        lv_obj_t *no_font = lv_label_create(list_cont);
        lv_label_set_text(no_font, "No fonts found in /Font");
        lv_obj_set_style_text_font(no_font, UI_FONT, 0);
        lv_obj_align(no_font, LV_ALIGN_CENTER, 0, 0);
    }
    
    /* 返回按钮 */
    lv_obj_t *btn_back = lv_btn_create(parent);
    lv_obj_set_size(btn_back, LV_HOR_RES - 20, 35);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_MID, 0, -5);
    settings_apply_static_btn_style(btn_back, 2, 4);
    lv_obj_add_event_cb(btn_back, font_back_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *label_back = lv_label_create(btn_back);
    lv_label_set_text(label_back, "Back");
    lv_obj_set_style_text_font(label_back, UI_FONT, 0);
    lv_obj_set_style_text_color(label_back, lv_color_black(), 0);
    lv_obj_center(label_back);
}
