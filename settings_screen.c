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
#include "chsc6540.h"
#include "lv_port_indev.h"
#include "font_warm.h"
#include "wlan_manager.h"
#include "wifi_controller.h"
#include "epd.h"
#include "lv_port_disp.h"  /* for epd_mark_refresh_pending */
#include "fs/fatfs/ff.h"
#include "kernel/os/os.h"
#include "sys/sys_heap.h"
#include "version.h"
#include "avatar_eink_img.h"
#include "logo_eink_img.h"

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
static void touch_test_open_cb(lv_event_t *e);   /* WiFi 页 Touch Test 入口 */
static void wifi_start_scan(void);
static void wifi_show_ap_list(int count, WLAN_ScanResult_t *results);
static void create_wifi_password_input(const char *ssid, int is_encrypted);
static void wifi_refresh_list_contents(void);
static void create_font_screen(lv_obj_t *parent);
static void load_settings_from_ini(void);
static void save_settings_to_ini(void);
static void settings_screen_rebuild_main(void);
/* 物理返回按键处理函数 (由各子界面打开时注册, 与屏幕返回按钮等效) */
static void wifi_back_do(void);
static void wifi_pwd_back_do(void);
static void font_back_do(void);
static void touch_test_back_do(void);
static void about_fw_back_do(void);
static void about_author_back_do(void);

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

    /* 加载已保存的触摸校准并应用到驱动 (无保存则保持恒等映射) */
    char buf[16];
    if (settings_get_string("touch", "valid", buf, sizeof(buf)) == 0 && atoi(buf) == 1) {
        int32_t sx_num = 1000, sx_den = 1000, sx_off = 0;
        int32_t sy_num = 1000, sy_den = 1000, sy_off = 0;
        if (settings_get_string("touch", "sx_num", buf, sizeof(buf)) == 0) sx_num = atoi(buf);
        if (settings_get_string("touch", "sx_off", buf, sizeof(buf)) == 0) sx_off = atoi(buf);
        if (settings_get_string("touch", "sy_num", buf, sizeof(buf)) == 0) sy_num = atoi(buf);
        if (settings_get_string("touch", "sy_off", buf, sizeof(buf)) == 0) sy_off = atoi(buf);

        /* 自愈校验: 旧版 bug 曾把偏移写成数千像素, 参数越界=坏数据, 拒绝并清掉 */
        int bad = (sx_num < 500 || sx_num > 2000 ||
                   sy_num < 500 || sy_num > 2000 ||
                   sx_off < -300 || sx_off > 300 ||
                   sy_off < -300 || sy_off > 300);
        if (bad) {
            CHSC6540_ResetCalibration();
            settings_set_string("touch", "valid", "0");
            SS_LOG("Touch calibration INVALID (stale buggy data), reset to identity");
        } else {
            CHSC6540_SetCalibration(sx_num, sx_den, sx_off, sy_num, sy_den, sy_off);
            SS_LOG("Loaded touch calibration: x=%d/1000%+d y=%d/1000%+d",
                   (int)sx_num, (int)sx_off, (int)sy_num, (int)sy_off);
        }
    }

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

    /* 绑定物理返回键: 与 WiFi 界面返回按钮等效 */
    touch_register_back_btn_callback(wifi_back_do);
}

void settings_font_select_open(lv_obj_t *return_screen)
{
    g_return_screen = return_screen;
    lv_obj_t *scr = lv_obj_create(NULL);
    create_font_screen(scr);
    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_OVER_LEFT, 0, 0, false);

    /* 绑定物理返回键: 与字体选择界面返回按钮等效 */
    touch_register_back_btn_callback(font_back_do);
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

