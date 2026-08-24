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

/* 校验遮罩对象是否仍然存活(仍是活动屏的直接子对象)。
 * 调用方可能直接 lv_obj_clean(活动屏) 重建 UI, 从而在未经过 loading_hide()
 * 的情况下把 s_bg 连带删除 —— 此时 s_bg 是悬垂指针, 若直接 lv_obj_del()
 * 会造成 use-after-free / TLSF 双重释放(堆破坏 → 开机约 30s 后整机卡死)。
 * 这里通过活动屏的子对象列表确认对象还活着, 避免对已释放内存做任何操作。 */
static int loading_overlay_alive(void)
{
    lv_obj_t *scr = lv_scr_act();
    uint32_t i, cnt;

    if (!s_bg || !scr) {
        return 0;
    }
    cnt = lv_obj_get_child_cnt(scr);
    for (i = 0; i < cnt; i++) {
        if (lv_obj_get_child(scr, i) == s_bg) {
            return 1;
        }
    }
    return 0;
}

void loading_show(const char *msg)
{
    if (s_bg) {
        if (loading_overlay_alive()) {
            loading_hide();          /* 正常关闭旧遮罩 */
        } else {
            s_bg = s_box = s_label = NULL;  /* 已被外部删除, 只清指针 */
        }
    }

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

    /* 若同步渲染被 EPD 刷新抑制 (epd_refresh_in_progress=1 时 _lv_disp_refr_timer
     * 直接返回, inv_p 未被消费), 登记延迟重绘: 由 lvgl_task 在刷新结束后补渲染,
     * 否则遮罩画面永远不上屏。 */
    {
        lv_disp_t *disp = lv_disp_get_default();
        if (disp && disp->inv_p > 0) {
            epd_request_rerender();
        }
    }
}

void loading_hide(void)
{
    if (s_bg && loading_overlay_alive()) {
        lv_obj_del(s_bg);
        s_bg = s_box = s_label = NULL;
        lv_obj_invalidate(lv_scr_act());
        lv_refr_now(NULL);
        epd_mark_refresh_pending();

        /* 【关键】若同步渲染被 EPD 刷新抑制 (inv_p 未被消费, 刷新窗口内
         * _lv_disp_refr_timer 被 epd_refresh_in_progress 挡住), 登记延迟重绘。
         * 否则遮罩删除后 inv_p 会被 epd_do_refresh 清掉, 底层画面永远不再
         * 渲染上屏 —— 表现为屏幕停在 "Preparing fonts..."。 */
        {
            lv_disp_t *disp = lv_disp_get_default();
            if (disp && disp->inv_p > 0) {
                epd_request_rerender();
            }
        }
    } else {
        /* 无有效遮罩(或已被外部删除): 只需清指针, 绝不能 lv_obj_del 悬垂指针 */
        s_bg = s_box = s_label = NULL;
    }
}
