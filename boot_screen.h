/*
 * boot_screen - 开机画面 (直接在 framebuffer 绘制, 不依赖 LVGL)
 *
 * 调用时机: platform_init_level0() + EPD_GPIO_Init_Public() 之后,
 * platform_init() (WiFi/SD) 之前。此时 flash/PSRAM/cache 就绪,
 * framebuffer 可访问, EPD GPIO 可驱动。
 */
#ifndef BOOT_SCREEN_H
#define BOOT_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

/* 显示开机画面: 白底 + 居中 "Tuwa" 字样 + 简单装饰边框。
 * 直接操作 framebuffer[] + EPD_3IN52_Init/Display, 不依赖 LVGL。
 * 调用后 EPD 进入 deep sleep, framebuffer 可被后续使用。 */
void boot_screen_show(void);

/* 更新开机画面底部状态行 (booting... 位置), 保留 logo。
 * 仅 boot 阶段 (LVGL 启动前) 使用: 唤醒 EPD → 清状态行 → 画新文字 → 推屏 → 睡眠。 */
void boot_screen_set_status(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_SCREEN_H */
