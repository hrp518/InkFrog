#ifndef BOOT_LOGO_H
#define BOOT_LOGO_H

#include <stdint.h>

#define BOOT_TITLE_W 232
#define BOOT_TITLE_H 45
#define BOOT_TITLE_ROWBYTES ((BOOT_TITLE_W + 7) / 8)

extern const uint8_t boot_title_bmp[1305];

/* 居中横向, 自顶部 y0 起, 将标题位图画入 framebuffer */
void boot_logo_draw(uint16_t y0);

#endif /* BOOT_LOGO_H */
