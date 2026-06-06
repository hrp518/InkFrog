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

/* Draw text string centered on screen */
void EPD_DrawStringCentered(const char *text);

/* Animation Demos */
void EPD_Demo_DrawingAnimation(void);
void EPD_Demo_TypingEffect(void);

#ifdef __cplusplus
}
#endif

#endif /* _EPD_H_ */
