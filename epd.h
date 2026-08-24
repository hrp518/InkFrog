/*
 * E-Paper 3.52 inch Driver Header (移植自ESP32原始驱动)
 * 
 * Pin映射关系 (ESP32 → XR872):
 * ESP32 IO13 (SCK)  → XR872 PA2
 * ESP32 IO14 (MOSI) → XR872 PA0
 * ESP32 IO15 (CS)   → XR872 PB4
 * ESP32 IO25 (BUSY) → XR872 PA8
 * ESP32 IO26 (RST)  → XR872 PA9
 * ESP32 IO27 (DC)   → XR872 PA1
 */

#ifndef _EPD_H_
#define _EPD_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Display resolution */
#define EPD_WIDTH       240
#define EPD_HEIGHT      415

/* Color definitions */
#define EPD_WHITE       0xFF
#define EPD_BLACK       0x00

/* Frame buffer - must be accessible by LVGL port */
extern uint8_t framebuffer[240 * 415 / 8];
#define EPD_BUFFER_SIZE (240 * 415 / 8)

/* Initialize EPD */
int EPD_3IN52_Init(void);

/* Display frame buffer */
void EPD_3IN52_Display(void);

/* Clear screen */
void EPD_3IN52_Clear(void);

/* Refresh display */
void EPD_3IN52_refresh(void);

/* Deep sleep */
void EPD_3IN52_Show(void);

/* Fill frame buffer with pattern */
void EPD_FillBuffer(uint8_t pattern);

/* Set pixel in frame buffer */
void EPD_SetPixel(uint16_t x, uint16_t y, uint8_t color);

/* Clear frame buffer */
void EPD_ClearBuffer(void);

/* Display test pattern */
void EPD_DisplayTestPattern(void);

/* Display Hello World */
void EPD_DisplayHelloWorld(void);

/* DU (fast) refresh functions */
void EPD_3IN52_Init_DU(void);
void EPD_3IN52_Display_DU(void);
void EPD_DisplayHelloWorld_DU(void);

/* Sleep function */
void EPD_Sleep(void);

/* Graphics primitive */
void EPD_DrawLine(int x0, int y0, int x1, int y1);

/*============================================================
 * 休眠时钟专用同步接口 (GPT clock 方案 §五/§六)
 * 在现有 void 接口基础上增加薄封装，返回 0=成功 / 非0=失败，
 * 供 clock_mode.c 分钟冷启动路径使用。现有时序全部保留。
 *============================================================*/

/* 初始化 EPD GPIO (PA0/1/2/3/8/9)；正常 InkFrog 路径由 EPD_3IN52_Init 内部调用，
 * 时钟冷启动路径需单独调一次（该路径不跑 Init 全流程）。 */
void EPD_GPIO_Init_Public(void);

/* 硬件复位 (RST 200ms / 2ms / 200ms)，公开给时钟错误恢复路径 */
void EPD_HardwareReset(void);

/* 等待 BUSY 高电平(空闲)，超时返回非0 */
int  EPD_WaitIdle(uint32_t timeout_ms);

/* DU 快刷同步版：初始化 + 发整帧 + 刷新触发 + 等 BUSY，返回 0/非0 */
int  EPD_3IN52_Init_DU_Sync(void);
int  EPD_3IN52_Display_DU_Sync(const uint8_t *frame);

/* GC 全刷同步版 */
int  EPD_3IN52_Init_GC_Sync(void);
int  EPD_3IN52_Display_GC_Sync(const uint8_t *frame);

/* EPD 深度睡眠同步版 (cmd 0x07 / 0xA5) */
int  EPD_Sleep_Sync(void);

/*============================================================
 * 大号数字字模 (时钟显示)
 *============================================================*/
/* 字模尺寸：48 像素宽 × 96 像素高，1bpp，每行 6 字节，共 576 字节 */
#define EPD_DIGIT_W   48
#define EPD_DIGIT_H   96
#define EPD_COLON_W   24
#define EPD_COLON_H   96

/* 在 framebuffer 上画一个大号数字 '0'-'9' 或冒号 ':'，黑色 */
void EPD_DrawDigitLarge(uint16_t x, uint16_t y, char c);

/* 取某字符字模指针，无则返回 NULL */
const uint8_t *EPD_GetLargeGlyph(char c);

/* Draw text string centered on screen */
void EPD_DrawStringCentered(const char *text);
/* 在指定 y 处水平居中绘制字符串, 支持放大 (scale) 与字距 (advance) */
void EPD_DrawStringScaled(const char *text, uint16_t y, uint8_t scale, uint16_t advance);

/* Animation Demos */
void EPD_Demo_DrawingAnimation(void);
void EPD_Demo_TypingEffect(void);

#ifdef __cplusplus
}
#endif

#endif /* _EPD_H_ */
