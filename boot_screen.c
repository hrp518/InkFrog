/*
 * boot_screen — 开机画面实现
 *
 * 在 platform_init_level0() 之后、platform_init() 之前调用。
 * 直接画 framebuffer + 推 EPD, 不依赖 LVGL。
 * Wi-Fi 和 SD 卡初始化期间用户看到此画面, 而非黑/花屏。
 */
#include "boot_screen.h"
#include "boot_logo.h"
#include "epd.h"
#include <stdio.h>

/* 开机画面: 白底 + 居中大标题 "Inkfrog"(位图) + 下方一行 "booting..." */
void boot_screen_show(void)
{
    printf("[BOOT] drawing boot screen...\r\n");

    /* 1. 清为白底 */
    EPD_ClearBuffer();

    /* 2. 大大的标题 (来自 canvas.jpg 转换的位图, 居中) */
    boot_logo_draw(110);

    /* 3. 下方一行 booting... (正常大小, 居中) */
    EPD_DrawStringScaled("booting...", 185, 1, 10);

    /* 4. 推到 EPD (统一 DU 快刷, 正常极性白底黑字) */
    EPD_3IN52_Init();
    EPD_3IN52_Display();
    EPD_Sleep();

    printf("[BOOT] boot screen displayed\r\n");
}

/* 更新开机画面底部的状态行 (原 "booting..." 位置)。
 * 用于 boot 阶段耗时步骤 (TTF 字体预加载等) 给用户反馈:
 * 只清状态行区域再画新文字, logo 保留; 前后台都不依赖 LVGL。 */
void boot_screen_set_status(const char *text)
{
    if (!text || !text[0]) return;
    printf("[BOOT] status: %s\r\n", text);

    EPD_3IN52_Init();              /* boot_screen_show 结束时已 Sleep, 先唤醒 */

    /* 清状态行区域 (y=180~206, 全宽), 保留 logo */
    for (int y = 180; y <= 206; y++) {
        for (int x = 0; x < 240; x++) {
            EPD_SetPixel(x, y, 1); /* 1 = 白 */
        }
    }
    EPD_DrawStringScaled(text, 185, 1, 10);
    EPD_3IN52_Display();
    EPD_Sleep();
}
