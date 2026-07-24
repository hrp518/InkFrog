/* loading.c - 通用 "加载中..." 提示窗口实现
 *
 * 复用 http_dialog_create (main.c) 的全屏遮罩 + 居中白底框样式。
 * e-paper 约束: 纯静态文字, 无 spinner/动画 (会闪烁)。
 */
#include "loading.h"
#include "lvgl/lvgl.h"
#include "lv_port_disp.h"

extern const lv_font_t lv_font_montserrat_12;

static lv_obj_t *s_bg = NULL;     /* 全屏遮罩 */
static lv_obj_t *s_box = NULL;    /* 居中文字框 */
static lv_obj_t *s_label = NULL;  /* 文字 */

void loading_show(const char *msg)
{
    if (s_bg) loading_hide();   /* 避免重复 */

    /* 1. 全屏白底遮罩 (盖住底层 UI) */
    s_bg = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_bg);
    lv_obj_set_size(s_bg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_bg, 0, 0);
    lv_obj_set_style_bg_color(s_bg, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_bg, LV_OPA_90, 0);
    lv_obj_clear_flag(s_bg, LV_OBJ_FLAG_SCROLLABLE);

    /* 2. 居中白底黑字框 */
    s_box = lv_obj_create(s_bg);
    lv_obj_set_size(s_box, 180, 80);
    lv_obj_align(s_box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_box, 2, 0);
    lv_obj_set_style_border_color(s_box, lv_color_black(), 0);
    lv_obj_set_style_radius(s_box, 8, 0);
    lv_obj_clear_flag(s_box, LV_OBJ_FLAG_SCROLLABLE);

    /* 3. 文字 (黑字, montserrat_16 居中) */
    s_label = lv_label_create(s_box);
    lv_label_set_text(s_label, msg ? msg : "Loading...");
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_label, lv_color_black(), 0);
    lv_obj_center(s_label);

    /* 4. 强制同步刷屏: 渲染到 framebuffer + 标记 pending。
     * disp_task 线程会在下一个周期把 framebuffer 推到 EPD。
     * 即使本函数返回后阻塞几秒, disp_task 仍会并行完成刷屏。 */
    lv_obj_invalidate(s_bg);
    lv_refr_now(NULL);
    epd_mark_refresh_pending();
}

void loading_hide(void)
{
    if (s_bg) {
        lv_obj_del(s_bg);
        s_bg = s_box = s_label = NULL;
        lv_obj_invalidate(lv_scr_act());
        lv_refr_now(NULL);
        epd_mark_refresh_pending();
    }
}
