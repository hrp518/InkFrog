/**
 * @file fm_ota.c
 * @brief FM 本地文件 OTA：整包镜像 file:// → SDK ota_get_image
 */
#include "fm_ota.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "lvgl/lvgl.h"
#include "lv_port_disp.h"
#include "fs/fatfs/ff.h"
#include "kernel/os/os.h"
#include "ota/ota.h"
#include "wifi_controller.h"
#include "wlan_manager.h"
#include "http_server.h"
#include "image/image.h"

extern void epd_disable_all_animations_recursive(lv_obj_t *obj);
extern const lv_font_t lv_font_misans_16;

#define FM_OTA_LOG(fmt, ...) printf("[FM_OTA] " fmt "\r\n", ##__VA_ARGS__)
#define FM_OTA_ERR(fmt, ...) printf("[FM_OTA ERR] " fmt "\r\n", ##__VA_ARGS__)

#define FM_OTA_MIN_SIZE   (256u * 1024u)
#define FM_OTA_STACK_SIZE (6 * 1024)

static OS_Thread_t s_ota_thread;
static volatile int s_ota_busy;
static char s_fat_path[512];
static char s_file_url[528];

static lv_obj_t *s_overlay;
static lv_obj_t *s_status_label;
static lv_obj_t *s_detail_label;
static lv_obj_t *s_ok_btn;

static char s_ui_status[96];
static char s_ui_detail[96];
static volatile int s_ui_show_ok;
static volatile int s_ui_dirty;
static volatile int s_epd_paused_for_ota;

static int path_ieq_ext(const char *filename, const char *ext)
{
    size_t n, e;
    if (!filename || !ext) return 0;
    n = strlen(filename);
    e = strlen(ext);
    if (n < e) return 0;
    for (size_t i = 0; i < e; i++) {
        if (tolower((unsigned char)filename[n - e + i]) !=
            tolower((unsigned char)ext[i]))
            return 0;
    }
    return 1;
}

int fm_ota_is_firmware_file(const char *filename)
{
    const char *base = filename;
    const char *slash;
    if (!filename || !filename[0]) return 0;
    slash = strrchr(filename, '/');
    if (slash) base = slash + 1;
    return path_ieq_ext(base, ".bin") || path_ieq_ext(base, ".img");
}

int fm_ota_is_busy(void)
{
    return s_ota_busy;
}

static int normalize_fat_path(const char *in, char *out, size_t out_sz)
{
    const char *p = in;
    if (!in || !out || out_sz < 8) return -1;

    if (strncmp(p, "file://", 7) == 0)
        p += 7;

    /* FM 常见 "//Font/..." */
    while (p[0] == '/' && p[1] == '/')
        p++;

    if (strncmp(p, "0:/", 3) == 0) {
        snprintf(out, out_sz, "%s", p);
    } else if (strncmp(p, "0:", 2) == 0) {
        snprintf(out, out_sz, "0:/%s", p + 2);
    } else if (p[0] == '/') {
        snprintf(out, out_sz, "0:%s", p);
    } else {
        snprintf(out, out_sz, "0:/%s", p);
    }
    return 0;
}

static uint32_t device_ota_file_max(void)
{
    const image_ota_param_t *iop = image_get_ota_param();
    uint32_t max_payload;

    if (!iop || iop->img_max_size == 0 || iop->img_max_size == IMAGE_INVALID_SIZE) {
        /* 回退：与当前 image.cfg max_size 同量级（8MB 双区 2044K） */
        return 2044u * 1024u;
    }
    /* 文件含 boot；可写载荷上限为 IMAGE_AREA_SIZE(img_max_size) */
    max_payload = IMAGE_AREA_SIZE(iop->img_max_size);
    return iop->bl_size + max_payload;
}

