/* loading.h - 通用 "加载中..." 提示窗口
 *
 * 用于长操作 (EPUB 打开 / 章节切换 / 字体预热) 期间给用户反馈,
 * 避免误以为设备卡死。e-paper 友好: 纯静态文字, 无动画。
 *
 * 用法:
 *   loading_show("Opening EPUB...");
 *   ... 阻塞工作 ...
 *   loading_hide();
 *
 * 线程安全: 只能在 LVGL 线程 (lvgl_task / lv_async_call 回调) 内调用。
 * loading_show 会同步把窗口渲染到 framebuffer 并标记 EPD 刷新,
 * 阻塞工作期间 disp_task 线程并行把 framebuffer 推到墨水屏。
 */
#ifndef LOADING_H
#define LOADING_H

#ifdef __cplusplus
extern "C" {
#endif

/* 显示全屏遮罩 + 居中文字框, 强制同步刷屏。
 * msg 为 NULL 时显示默认 "Loading..."。重复调用会先 hide 旧的。 */
void loading_show(const char *msg);

/* 销毁遮罩, 强制同步刷屏。无遮罩时是空操作。 */
void loading_hide(void);

#ifdef __cplusplus
}
#endif

#endif /* LOADING_H */