/* WiFi界面返回 (屏幕返回与物理返回共用) */
static void wifi_back_do(void)
{
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

/* WiFi界面返回 */
static void wifi_back_cb(lv_event_t *e)
{
    (void)e;
    wifi_back_do();
}

/* 重新扫描按钮回调 */
static void wifi_rescan_cb(lv_event_t *e)
{
    (void)e;
    wifi_start_scan();
}

/* ===================================================
 * 触摸校准界面（十字光标逐点采样）
 * ===================================================
 * 屏幕一次只显示一个目标: 小圆环 + 十字线, 目标中心是已知显示坐标。
 * 用户对准十字中心点按 → 记录 (目标显示坐标, 面板原始坐标) 采样,
 * 圆环变实心(反馈) + 状态行计数, 自动进入下一个点。
 * 全部点采完后自动最小二乘拟合 x/y 两轴仿射映射:
 *   display = raw * num / den + off
 * 立即应用到驱动并持久化到 settings.ini [touch], 重启自动加载。
 * 面板原始坐标范围 > 屏幕分辨率, 无映射时底部/右侧误差随坐标增大
 * (表现为"按 G 出 V"这类错位), 校准后消除。
 */

#define CAL_POINTS 5

/* 校准采样点: 目标(显示坐标) + 面板原始坐标(校准前) */
typedef struct {
    int16_t  disp_x, disp_y;
    uint16_t raw_x, raw_y;
} CalSample_t;

/* 5 个采样目标: 左上/右上/中心/左下/右下, 覆盖全屏且相距远 */
static const lv_point_t s_cal_targets[CAL_POINTS] = {
    { 60,  90},   /* 0 左上 */
    {180,  90},   /* 1 右上 */
    {120, 205},   /* 2 中心 */
    { 60, 330},   /* 3 左下 */
    {180, 330},   /* 4 右下 */
};

static CalSample_t s_cal_samp[CAL_POINTS];
static int s_cal_step = 0;               /* 已采集点数 */
static int s_cal_done = 0;               /* 校准已应用 */
static lv_obj_t *g_cal_status_label = NULL;
static lv_obj_t *g_cal_ring = NULL;      /* 当前目标的圆环(采集后变实心) */
static lv_obj_t *g_cal_cross_h = NULL;   /* 当前目标的十字线(前进时删除) */
static lv_obj_t *g_cal_cross_v = NULL;

/* 最小二乘拟合 raw = scale * disp + off */
static void cal_ls_fit(const double *d, const double *r, int n,
                       double *scale, double *off)
{
    double md = 0.0, mr = 0.0;
    for (int i = 0; i < n; i++) { md += d[i]; mr += r[i]; }
    md /= n; mr /= n;
    double num = 0.0, den = 0.0;
    for (int i = 0; i < n; i++) {
        num += (d[i] - md) * (r[i] - mr);
        den += (d[i] - md) * (d[i] - md);
    }
    if (den == 0.0) { *scale = 1.0; *off = 0.0; return; }
    *scale = num / den;
    *off = mr - (*scale) * md;
}

/* 拟合全部采样, 应用仿射校准并持久化到 settings.ini [touch] */
static void cal_compute_and_apply(void)
{
    int n = s_cal_step;
    double dx[CAL_POINTS], rx[CAL_POINTS];
    double dy[CAL_POINTS], ry[CAL_POINTS];
    for (int i = 0; i < n; i++) {
        dx[i] = s_cal_samp[i].disp_x; rx[i] = s_cal_samp[i].raw_x;
        dy[i] = s_cal_samp[i].disp_y; ry[i] = s_cal_samp[i].raw_y;
    }

    double sx, ox, sy, oy;
    cal_ls_fit(dx, rx, n, &sx, &ox);   /* raw_x = sx*disp_x + ox */
    cal_ls_fit(dy, ry, n, &sy, &oy);   /* raw_y = sy*disp_y + oy */

    /* display = raw*scale + bias, 其中 scale = 1/s, bias = -o/s */
    double x_scale = 1.0 / sx, x_bias = -ox / sx;
    double y_scale = 1.0 / sy, y_bias = -oy / sy;

    /* 定点化: 比例用 num/1000 (CAL_FIXP 含 *1000), 偏移是像素量直接取整 */
    #define CAL_FIXP(s) ((int32_t)((s) * 1000.0 + ((s) >= 0 ? 0.5 : -0.5)))
    int32_t x_num = CAL_FIXP(x_scale), x_off = (int32_t)(x_bias + (x_bias >= 0 ? 0.5 : -0.5));
    int32_t y_num = CAL_FIXP(y_scale), y_off = (int32_t)(y_bias + (y_bias >= 0 ? 0.5 : -0.5));
    /* 防止病态参数: 比例限制 0.5x..2.0x, 偏移限制 ±300px (超出说明拟合失败) */
    if (x_num < 500) x_num = 500;
    if (x_num > 2000) x_num = 2000;
    if (y_num < 500) y_num = 500;
    if (y_num > 2000) y_num = 2000;
    if (x_off < -300) x_off = -300;
    if (x_off > 300) x_off = 300;
    if (y_off < -300) y_off = -300;
    if (y_off > 300) y_off = 300;

    CHSC6540_SetCalibration(x_num, 1000, x_off, y_num, 1000, y_off);

    /* 持久化到 settings.ini [touch] */
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", x_num); settings_set_string("touch", "sx_num", buf);
    snprintf(buf, sizeof(buf), "%d", x_off); settings_set_string("touch", "sx_off", buf);
    snprintf(buf, sizeof(buf), "%d", y_num); settings_set_string("touch", "sy_num", buf);
    snprintf(buf, sizeof(buf), "%d", y_off); settings_set_string("touch", "sy_off", buf);
    settings_set_string("touch", "valid", "1");

    s_cal_done = 1;
    SS_LOG("[CAL] applied from %d samples: x=%d/1000%+d y=%d/1000%+d",
           n, (int)x_num, (int)x_off, (int)y_num, (int)y_off);

    if (g_cal_status_label) {
        lv_label_set_text_fmt(g_cal_status_label,
                              "Done: x=%d/1000%+d  y=%d/1000%+d",
                              (int)x_num, (int)x_off, (int)y_num, (int)y_off);
        epd_mark_refresh_pending();
    }
}

/* 显示第 idx 个目标的十字光标 (小圆环 + 十字线, 不可点击) */
static void cal_show_target(lv_obj_t *scr, int idx)
{
    /* 前进时删除上一个目标的十字线, 圆环保留作已采样的实心点 */
    if (g_cal_cross_h) lv_obj_del(g_cal_cross_h);
    if (g_cal_cross_v) lv_obj_del(g_cal_cross_v);
    g_cal_cross_h = NULL;
    g_cal_cross_v = NULL;

    const lv_point_t *t = &s_cal_targets[idx];

    /* 圆环: 36x36 黑边圆圈 */
    lv_obj_t *ring = lv_obj_create(scr);
    lv_obj_set_size(ring, 36, 36);
    lv_obj_set_pos(ring, t->x - 18, t->y - 18);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_black(), 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    g_cal_ring = ring;

    /* 十字线: 两条 2px 黑条, 长 40 (lv_line 未编译进工程, 用细长 obj 代替) */
    lv_obj_t *lh = lv_obj_create(scr);
    lv_obj_set_size(lh, 40, 2);
    lv_obj_set_pos(lh, t->x - 20, t->y - 1);
    lv_obj_set_style_bg_opa(lh, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(lh, lv_color_black(), 0);
    lv_obj_set_style_border_width(lh, 0, 0);
    lv_obj_set_style_radius(lh, 0, 0);
    lv_obj_clear_flag(lh, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    g_cal_cross_h = lh;

    lv_obj_t *lv2 = lv_obj_create(scr);
    lv_obj_set_size(lv2, 2, 40);
    lv_obj_set_pos(lv2, t->x - 1, t->y - 20);
    lv_obj_set_style_bg_opa(lv2, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(lv2, lv_color_black(), 0);
    lv_obj_set_style_border_width(lv2, 0, 0);
    lv_obj_set_style_radius(lv2, 0, 0);
    lv_obj_clear_flag(lv2, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    g_cal_cross_v = lv2;
}

/* 屏幕任意处点按 = 采集当前目标点 */
static void cal_tap_cb(lv_event_t *e)
{
    lv_obj_t *scr = (lv_obj_t *)lv_event_get_user_data(e);
    /* 点按事件冒泡: 只有点按在背景(目标是屏幕本身)才算采样, 点 Back 忽略 */
    if (lv_event_get_target(e) != scr) return;
    if (s_cal_done || s_cal_step >= CAL_POINTS) return;

    /* 记录采样: 目标显示坐标 + 面板原始坐标(校准前) */
    uint16_t rx = 0, ry = 0;
    CHSC6540_GetLastRaw(&rx, &ry);
    const lv_point_t *t = &s_cal_targets[s_cal_step];
    s_cal_samp[s_cal_step].disp_x = t->x;
    s_cal_samp[s_cal_step].disp_y = t->y;
    s_cal_samp[s_cal_step].raw_x = rx;
    s_cal_samp[s_cal_step].raw_y = ry;

    /* 反馈: 当前圆环变实心黑点 */
    if (g_cal_ring) {
        lv_obj_set_style_bg_opa(g_cal_ring, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(g_cal_ring, lv_color_black(), 0);
        lv_obj_set_style_border_width(g_cal_ring, 0, 0);
    }
    s_cal_step++;
    SS_LOG("[CAL] sample %d: target=(%d,%d) raw=(%d,%d)",
           s_cal_step, t->x, t->y, (int)rx, (int)ry);

    if (s_cal_step >= CAL_POINTS) {
        cal_compute_and_apply();
    } else {
        cal_show_target(scr, s_cal_step);
        if (g_cal_status_label) {
            lv_label_set_text_fmt(g_cal_status_label,
                                  "Tap crosshair %d/%d",
                                  s_cal_step + 1, CAL_POINTS);
        }
    }
    epd_mark_refresh_pending();
}

/* 触摸测试界面返回 (屏幕返回与物理返回共用) */
static void touch_test_back_do(void)
{
    settings_screen_rebuild_main();
}

/* 触摸测试界面返回按钮回调 */
static void touch_test_back_cb(lv_event_t *e)
{
    (void)e;
    touch_test_back_do();
}

/* WiFi 设置页的"Touch Test"入口按钮回调 */
static void touch_test_open_cb(lv_event_t *e)
{
    (void)e;
    settings_touch_test_open();
}

/* Reset 按钮: 恢复恒等映射并清掉已保存校准 */
static void touch_cal_reset_cb(lv_event_t *e)
{
    (void)e;
    CHSC6540_ResetCalibration();
    settings_set_string("touch", "valid", "0");
    s_cal_step = 0;
    s_cal_done = 1;   /* 停止采样, 重新校准需退出后重进 */
    SS_LOG("[CAL] reset to identity, saved cal cleared");
    if (g_cal_status_label) {
        lv_label_set_text_fmt(g_cal_status_label, "Reset: no cal (re-enter to redo)");
        epd_mark_refresh_pending();
    }
}

/* 打开触摸校准界面（创建新屏幕） */
void settings_touch_test_open(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC |
                      LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN);

    /* 每次进入都从头采样 */
    s_cal_step = 0;
    s_cal_done = 0;
    g_cal_status_label = NULL;
    g_cal_ring = NULL;
    g_cal_cross_h = NULL;
    g_cal_cross_v = NULL;

    /* 标题 + 返回 */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Touch Cal");
    lv_obj_set_style_text_font(title, UI_FONT, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* Back: 统一左上角，尺寸与 FM 关闭按钮一致 */
    lv_obj_t *btn_back = lv_btn_create(scr);
    lv_obj_set_size(btn_back, 60, 30);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 2, 2);
    settings_apply_static_btn_style(btn_back, 1, 0);
    lv_obj_add_event_cb(btn_back, touch_test_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_set_style_text_font(lbl_back, UI_FONT, 0);
    lv_obj_center(lbl_back);

    /* Reset: 恢复恒等映射并清除已保存校准 */
    /* Reset: 与 Back 并排，统一左上角 */
    lv_obj_t *btn_reset = lv_btn_create(scr);
    lv_obj_set_size(btn_reset, 60, 30);
    lv_obj_align(btn_reset, LV_ALIGN_TOP_LEFT, 66, 2);
    settings_apply_static_btn_style(btn_reset, 1, 0);
    lv_obj_add_event_cb(btn_reset, touch_cal_reset_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_reset = lv_label_create(btn_reset);
    lv_label_set_text(lbl_reset, "Reset");
    lv_obj_set_style_text_font(lbl_reset, UI_FONT, 0);
    lv_obj_center(lbl_reset);

    /* 状态行: 提示当前目标 / 显示已应用参数 */
    g_cal_status_label = lv_label_create(scr);
    lv_label_set_text(g_cal_status_label, "Tap crosshair 1/5");
    lv_obj_set_style_text_font(g_cal_status_label, UI_FONT, 0);
    lv_obj_set_style_text_color(g_cal_status_label, lv_color_make(60, 60, 60), 0);
    lv_obj_align(g_cal_status_label, LV_ALIGN_TOP_LEFT, 8, 45);

    /* 背景点按 = 采集当前目标 (Back 按钮的点按不会冒泡成采样) */
    lv_obj_add_event_cb(scr, cal_tap_cb, LV_EVENT_CLICKED, scr);

    /* 第一个目标 */
    cal_show_target(scr, 0);

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_OVER_LEFT, 0, 0, false);
    epd_mark_refresh_pending();
    SS_LOG("Touch cal screen opened (%d crosshair points)", CAL_POINTS);

    /* 绑定物理返回键: 与触摸校准界面 Back 按钮等效 */
    touch_register_back_btn_callback(touch_test_back_do);
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

/* 缓存 scan button 上次状态，避免不变时触发 invalidation */
static char g_wifi_scan_btn_label_cached[32] = "";
static int  g_wifi_scan_btn_disabled_cached = -1;  /* -1=未初始化 */

static void wifi_update_scan_button(void)
{
    if (!g_wifi_scan_btn || !g_wifi_scan_btn_label) return;

    const char *new_label;
    int new_disabled;

    if (!g_wifi.enabled) {
        new_label = "WiFi Off";
        new_disabled = 1;
    } else if (g_wifi_scan_in_progress) {
        new_label = "Scanning...";
        new_disabled = 1;
    } else {
        new_label = LV_SYMBOL_WIFI " Rescan";
        new_disabled = 0;
    }

    /* 状态和文字都没变 → 跳过 */
    if (new_disabled == g_wifi_scan_btn_disabled_cached &&
        strcmp(g_wifi_scan_btn_label_cached, new_label) == 0) return;

    strncpy(g_wifi_scan_btn_label_cached, new_label, sizeof(g_wifi_scan_btn_label_cached) - 1);
    g_wifi_scan_btn_disabled_cached = new_disabled;

    lv_label_set_text(g_wifi_scan_btn_label, new_label);
    if (new_disabled) {
        lv_obj_add_state(g_wifi_scan_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(g_wifi_scan_btn, LV_STATE_DISABLED);
    }
    lv_obj_center(g_wifi_scan_btn_label);
}

/* 上一次 switch label 文字缓存（避免内容不变时也 invalidation→全屏重绘） */
static char g_wifi_switch_label_cached[80] = "";

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

    /* 内容不变就跳过——lv_label_set_text 内部必定触发 invalidation，
     * full_refresh=1 将其放大为 240x415 整屏重绘 + EPD 刷新。 */
    if (strcmp(g_wifi_switch_label_cached, text) == 0) return;
    strncpy(g_wifi_switch_label_cached, text, sizeof(g_wifi_switch_label_cached) - 1);

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

    /* 返回按钮（左上角）- 统一尺寸与 FM 关闭按钮一致 */
    lv_obj_t *btn_back = lv_btn_create(parent);
    lv_obj_set_size(btn_back, 60, 30);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 2, 2);
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

    /* 立即解析整屏布局 + 同步渲染到 framebuffer，稳定滚动条状态。
     * 仅 lv_obj_update_layout(g_wifi_list_cont) 不够——scrollbar 的
     * toggle 发生在 Screen 级的 lv_obj_update_layout(act_scr) 中。
     * lv_refr_now 确保本帧完整渲染，后续 REFR_TIMER 无脏布局可解析。 */
    lv_obj_update_layout(lv_scr_act());
    lv_refr_now(NULL);
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

/* 密码界面返回 (屏幕返回与物理返回共用) */
static void wifi_pwd_back_do(void)
{
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

    /* 现在 WiFi 界面处于活动状态, 物理返回切回 WiFi 返回处理 */
    touch_register_back_btn_callback(wifi_back_do);
}

/* 密码界面返回 */
static void wifi_pwd_back_cb(lv_event_t *e)
{
    (void)e;
    wifi_pwd_back_do();
}

/* 键盘ready回调 */
static void wifi_kb_ready_cb(lv_event_t *e)
{
    wifi_connect_btn_cb(e);
}

/* ========== WiFi 密码键盘自定义布局 (240px 屏, 1u = 24px) ==========
 * 排版原则（HTML prototype 验证过）:
 *   每行统一 10u → 1u 恒为 24px, 所有列对齐。
 *   行2 左 1u 占位、行3 左 2u 占位 → 标准 QWERTY 阶梯。
 *   行4: CAPS(2u) + 空格(4u) + ?123(2u) + DEL(2u)。
 * 占位键用 " " + LV_BTNMATRIX_CTRL_HIDDEN（仍占宽度但不可见不可点）。
 * 注意: 旧布局每行单位数不同(10/10/9/9)导致 1u 宽度每行不同(24 vs 26.7px),
 * 键位错乱 + 左侧占位无对称右占位 → 视觉右移且右侧缺一块。 */
static const char *kb_map_lc[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    " ", "a", "s", "d", "f", "g", "h", "j", "k", "l", " ", "\n",
    " ", " ", "z", "x", "c", "v", "b", "n", "m", " ", "\n",
    "CAPS", " ", "?123", "DEL", ""
};
/* ctrl 数组只含按钮(不含 \n)。每行 10u: 行2 左1u+9键+右1u, 行3 左2u+7键+右1u */
static const lv_btnmatrix_ctrl_t kb_ctrl_lc[] = {
    1,1,1,1,1,1,1,1,1,1,                          /* q-p (10u) */
    LV_BTNMATRIX_CTRL_HIDDEN | 1,                  /* 行2 左1u占位 */
    1,1,1,1,1,1,1,1,1,                            /* a-l (9u) */
    LV_BTNMATRIX_CTRL_HIDDEN | 1,                  /* 行2 右1u占位 */
    LV_BTNMATRIX_CTRL_HIDDEN | 1, LV_BTNMATRIX_CTRL_HIDDEN | 1,  /* 行3 左2u占位 */
    1,1,1,1,1,1,1,                                /* z-m (7u) */
    LV_BTNMATRIX_CTRL_HIDDEN | 1,                  /* 行3 右1u占位 */
    2, LV_BTNMATRIX_CTRL_HIDDEN | 4, 2, 2          /* CAPS, spc, ?123, DEL (10u) */
};

static const char *kb_map_uc[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
    " ", " ", "Z", "X", "C", "V", "B", "N", "M", " ", "\n",
    "CAPS", " ", "?123", "DEL", ""
};
static const lv_btnmatrix_ctrl_t kb_ctrl_uc[] = {
    1,1,1,1,1,1,1,1,1,1,                          /* Q-P (10u) */
    LV_BTNMATRIX_CTRL_HIDDEN | 1,                  /* 行2 左1u占位 */
    1,1,1,1,1,1,1,1,1,                            /* A-L (9u) */
    LV_BTNMATRIX_CTRL_HIDDEN | 1,                  /* 行2 右1u占位 */
    LV_BTNMATRIX_CTRL_HIDDEN | 1, LV_BTNMATRIX_CTRL_HIDDEN | 1,  /* 行3 左2u占位 */
    1,1,1,1,1,1,1,                                /* Z-M (7u) */
    LV_BTNMATRIX_CTRL_HIDDEN | 1,                  /* 行3 右1u占位 */
    2, LV_BTNMATRIX_CTRL_HIDDEN | 4, 2, 2          /* CAPS, spc, ?123, DEL (10u) */
};

/* 数字键盘: 3 行 × 10 键(1u) + 行4: ABC(2u) + 空格(6u) + DEL(2u) = 10u */
static const char *kb_map_num[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    ".", ",", "?", "!", "'", "\"", ":", ";", "-", "_", "\n",
    "+", "=", "*", "/", "\\", "#", "@", "&", "%", "$", "\n",
    "ABC", " ", "DEL", ""
};
static const lv_btnmatrix_ctrl_t kb_ctrl_num[] = {
    1,1,1,1,1,1,1,1,1,1,                      /* 1-0 (10u) */
    1,1,1,1,1,1,1,1,1,1,                       /* .-_ (10u) */
    1,1,1,1,1,1,1,1,1,1,                       /* +-$ (10u) */
    2, LV_BTNMATRIX_CTRL_HIDDEN | 6, 2          /* ABC, spc, DEL (10u) */
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

    /* 注意：此处绝不能再调用 epd_pause_refresh()！
     * 按键时 pause 会让 refresh_paused=1 → EPD 物理刷新永久冻结，
     * 之后 textarea 每次内容变化都触发整屏渲染但永不推屏 → LVGL
     * 无限空转（日志里按 G 后 e-Paper busy 消失但 EPD_RENDER 持续）。
     * 正确做法：每次按键正常推屏一帧（~755ms/键，墨水屏物理极限），
     * 配合 create_wifi_password_input() 里已禁用的光标闪烁，无死循环。 */
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
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    /* 密码输入框 */
    g_pwd_textarea = lv_textarea_create(scr);
    lv_obj_set_size(g_pwd_textarea, LV_HOR_RES - 20, 28);
    lv_obj_align(g_pwd_textarea, LV_ALIGN_TOP_MID, 0, 30);
    lv_textarea_set_placeholder_text(g_pwd_textarea, "Password");
    lv_textarea_set_text(g_pwd_textarea, "");
    lv_textarea_set_password_mode(g_pwd_textarea, 0);
    lv_textarea_set_one_line(g_pwd_textarea, 1);
    /* 关闭光标闪烁：墨水屏 full_refresh=1 下光标每次闪烁=整屏重绘=EPD 刷新，
     * 会造成"键盘卡死无限刷新"（REFR_TIMER 恒 inv_p=1）。墨水屏不需要光标动画。
     * 光标闪烁周期由 LV_PART_CURSOR 的 anim_time 样式控制，设为 0 即禁用。
     * 关键: 必须用 LV_PART_CURSOR | LV_STATE_ANY —— 仅 LV_PART_CURSOR 等价于
     * DEFAULT 状态，textarea 获得焦点(LV_STATE_FOCUSED)时 start_cursor_blink
     * 读的是 FOCUSED 状态的 anim_time（主题默认非0），动画照常启动！ */
    lv_obj_set_style_anim_time(g_pwd_textarea, 0, LV_PART_CURSOR | LV_STATE_ANY);
    
    /* 从 wifi_ap section 加载保存的密码 */
    char saved_pwd[65] = "";
    if (settings_get_string("wifi_ap", ssid, saved_pwd, sizeof(saved_pwd)) == 0
        && saved_pwd[0] != '\0') {
        lv_textarea_set_text(g_pwd_textarea, saved_pwd);
        SS_LOG("Loaded saved password for %s", ssid);
    }
    
    /* 连接按钮 */
    lv_obj_t *btn_connect = lv_btn_create(scr);
    lv_obj_set_size(btn_connect, LV_HOR_RES - 20, 28);
    lv_obj_align(btn_connect, LV_ALIGN_TOP_MID, 0, 62);
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
    lv_obj_set_size(btn_back, LV_HOR_RES - 20, 26);
    lv_obj_align(btn_back, LV_ALIGN_TOP_MID, 0, 94);
    settings_apply_static_btn_style(btn_back, 1, 3);
    lv_obj_add_event_cb(btn_back, wifi_pwd_back_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *label_back = lv_label_create(btn_back);
    lv_label_set_text(label_back, "Cancel");
    lv_obj_set_style_text_font(label_back, UI_FONT, 0);
    lv_obj_set_style_text_color(label_back, lv_color_black(), 0);
    lv_obj_center(label_back);
    
    /* 虚拟键盘 - 4 行 × 55px = 220px 高，贴底。
     * 旧版 276px(415*2/3) 每行 69px 造成 24×69 竖长条键 + 顶部 UI 被压缩。 */
    lv_obj_t *kb = lv_keyboard_create(scr);
    lv_obj_set_size(kb, LV_HOR_RES, 220);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, kb_map_lc, kb_ctrl_lc);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, kb_map_uc, kb_ctrl_uc);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_NUMBER,     kb_map_num, kb_ctrl_num);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, g_pwd_textarea);
    /* 自动聚焦 textarea，用户无需额外点击就能直接输入 */
    {
        lv_group_t *g = lv_group_get_default();
        if (g) lv_group_focus_obj(g_pwd_textarea);
    }
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

    /* 键盘绑定 + 样式设置完毕后再强制关闭光标闪烁（LV_STATE_ANY 覆盖聚焦态）。
     * lv_keyboard_set_textarea/聚焦等操作会触发 STYLE_CHANGED → start_cursor_blink，
     * 若此处不重设，blink 动画可能被重新启动（用户观察到光标仍闪烁）。 */
    lv_obj_set_style_anim_time(g_pwd_textarea, 0, LV_PART_CURSOR | LV_STATE_ANY);

    /* [DEBUG] 打印键盘实际几何 + 按钮区域，验证渲染/触摸一致性
     * 用户反馈"按 G 出 V"——怀疑键盘实际渲染位置与 LVGL 布局错位一行 */
    lv_obj_update_layout(kb);
    printf("[KB_DEBUG] kb pos=(%d,%d) size=(%d,%d) scr_h=%d\n",
           lv_obj_get_x(kb), lv_obj_get_y(kb),
           lv_obj_get_width(kb), lv_obj_get_height(kb),
           lv_obj_get_height(lv_scr_act()));

    /* 同步渲染到 framebuffer 后再请求 EPD 刷新, 避免 disp_task 在密码屏
     * 绘制前抢锁抓帧, 导致 EPD 刷出旧 WiFi 列表 (inv_p=1 被清, 密码屏永不
     * 上屏)。与 wifi_show_ap_list() 末尾一致: 先在锁内画完, EPD 才拿新帧。 */
    lv_obj_update_layout(lv_scr_act());
    lv_refr_now(NULL);

    epd_mark_refresh_pending();

    /* 绑定物理返回键: 与密码界面 Cancel 按钮等效 */
    touch_register_back_btn_callback(wifi_pwd_back_do);
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

/* 字体选择界面返回 (屏幕返回与物理返回共用) */
static void font_back_do(void)
{
    settings_screen_rebuild_main();
}

static void font_back_cb(lv_event_t *e)
{
    (void)e;
    font_back_do();
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


/* ===================================================
 * 关于界面 (两层: 固件信息 -> 作者信息)
 * 返回按钮样式与 WiFi 子页一致: 右上角 70x35 "Back"
 * (复用 settings_apply_static_btn_style, 保证与固件其他返回按钮一致)
 * =================================================== */

static lv_obj_t *g_about_fw_screen = NULL;   /* 屏① 固件信息 */
static char s_about_ver[16] = "";
static char s_about_build[32] = "";

/* 返回按钮: 左上角 60x30 "返回" (与设置主页返回按钮一致, 边框2 圆角4) */
static void about_make_back_btn(lv_obj_t *parent, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 60, 30);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 10, 20);
    settings_apply_static_btn_style(btn, 2, 4);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "返回");
    lv_obj_set_style_text_font(lbl, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
}

/* 屏② 作者信息: 返回 -> 屏① 固件信息 (屏幕返回与物理返回共用) */
static void about_author_back_do(void)
{
    if (g_about_fw_screen) {
        lv_obj_t *fw = g_about_fw_screen;
        g_about_fw_screen = NULL;
        lv_scr_load_anim(fw, LV_SCR_LOAD_ANIM_OVER_RIGHT, 0, 0, false);
        /* 切回屏① 固件信息, 物理返回转由它的处理接管 */
        touch_register_back_btn_callback(about_fw_back_do);
    }
}

/* 屏② 作者信息: 返回按钮回调 */
static void about_author_back_cb(lv_event_t *e)
{
    (void)e;
    about_author_back_do();
}

/* 屏① 固件信息: 返回 -> 设置主页 (屏幕返回与物理返回共用) */
static void about_fw_back_do(void)
{
    g_about_fw_screen = NULL;
    settings_screen_rebuild_main();
}

/* 屏① 固件信息: 返回按钮回调 */
static void about_fw_back_cb(lv_event_t *e)
{
    (void)e;
    about_fw_back_do();
}

/* 屏② 作者信息 */
static void create_about_author_screen(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_white(), 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "作者信息");
    lv_obj_set_style_text_font(title, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    about_make_back_btn(parent, about_author_back_cb);

    /* 头像 (1-bit 抖动位图) */
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, &avatar_eink_img);
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 66);

    lv_obj_t *name = lv_label_create(parent);
    lv_label_set_text(name, "hrp");
    lv_obj_set_style_text_font(name, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(name, lv_color_black(), 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 182);

    lv_obj_t *role = lv_label_create(parent);
    lv_label_set_text(role, "Inkfrog 固件作者");
    lv_obj_set_style_text_font(role, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(role, lv_color_black(), 0);
    lv_obj_align(role, LV_ALIGN_TOP_MID, 0, 204);

    /* 账号行: Bilibili */
    lv_obj_t *row_b = lv_obj_create(parent);
    lv_obj_set_size(row_b, 200, 40);
    lv_obj_align(row_b, LV_ALIGN_TOP_MID, 0, 226);
    lv_obj_set_style_border_width(row_b, 2, 0);
    lv_obj_set_style_border_color(row_b, lv_color_black(), 0);
    lv_obj_clear_flag(row_b, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *chip_b = lv_label_create(row_b);
    lv_label_set_text(chip_b, "B");
    lv_obj_set_style_bg_color(chip_b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(chip_b, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(chip_b, lv_color_white(), 0);
    lv_obj_set_style_text_font(chip_b, UI_FONT, 0);
    lv_obj_set_style_pad_all(chip_b, 3, 0);
    lv_obj_align(chip_b, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *id_b = lv_label_create(row_b);
    lv_label_set_text(id_b, "hrp8888");
    lv_obj_set_style_text_font(id_b, UI_FONT, 0);
    lv_obj_set_style_text_color(id_b, lv_color_black(), 0);
    lv_obj_align(id_b, LV_ALIGN_LEFT_MID, 40, 0);

    lv_obj_t *plat_b = lv_label_create(row_b);
    lv_label_set_text(plat_b, "Bilibili");
    lv_obj_set_style_text_font(plat_b, UI_FONT, 0);
    lv_obj_set_style_text_color(plat_b, lv_color_black(), 0);
    lv_obj_align(plat_b, LV_ALIGN_RIGHT_MID, -10, 0);

    /* 账号行: GitHub */
    lv_obj_t *row_g = lv_obj_create(parent);
    lv_obj_set_size(row_g, 200, 40);
    lv_obj_align(row_g, LV_ALIGN_TOP_MID, 0, 272);
    lv_obj_set_style_border_width(row_g, 2, 0);
    lv_obj_set_style_border_color(row_g, lv_color_black(), 0);
    lv_obj_clear_flag(row_g, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *chip_g = lv_label_create(row_g);
    lv_label_set_text(chip_g, "G");
    lv_obj_set_style_bg_color(chip_g, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(chip_g, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(chip_g, lv_color_white(), 0);
    lv_obj_set_style_text_font(chip_g, UI_FONT, 0);
    lv_obj_set_style_pad_all(chip_g, 3, 0);
    lv_obj_align(chip_g, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *id_g = lv_label_create(row_g);
    lv_label_set_text(id_g, "hrp518");
    lv_obj_set_style_text_font(id_g, UI_FONT, 0);
    lv_obj_set_style_text_color(id_g, lv_color_black(), 0);
    lv_obj_align(id_g, LV_ALIGN_LEFT_MID, 40, 0);

    lv_obj_t *plat_g = lv_label_create(row_g);
    lv_label_set_text(plat_g, "GitHub");
    lv_obj_set_style_text_font(plat_g, UI_FONT, 0);
    lv_obj_set_style_text_color(plat_g, lv_color_black(), 0);
    lv_obj_align(plat_g, LV_ALIGN_RIGHT_MID, -10, 0);

    /* 简介 */
    lv_obj_t *bio = lv_label_create(parent);
    lv_label_set_text(bio, "独立开发 Inkfrog，主打墨水屏与\n嵌入式折腾，欢迎 Star 与反馈。");
    lv_obj_set_style_text_font(bio, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(bio, lv_color_black(), 0);
    lv_obj_set_width(bio, 210);
    lv_obj_align(bio, LV_ALIGN_TOP_MID, 0, 330);

    /* 绑定物理返回键: 与作者信息屏返回按钮等效 */
    touch_register_back_btn_callback(about_author_back_do);
}

/* 屏① 里“作者信息”入口 -> 打开屏② */
static void about_author_open_cb(lv_event_t *e)
{
    lv_obj_t *fw = (lv_obj_t *)lv_event_get_user_data(e);
    g_about_fw_screen = fw;
    lv_obj_t *scr = lv_obj_create(NULL);
    create_about_author_screen(scr);
    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_OVER_LEFT, 0, 0, false);
}

/* 屏① 固件信息 */
static void create_about_fw_screen(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_white(), 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "关 于");
    lv_obj_set_style_text_font(title, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    about_make_back_btn(parent, about_fw_back_cb);

    /* 品牌行: Inkfrog logo (canvas 位图) */
    lv_obj_t *banner = lv_img_create(parent);
    lv_img_set_src(banner, &logo_eink_img);
    lv_obj_align(banner, LV_ALIGN_TOP_MID, 0, 58);

    /* tagline */
    lv_obj_t *tagline = lv_label_create(parent);
    lv_label_set_text(tagline, "途蛙单词卡 · 开源固件");
    lv_obj_set_style_text_font(tagline, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(tagline, lv_color_black(), 0);
    lv_obj_align(tagline, LV_ALIGN_TOP_MID, 0, 112);

    /* 信息行: 固件版本 */
    lv_obj_t *r_ver = lv_obj_create(parent);
    lv_obj_set_size(r_ver, 220, 46);
    lv_obj_align(r_ver, LV_ALIGN_TOP_MID, 0, 134);
    lv_obj_set_style_border_width(r_ver, 2, 0);
    lv_obj_set_style_border_color(r_ver, lv_color_black(), 0);
    lv_obj_clear_flag(r_ver, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *k1 = lv_label_create(r_ver);
    lv_label_set_text(k1, "固件版本");
    lv_obj_set_style_text_font(k1, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(k1, lv_color_black(), 0);
    lv_obj_align(k1, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *verl = lv_label_create(r_ver);
    snprintf(s_about_ver, sizeof s_about_ver, "v%s", INKFROG_VERSION_STR);
    lv_label_set_text(verl, s_about_ver);
    lv_obj_set_style_bg_color(verl, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(verl, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(verl, lv_color_white(), 0);
    lv_obj_set_style_text_font(verl, UI_FONT, 0);
    lv_obj_set_style_pad_all(verl, 3, 0);
    lv_obj_align(verl, LV_ALIGN_RIGHT_MID, -12, 0);

    /* 信息行: 编译时间 */
    lv_obj_t *r_build = lv_obj_create(parent);
    lv_obj_set_size(r_build, 220, 46);
    lv_obj_align(r_build, LV_ALIGN_TOP_MID, 0, 186);
    lv_obj_set_style_border_width(r_build, 2, 0);
    lv_obj_set_style_border_color(r_build, lv_color_black(), 0);
    lv_obj_clear_flag(r_build, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *k2 = lv_label_create(r_build);
    lv_label_set_text(k2, "编译时间");
    lv_obj_set_style_text_font(k2, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(k2, lv_color_black(), 0);
    lv_obj_align(k2, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *buildl = lv_label_create(r_build);
    snprintf(s_about_build, sizeof s_about_build, "%s %s",
             INKFROG_BUILD_DATE, INKFROG_BUILD_TIME);
    lv_label_set_text(buildl, s_about_build);
    lv_obj_set_style_text_font(buildl, UI_FONT, 0);
    lv_obj_set_style_text_color(buildl, lv_color_black(), 0);
    lv_obj_align(buildl, LV_ALIGN_RIGHT_MID, -12, 0);

    /* 入口行: 作者信息 */
    lv_obj_t *r_author = lv_btn_create(parent);
    lv_obj_set_size(r_author, 220, 52);
    lv_obj_align(r_author, LV_ALIGN_TOP_MID, 0, 238);
    lv_obj_set_style_bg_color(r_author, lv_color_white(), 0);
    lv_obj_set_style_border_width(r_author, 2, 0);
    lv_obj_set_style_border_color(r_author, lv_color_black(), 0);
    lv_obj_set_style_radius(r_author, 4, 0);
    lv_obj_clear_flag(r_author, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(r_author, about_author_open_cb, LV_EVENT_CLICKED, parent);

    lv_obj_t *k3 = lv_label_create(r_author);
    lv_label_set_text(k3, "作者信息");
    lv_obj_set_style_text_font(k3, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(k3, lv_color_black(), 0);
    lv_obj_align(k3, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *chev = lv_label_create(r_author);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(chev, UI_FONT, 0);
    lv_obj_set_style_text_color(chev, lv_color_black(), 0);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -12, 0);

    /* 开源库清单 (屏幕下方空余区) */
    static const char * const s_libs[] = {
        "LVGL 8.3.11", "FreeRTOS 8.2.3", "FatFs R0.12c",
        "miniz 10.2.0", "expat 2.8.0", "XR872 SDK",
    };
    const int n_libs = (int)(sizeof(s_libs) / sizeof(s_libs[0]));
    lv_obj_t *r_lib = lv_obj_create(parent);
    lv_obj_set_size(r_lib, 220, 106);
    lv_obj_align(r_lib, LV_ALIGN_TOP_MID, 0, 298);
    lv_obj_set_style_border_width(r_lib, 2, 0);
    lv_obj_set_style_border_color(r_lib, lv_color_black(), 0);
    lv_obj_clear_flag(r_lib, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *libh = lv_label_create(r_lib);
    lv_label_set_text(libh, "LIBRARIES");
    lv_obj_set_style_text_font(libh, UI_FONT, 0);
    lv_obj_set_style_text_color(libh, lv_color_black(), 0);
    lv_obj_set_style_border_width(libh, 0, 0);
    lv_obj_set_style_border_side(libh, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_align(libh, LV_ALIGN_TOP_LEFT, 10, 5);

    for (int i = 0; i < n_libs; i++) {
        lv_obj_t *li = lv_label_create(r_lib);
        lv_label_set_text(li, s_libs[i]);
        lv_obj_set_style_text_font(li, UI_FONT, 0);
        lv_obj_set_style_text_color(li, lv_color_black(), 0);
        lv_obj_align(li, LV_ALIGN_TOP_LEFT, 12, 22 + i * 13);
    }
}

/* 对外接口: 打开关于页 (屏① 固件信息) */
void settings_about_open(lv_obj_t *return_screen)
{
    (void)return_screen;
    g_about_fw_screen = NULL;
    lv_obj_t *scr = lv_obj_create(NULL);
    create_about_fw_screen(scr);
    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_OVER_LEFT, 0, 0, false);

    /* 绑定物理返回键: 与固件信息屏返回按钮等效 */
    touch_register_back_btn_callback(about_fw_back_do);
}