static int precheck_image(const char *fat_path, uint32_t *out_size)
{
    FIL fp;
    FRESULT res;
    UINT br;
    char magic[4];
    FSIZE_t sz;
    uint32_t max_sz = device_ota_file_max();

    res = f_open(&fp, fat_path, FA_READ);
    if (res != FR_OK) {
        FM_OTA_ERR("open fail %s res=%d", fat_path, (int)res);
        return -1;
    }

    sz = f_size(&fp);
    if (sz < FM_OTA_MIN_SIZE) {
        FM_OTA_ERR("size %lu too small (min %u)", (unsigned long)sz, FM_OTA_MIN_SIZE);
        f_close(&fp);
        return -2;
    }
    if (sz > max_sz) {
        FM_OTA_ERR("size %lu > device OTA slot %u (use matching xr_system.img)",
                   (unsigned long)sz, (unsigned)max_sz);
        f_close(&fp);
        return -5;
    }

    res = f_read(&fp, magic, 4, &br);
    f_close(&fp);
    if (res != FR_OK || br != 4) {
        FM_OTA_ERR("read magic fail");
        return -3;
    }
    if (memcmp(magic, "AWIH", 4) != 0) {
        FM_OTA_ERR("bad magic %02X%02X%02X%02X (need AWIH whole image)",
                   (unsigned)(uint8_t)magic[0], (unsigned)(uint8_t)magic[1],
                   (unsigned)(uint8_t)magic[2], (unsigned)(uint8_t)magic[3]);
        return -4;
    }

    if (out_size) *out_size = (uint32_t)sz;
    FM_OTA_LOG("precheck OK path=%s size=%lu max=%u",
               fat_path, (unsigned long)sz, (unsigned)max_sz);
    return 0;
}

static void ui_apply_cb(void *arg)
{
    (void)arg;
    if (!s_overlay) {
        s_ui_dirty = 0;
        return;
    }

    if (s_status_label && lv_obj_is_valid(s_status_label))
        lv_label_set_text(s_status_label, s_ui_status);
    if (s_detail_label && lv_obj_is_valid(s_detail_label))
        lv_label_set_text(s_detail_label, s_ui_detail);

    if (s_ui_show_ok && s_ok_btn && lv_obj_is_valid(s_ok_btn))
        lv_obj_clear_flag(s_ok_btn, LV_OBJ_FLAG_HIDDEN);

    if (!s_epd_paused_for_ota)
        epd_mark_refresh_pending();
    s_ui_dirty = 0;
}

/* force=1：立即排队一次 UI；写入过程中只改缓冲区，避免跟 EPD 抢互斥 */
static void ui_request_update(const char *status, const char *detail, int show_ok, int force)
{
    if (status) {
        strncpy(s_ui_status, status, sizeof(s_ui_status) - 1);
        s_ui_status[sizeof(s_ui_status) - 1] = '\0';
    }
    if (detail) {
        strncpy(s_ui_detail, detail, sizeof(s_ui_detail) - 1);
        s_ui_detail[sizeof(s_ui_detail) - 1] = '\0';
    }
    if (show_ok)
        s_ui_show_ok = 1;

    if (!force)
        return;

    if (!s_ui_dirty) {
        s_ui_dirty = 1;
        lv_async_call(ui_apply_cb, NULL);
    }
}

static void ota_progress_cb(ota_upgrade_status_t status, uint32_t data_size, uint32_t percentage)
{
    (void)data_size;
    /* 写 Flash 期间只打串口进度，不刷墨水屏（否则与 erase/write 竞态易 fault） */
    switch (status) {
    case OTA_UPGRADE_START:
        FM_OTA_LOG("progress: start");
        ui_request_update("开始更新", "请保持供电", 0, 0);
        break;
    case OTA_UPGRADE_UPDATING:
        if ((percentage % 10) == 0)
            FM_OTA_LOG("progress: %u%%", (unsigned)percentage);
        break;
    case OTA_UPGRADE_SUCCESS:
        FM_OTA_LOG("progress: success");
        ui_request_update("校验成功", "即将重启...", 0, 0);
        break;
    case OTA_UPGRADE_FAIL:
        FM_OTA_LOG("progress: fail @%u%%", (unsigned)percentage);
        ui_request_update("更新失败", "镜像不适配本机", 1, 0);
        break;
    case OTA_UPGRADE_STOP:
        FM_OTA_LOG("progress: stop");
        break;
    default:
        break;
    }
}

static void close_overlay_cb(lv_event_t *e)
{
    (void)e;
    if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
        s_status_label = NULL;
        s_detail_label = NULL;
        s_ok_btn = NULL;
    }
    s_ota_busy = 0;
    epd_mark_refresh_pending();
}

static void show_progress_overlay(const char *fname, uint32_t size)
{
    char line[96];

    if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }

    s_ui_show_ok = 0;
    snprintf(s_ui_status, sizeof(s_ui_status), "准备更新");
    snprintf(s_ui_detail, sizeof(s_ui_detail), "请勿断电");

    s_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_overlay, 240, 415);
    lv_obj_align(s_overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_overlay, 8, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    epd_disable_all_animations_recursive(s_overlay);

    lv_obj_t *title = lv_label_create(s_overlay);
    lv_label_set_text(title, "固件更新");
    lv_obj_set_style_text_font(title, &lv_font_misans_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *fn = lv_label_create(s_overlay);
    lv_label_set_text(fn, fname ? fname : "");
    lv_obj_set_style_text_font(fn, &lv_font_misans_16, 0);
    lv_label_set_long_mode(fn, LV_LABEL_LONG_DOT);
    lv_obj_set_width(fn, 220);
    lv_obj_align(fn, LV_ALIGN_TOP_MID, 0, 55);

    snprintf(line, sizeof(line), "大小: %u KB", (unsigned)(size / 1024));
    lv_obj_t *sz = lv_label_create(s_overlay);
    lv_label_set_text(sz, line);
    lv_obj_set_style_text_font(sz, &lv_font_misans_16, 0);
    lv_obj_align(sz, LV_ALIGN_TOP_MID, 0, 85);

    s_status_label = lv_label_create(s_overlay);
    lv_label_set_text(s_status_label, s_ui_status);
    lv_obj_set_style_text_font(s_status_label, &lv_font_misans_16, 0);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, -20);

    s_detail_label = lv_label_create(s_overlay);
    lv_label_set_text(s_detail_label, s_ui_detail);
    lv_obj_set_style_text_font(s_detail_label, &lv_font_misans_16, 0);
    lv_obj_align(s_detail_label, LV_ALIGN_CENTER, 0, 20);

    s_ok_btn = lv_btn_create(s_overlay);
    lv_obj_set_size(s_ok_btn, 100, 36);
    lv_obj_align(s_ok_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_add_flag(s_ok_btn, LV_OBJ_FLAG_HIDDEN);
    epd_disable_all_animations_recursive(s_ok_btn);
    lv_obj_add_event_cb(s_ok_btn, close_overlay_cb, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *lbl = lv_label_create(s_ok_btn);
        lv_label_set_text(lbl, "确定");
        lv_obj_set_style_text_font(lbl, &lv_font_misans_16, 0);
        lv_obj_center(lbl);
    }

    /* 点击「确认更新」后第一时间把「请勿断电」画面推上屏 (加载遮罩同样式)。
     * 仅 epd_mark_refresh_pending 只会把刷新排队, 且紧接的
     * prepare_network_for_ota 会阻塞 lvgl_task 数百 ms(停 HTTP/断 WiFi),
     * 期间 LVGL 不再重绘, 用户点击后迟迟看不到更新提示。
     * 这里强制同步渲染到 framebuffer + 标记 pending, disp_task 立刻推 EPD。 */
    lv_obj_invalidate(s_overlay);
    lv_refr_now(NULL);
    epd_mark_refresh_pending();

    /* 若同步渲染被 EPD 刷新抑制 (epd_refresh_in_progress 时 _lv_disp_refr_timer
     * 直接返回, inv_p 未被消费), 登记延迟重绘: 由 lvgl_task 在刷新结束后补渲染,
     * 否则遮罩画面永远不上屏。 */
    {
        lv_disp_t *disp = lv_disp_get_default();
        if (disp && disp->inv_p > 0) {
            epd_request_rerender();
        }
    }
}

static void show_alert(const char *title, const char *msg)
{
    static const char *btns[] = {"确定", ""};
    lv_obj_t *mbox = lv_msgbox_create(lv_scr_act(), title, msg, btns, true);
    epd_disable_all_animations_recursive(mbox);
    lv_obj_set_style_text_font(lv_msgbox_get_title(mbox), &lv_font_misans_16, 0);
    lv_obj_set_style_text_font(lv_msgbox_get_text(mbox), &lv_font_misans_16, 0);
    lv_obj_set_width(mbox, 220);
    lv_obj_center(mbox);
    epd_mark_refresh_pending();
}

static void prepare_network_for_ota(void)
{
    if (g_wifi.http_running) {
        http_server_stop();
        wifi_controller_set_http_running(0);
    }
    if (!g_wifi.fm_paused) {
        FM_OTA_LOG("pause wifi for OTA");
        g_wifi.fm_paused = 1;
        wlan_manager_cancel_connect();
        wlan_manager_disconnect();
        OS_MSleep(300);
    }
}

static void ota_task(void *arg)
{
    ota_verify_data_t verify_data;
    ota_verify_t verify_type;
    uint32_t *verify_value;
    ota_status_t st;
    (void)arg;

    FM_OTA_LOG("task start url=%s", s_file_url);

    /* 先把 fm_ota_start 里 show_progress_overlay 排队的「正在更新」overlay
     * 刷新到墨水屏，再暂停刷新/写 Flash；否则下面的 epd_pause_refresh()
     * 会把这个刚排队的 pending 吞掉，点击确认后迟迟看不到更新画面。 */
    epd_wait_refresh_idle(2500);

    /* 暂停墨水刷新，避免写 Flash 期间 DU 与 OTA erase 互斥拖死/踩堆 */
    epd_pause_refresh();
    s_epd_paused_for_ota = 1;

    ota_set_cb(ota_progress_cb);
    st = ota_get_image(OTA_PROTOCOL_FILE, s_file_url);
    ota_set_cb(NULL);

    if (st != OTA_STATUS_OK) {
        FM_OTA_ERR("ota_get_image failed");
        ui_request_update("更新失败", "镜像过大或不适配", 1, 0);
        s_epd_paused_for_ota = 0;
        epd_resume_refresh();
        OS_MSleep(100);
        ui_request_update(NULL, NULL, 1, 1);
        s_ota_busy = 0;
        OS_ThreadDelete(&s_ota_thread);
        return;
    }

    if (ota_get_verify_data(&verify_data) != OTA_STATUS_OK) {
        verify_type = OTA_VERIFY_NONE;
        verify_value = NULL;
    } else {
        verify_type = (ota_verify_t)verify_data.ov_type;
        verify_value = (uint32_t *)(verify_data.ov_data);
    }

    ui_request_update("正在校验", "请稍候", 0, 0);
    if (ota_verify_image(verify_type, verify_value) != OTA_STATUS_OK) {
        FM_OTA_ERR("ota_verify_image failed");
        ui_request_update("校验失败", "固件未切换", 1, 0);
        s_epd_paused_for_ota = 0;
        epd_resume_refresh();
        OS_MSleep(100);
        ui_request_update(NULL, NULL, 1, 1);
        s_ota_busy = 0;
        OS_ThreadDelete(&s_ota_thread);
        return;
    }

    FM_OTA_LOG("verify OK, rebooting...");
    ui_request_update("即将重启", "请稍候", 0, 0);
    s_epd_paused_for_ota = 0;
    epd_resume_refresh();
    OS_MSleep(200);
    ui_request_update(NULL, NULL, 0, 1);
    OS_MSleep(800);
    ota_reboot();

    s_ota_busy = 0;
    OS_ThreadDelete(&s_ota_thread);
}

int fm_ota_start(const char *filepath)
{
    uint32_t size = 0;
    int pr;
    const char *fname;

    if (s_ota_busy) {
        show_alert("固件更新", "正在更新中");
        return -1;
    }
    if (!filepath || !filepath[0]) {
        show_alert("固件更新", "无效路径");
        return -1;
    }
    if (!fm_ota_is_firmware_file(filepath)) {
        show_alert("固件更新", "请选择 .bin/.img");
        return -1;
    }

    if (normalize_fat_path(filepath, s_fat_path, sizeof(s_fat_path)) != 0) {
        show_alert("固件更新", "路径错误");
        return -1;
    }

    pr = precheck_image(s_fat_path, &size);
    if (pr == -4) {
        show_alert("非法固件", "需要整包 AWIH\n(xr_system.img)");
        return -1;
    }
    if (pr == -5) {
        char msg[96];
        snprintf(msg, sizeof(msg), "文件过大\n上限约 %uKB\n请用本机 mkimage 产物",
                 (unsigned)(device_ota_file_max() / 1024));
        show_alert("非法固件", msg);
        return -1;
    }
    if (pr == -2) {
        show_alert("非法固件", "文件过小");
        return -1;
    }
    if (pr != 0) {
        show_alert("固件更新", "无法读取文件");
        return -1;
    }

    snprintf(s_file_url, sizeof(s_file_url), "file://%s", s_fat_path);
    fname = strrchr(s_fat_path, '/');
    fname = fname ? fname + 1 : s_fat_path;

    /* 先显示「正在更新」overlay，再停 wifi / 起 OTA 线程；
     * 否则停 wifi 那几百毫秒里屏幕无任何反馈。而 ota_task 内会
     * epd_wait_refresh_idle() 等这次刷新真正上屏后才暂停/写 Flash。 */
    show_progress_overlay(fname, size);
    prepare_network_for_ota();

    s_ota_busy = 1;
    if (OS_ThreadCreate(&s_ota_thread, "fm_ota", ota_task, NULL,
                        OS_THREAD_PRIO_APP, FM_OTA_STACK_SIZE) != OS_OK) {
        FM_OTA_ERR("create thread fail");
        s_ota_busy = 0;
        show_alert("固件更新", "无法创建任务");
        return -1;
    }

    FM_OTA_LOG("started %s", s_file_url);
    return 0;
}
