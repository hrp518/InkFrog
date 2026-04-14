/*
 * E-Paper 3.52 inch Driver (移植自ESP32原始驱动到XR872)
 * 基于Waveshare 3.52寸墨水屏驱动
 * 
 * Pin映射关系 (ESP32 → XR872):
 * ESP32 IO13 (SCK)  → XR872 PA2
 * ESP32 IO14 (MOSI) → XR872 PA0
 * ESP32 IO15 (CS)   → XR872 PB4
 * ESP32 IO25 (BUSY) → XR872 PA8
 * ESP32 IO26 (RST)  → XR872 PA9
 * ESP32 IO27 (DC)   → XR872 PA1
 */

#include <stdio.h>
#include <string.h>
#include "driver/chip/hal_gpio.h"
#include "kernel/os/os.h"

/* Frame buffer - 240x415 pixels = 12450 bytes */
/* Note: framebuffer must be accessible by lv_port_disp.c, so NOT static */
uint8_t framebuffer[240 * 415 / 8];

/* Global flag for display state */
static uint8_t EPD_3IN52_Flag = 0;

/* LUT tables from original driver */
static const uint8_t EPD_3IN52_lut_R20_GC[] = {
    0x01,0x0f,0x0f,0x0f,0x01,0x01,0x01,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

static const uint8_t EPD_3IN52_lut_R21_GC[] = {
    0x01,0x4f,0x8f,0x0f,0x01,0x01,0x01,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

static const uint8_t EPD_3IN52_lut_R22_GC[] = {
    0x01,0x0f,0x8f,0x0f,0x01,0x01,0x01,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

static const uint8_t EPD_3IN52_lut_R23_GC[] = {
    0x01,0x4f,0x8f,0x4f,0x01,0x01,0x01,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

static const uint8_t EPD_3IN52_lut_R24_GC[] = {
    0x01,0x0f,0x8f,0x4f,0x01,0x01,0x01,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* DU refresh LUT tables - for fast update mode (0.3s refresh) */
static const uint8_t EPD_3IN52_lut_R20_DU[] = {
    0x01,0x0f,0x01,0x00,0x00,0x01,0x01,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

static const uint8_t EPD_3IN52_lut_R21_DU[] = {
    0x01,0x0f,0x01,0x00,0x00,0x01,0x01,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

static const uint8_t EPD_3IN52_lut_R22_DU[] = {
    0x01,0x8f,0x01,0x00,0x00,0x01,0x01,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

static const uint8_t EPD_3IN52_lut_R23_DU[] = {
    0x01,0x4f,0x01,0x00,0x00,0x01,0x01,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

static const uint8_t EPD_3IN52_lut_R24_DU[] = {
    0x01,0x0f,0x01,0x00,0x00,0x01,0x01,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* Common LUT tables for DU mode */
static const uint8_t EPD_3IN52_lut_vcom[] = {
    0x01,0x19,0x19,0x19,0x19,0x01,0x01,
    0x01,0x19,0x19,0x19,0x01,0x01,0x01,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

static const uint8_t EPD_3IN52_lut_ww[] = {
    0x01,0x59,0x99,0x59,0x99,0x01,0x01,
    0x01,0x59,0x99,0x19,0x01,0x01,0x01,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

static const uint8_t EPD_3IN52_lut_bw[] = {
    0x01,0x59,0x99,0x59,0x99,0x01,0x01,
    0x01,0x59,0x99,0x19,0x01,0x01,0x01,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

static const uint8_t EPD_3IN52_lut_wb[] = {
    0x01,0x19,0x99,0x59,0x99,0x01,0x01,
    0x01,0x59,0x99,0x59,0x01,0x01,0x01,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

static const uint8_t EPD_3IN52_lut_bb[] = {
    0x01,0x19,0x99,0x59,0x99,0x01,0x01,
    0x01,0x59,0x99,0x59,0x01,0x01,0x01,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/*
 * GPIO Pin definitions - 严格按照ESP32→XR872映射:
 * PA0 = ESP32 IO14 (MOSI/DIN)
 * PA1 = ESP32 IO27 (DC)
 * PA2 = ESP32 IO13 (SCK)
 * PA3 = ESP32 IO15 (CS) - 改为PA3避免与Flash冲突
 * PA8 = ESP32 IO25 (BUSY)
 * PA9 = ESP32 IO26 (RST)
 */

/* Initialize GPIO pins for EPD */
static void EPD_GPIO_Init(void)
{
    GPIO_InitParam param;
    
    param.driving = GPIO_DRIVING_LEVEL_1;
    param.mode = GPIOx_Pn_F1_OUTPUT;
    param.pull = GPIO_PULL_NONE;
    
    /* PA0 = MOSI (ESP32 IO14) */
    HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_0, &param);
    /* PA1 = DC (ESP32 IO27) */
    HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_1, &param);
    /* PA2 = SCK (ESP32 IO13) */
    HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_2, &param);
    /* PA3 = CS (ESP32 IO15) - 改为PA3避免与系统Flash冲突 */
    HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_3, &param);
    /* PA9 = RST (ESP32 IO26) */
    HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_9, &param);
    
    /* PA8 = BUSY (ESP32 IO25) - as input */
    param.mode = GPIOx_Pn_F0_INPUT;
    HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_8, &param);
    
    /* Set default output levels */
    HAL_GPIO_WritePin(GPIO_PORT_A, GPIO_PIN_3, GPIO_PIN_HIGH);  // CS HIGH
    HAL_GPIO_WritePin(GPIO_PORT_A, GPIO_PIN_9, GPIO_PIN_HIGH);  // RST HIGH
    HAL_GPIO_WritePin(GPIO_PORT_A, GPIO_PIN_1, GPIO_PIN_HIGH);  // DC HIGH
    HAL_GPIO_WritePin(GPIO_PORT_A, GPIO_PIN_2, GPIO_PIN_LOW);   // SCK LOW
}

/* Software SPI transfer - send one byte */
static void EpdSpiTransferCallback(uint8_t data)
{
    HAL_GPIO_WritePin(GPIO_PORT_A, GPIO_PIN_3, GPIO_PIN_LOW);  // CS LOW
    
    for (int i = 0; i < 8; i++) {
        if (data & 0x80) {
            HAL_GPIO_WritePin(GPIO_PORT_A, GPIO_PIN_0, GPIO_PIN_HIGH);  // MOSI HIGH
        } else {
            HAL_GPIO_WritePin(GPIO_PORT_A, GPIO_PIN_0, GPIO_PIN_LOW);   // MOSI LOW
        }
        data <<= 1;
        HAL_GPIO_WritePin(GPIO_PORT_A, GPIO_PIN_2, GPIO_PIN_HIGH);  // SCK HIGH
        HAL_GPIO_WritePin(GPIO_PORT_A, GPIO_PIN_2, GPIO_PIN_LOW);   // SCK LOW
    }
    
    HAL_GPIO_WritePin(GPIO_PORT_A, GPIO_PIN_3, GPIO_PIN_HIGH);  // CS HIGH
}

/* Send command to EPD */
static void EPD_SendCommand(uint8_t command)
{
    HAL_GPIO_WritePin(GPIO_PORT_A, GPIO_PIN_1, GPIO_PIN_LOW);  // DC LOW = Command
    EpdSpiTransferCallback(command);
}

/* Send data to EPD */
static void EPD_SendData(uint8_t data)
{
    HAL_GPIO_WritePin(GPIO_PORT_A, GPIO_PIN_1, GPIO_PIN_HIGH);  // DC HIGH = Data
    EpdSpiTransferCallback(data);
}

/* Reset EPD */
static void EPD_Reset(void)
{
    HAL_GPIO_WritePin(GPIO_PORT_A, GPIO_PIN_9, GPIO_PIN_HIGH);
    OS_MSleep(200);
    HAL_GPIO_WritePin(GPIO_PORT_A, GPIO_PIN_9, GPIO_PIN_LOW);
    OS_MSleep(2);
    HAL_GPIO_WritePin(GPIO_PORT_A, GPIO_PIN_9, GPIO_PIN_HIGH);
    OS_MSleep(200);
}

/* Wait until EPD is not busy */
static void EPD_3IN52_ReadBusy(void)
{
    printf("e-Paper busy\r\n");
    uint8_t busy;
    int timeout = 0;
    
    // 全刷可能需要更长时间，最多等待5秒
    while (timeout < 5000) {
        busy = HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_8);  // PA8 = BUSY
        if (busy) {
            // BUSY为高时退出
            break;
        }
        OS_MSleep(1);
        timeout++;
    }
    
    if (timeout >= 5000) {
        printf("[EPD FATAL] e-Paper busy timeout!\r\n");
    } else {
        OS_MSleep(200);
        printf("e-Paper busy release\r\n");
    }
}

/* Refresh display */
static void EPD_3IN52_refresh(void)
{
    EPD_SendCommand(0x17);
    EPD_SendData(0xA5);
    EPD_3IN52_ReadBusy();
    OS_MSleep(200);
}

/* LUT download for GC mode */
static void EPD_3IN52_lut_GC(void)
{
    uint8_t count;
    
    EPD_SendCommand(0x20);  // vcom
    for (count = 0; count < 56; count++) {
        EPD_SendData(EPD_3IN52_lut_R20_GC[count]);
    }
    
    EPD_SendCommand(0x21);  // red not use
    for (count = 0; count < 42; count++) {
        EPD_SendData(EPD_3IN52_lut_R21_GC[count]);
    }
    
    EPD_SendCommand(0x24);  // bb b
    for (count = 0; count < 42; count++) {
        EPD_SendData(EPD_3IN52_lut_R24_GC[count]);
    }
    
    if (EPD_3IN52_Flag == 0) {
        EPD_SendCommand(0x22);  // bw r
        for (count = 0; count < 56; count++) {
            EPD_SendData(EPD_3IN52_lut_R22_GC[count]);
        }
        
        EPD_SendCommand(0x23);  // wb w
        for (count = 0; count < 42; count++) {
            EPD_SendData(EPD_3IN52_lut_R23_GC[count]);
        }
        
        EPD_3IN52_Flag = 1;
    } else {
        EPD_SendCommand(0x22);  // bw r
        for (count = 0; count < 56; count++) {
            EPD_SendData(EPD_3IN52_lut_R23_GC[count]);
        }
        
        EPD_SendCommand(0x23);  // wb w
        for (count = 0; count < 42; count++) {
            EPD_SendData(EPD_3IN52_lut_R22_GC[count]);
        }
        
        EPD_3IN52_Flag = 0;
    }
}

/* Clear screen */
void EPD_3IN52_Clear(void)
{
    EPD_SendCommand(0x13);
    
    // 240 x 415 = 12450 bytes
    for (int i = 0; i < 12450; i++) {
        EPD_SendData(0xFF);
    }
    
    EPD_3IN52_lut_GC();
    EPD_3IN52_refresh();

    EPD_SendCommand(0x50);
    EPD_SendData(0x17);

    OS_MSleep(500);
}

/* Check if EPD is responding */
static int EPD_CheckReady(void)
{
    uint8_t busy;
    int timeout = 0;
    
    // Wait for BUSY to go HIGH (ready)
    while (timeout < 1000) {
        busy = HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_8);  // PA8 = BUSY
        if (busy == GPIO_PIN_HIGH) {
            return 0;  // Ready
        }
        OS_MSleep(1);
        timeout++;
    }
    
    printf("[EPD] Device not ready (BUSY timeout)\n");
    return -1;  // Not ready
}

/* Initialize EPD */
int EPD_3IN52_Init(void)
{
    int ret;
    
    /* Initialize GPIO */
    EPD_GPIO_Init();
    
    EPD_3IN52_Flag = 0;
    EPD_Reset();

    EPD_SendCommand(0x00);  // panel setting PSR
    EPD_SendData(0xFF);
    EPD_SendData(0x01);

    EPD_SendCommand(0x01);  // POWER SETTING PWR
    EPD_SendData(0x03);
    EPD_SendData(0x10);
    EPD_SendData(0x3F);
    EPD_SendData(0x3F);
    EPD_SendData(0x03);

    EPD_SendCommand(0x06);  // booster soft start BTST
    EPD_SendData(0x37);
    EPD_SendData(0x3D);
    EPD_SendData(0x3D);

    EPD_SendCommand(0x60);  // TCON setting
    EPD_SendData(0x22);

    EPD_SendCommand(0x82);  // VCOM_DC setting VDCS
    EPD_SendData(0x07);

    EPD_SendCommand(0x30);
    EPD_SendData(0x09);

    EPD_SendCommand(0xe3);  // power saving PWS
    EPD_SendData(0x88);

    // Resolution: 240 x 415
    EPD_SendCommand(0x61);  // resoultion setting
    EPD_SendData(0xF0);     // HRES[7:0] = 240
    EPD_SendData(0x01);     // VRES[8]
    EPD_SendData(0x9F);     // VRES[7:0] = 415

    EPD_SendCommand(0x50);
    EPD_SendData(0xB7);

    // Check if EPD is ready before proceeding
    ret = EPD_CheckReady();
    if (ret != 0) {
        printf("[EPD] Init failed: EPD not responding!\n");
        return -1;  // Return error instead of continuing
    }

    // CRITICAL FIX: Add delay after EPD_CheckReady() returns
    // EPD needs time to stabilize after BUSY goes HIGH before we can safely send commands
    OS_MSleep(200);

    EPD_3IN52_Clear();

    EPD_SendCommand(0x13);  // DATA_START_TRANSMISSION_1
    OS_MSleep(2);
    
    return 0;
}

/* Show display (Deep sleep removed for LVGL compatibility) */
void EPD_3IN52_Show(void)
{
    EPD_3IN52_lut_GC();
    EPD_3IN52_refresh();
    OS_MSleep(2);
    printf("EPD_3IN52_Show END\r\n");
    
    // Deep sleep removed - do not sleep after display!
    // If sleep is needed, call EPD_3IN52_Sleep() explicitly
}

/* Display frame buffer */
void EPD_3IN52_Display(void)
{
    EPD_SendCommand(0x13);
    
    // Send framebuffer to EPD
    for (int i = 0; i < 240 * 415 / 8; i++) {
        EPD_SendData(framebuffer[i]);
    }
    
    EPD_3IN52_Show();
}

/* Fill frame buffer with pattern */
void EPD_FillBuffer(uint8_t pattern)
{
    memset(framebuffer, pattern, 240 * 415 / 8);
}

/* Set pixel in frame buffer */
void EPD_SetPixel(uint16_t x, uint16_t y, uint8_t color)
{
    if (x >= 240 || y >= 415) return;
    
    uint16_t byte_index = (y * 240 + x) / 8;
    uint8_t bit_index = 7 - (x % 8);
    
    if (color) {
        framebuffer[byte_index] |= (1 << bit_index);  // White
    } else {
        framebuffer[byte_index] &= ~(1 << bit_index);  // Black
    }
}

/* Clear frame buffer */
void EPD_ClearBuffer(void)
{
    memset(framebuffer, 0xFF, 240 * 415 / 8);
}

/* Display test pattern - black border frame */
void EPD_DisplayTestPattern(void)
{
    /* Clear to white */
    EPD_ClearBuffer();
    
    /* Draw black border */
    for (uint16_t x = 0; x < 240; x++) {
        EPD_SetPixel(x, 0, 0);           // Top border
        EPD_SetPixel(x, 1, 0);           // Top border 2
        EPD_SetPixel(x, 414, 0);         // Bottom border 2
    }
    
    for (uint16_t y = 0; y < 415; y++) {
        EPD_SetPixel(0, y, 0);            // Left border
        EPD_SetPixel(1, y, 0);            // Left border 2
        EPD_SetPixel(239, y, 0);          // Right border
        EPD_SetPixel(238, y, 0);          // Right border 2
    }
    
    /* Display the pattern */
    EPD_3IN52_Display();
}

/* Simple 16x16 bitmap font for "Hello,World!" - larger and clearer */
// 'H' - 16x16
/* Simple 16x16 bitmap font for "Hello,World!" */

// 'H' - 16x16
static const uint8_t font_H[32] = {
    0x00,0x00, 0x00,0x00, 0xC3,0x00, 0xC3,0x00, 0xC3,0x00, 0xC3,0x00, 0xC3,0x00, 0xFF,0x00,
    0xFF,0x00, 0xC3,0x00, 0xC3,0x00, 0xC3,0x00, 0xC3,0x00, 0xC3,0x00, 0x00,0x00, 0x00,0x00
};
// 'e' - 16x16
static const uint8_t font_e[32] = {
    0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x3C,0x00, 0x66,0x00, 0xC3,0x00, 0xFF,0x00,
    0xC0,0x00, 0xC0,0x00, 0xC0,0x00, 0x66,0x00, 0x3C,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00
};
// 'l' - 16x16
static const uint8_t font_l[32] = {
    0x00,0x00, 0x30,0x00, 0x30,0x00, 0x30,0x00, 0x30,0x00, 0x30,0x00, 0x30,0x00, 0x30,0x00,
    0x30,0x00, 0x30,0x00, 0x30,0x00, 0x30,0x00, 0x30,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00
};
// 'o' - 16x16
static const uint8_t font_o[32] = {
    0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x3C,0x00, 0x66,0x00, 0xC3,0x00, 0xC3,0x00,
    0xC3,0x00, 0xC3,0x00, 0xC3,0x00, 0x66,0x00, 0x3C,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00
};
// ',' (comma) - 16x16
static const uint8_t font_comma[32] = {
    0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00,
    0x00,0x00, 0x00,0x00, 0x00,0x00, 0x18,0x00, 0x18,0x00, 0x30,0x00, 0x00,0x00, 0x00,0x00
};
// 'W' - 16x16
static const uint8_t font_W[32] = {
    0x00,0x00, 0x00,0x00, 0xC3,0x00, 0xC3,0x00, 0xC3,0x00, 0xC3,0x00, 0xC3,0x00, 0xC3,0x00,
    0xDB,0x00, 0xDB,0x00, 0xFF,0x00, 0x66,0x00, 0x66,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00
};
// 'r' - 16x16
static const uint8_t font_r[32] = {
    0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0xDC,0x00, 0x66,0x00, 0x60,0x00, 0x60,0x00,
    0x60,0x00, 0x60,0x00, 0x60,0x00, 0x60,0x00, 0xF0,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00
};
// 'd' - 16x16
static const uint8_t font_d[32] = {
    0x00,0x00, 0x03,0x00, 0x03,0x00, 0x03,0x00, 0x3F,0x00, 0x63,0x00, 0xC3,0x00, 0xC3,0x00,
    0xC3,0x00, 0xC3,0x00, 0xC3,0x00, 0x63,0x00, 0x3F,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00
};
// '!' (exclamation) - 16x16
static const uint8_t font_excl[32] = {
    0x00,0x00, 0x18,0x00, 0x18,0x00, 0x18,0x00, 0x18,0x00, 0x18,0x00, 0x18,0x00, 0x18,0x00,
    0x18,0x00, 0x00,0x00, 0x18,0x00, 0x18,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00
};

/* Draw 16x16 character at position */
static void EPD_DrawChar16(uint16_t x, uint16_t y, const uint8_t* bitmap)
{
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 8; col++) {
            uint8_t byte = bitmap[row * 2 + col / 8];
            if (byte & (0x80 >> (col % 8))) {
                EPD_SetPixel(x + col, y + row, 0);  // Black pixel
            }
        }
        for (int col = 8; col < 16; col++) {
            uint8_t byte = bitmap[row * 2 + col / 8];
            if (byte & (0x80 >> (col % 8))) {
                EPD_SetPixel(x + col, y + row, 0);  // Black pixel
            }
        }
    }
}

/* Display Hello World on screen */
void EPD_DisplayHelloWorld(void)
{
    /* Clear to white */
    EPD_ClearBuffer();
    
    /* Draw black border frame for debugging */
    /* Top border */
    for (uint16_t x = 0; x < 240; x++) {
        EPD_SetPixel(x, 0, 0);
        EPD_SetPixel(x, 1, 0);
    }
    /* Bottom border */
    for (uint16_t x = 0; x < 240; x++) {
        EPD_SetPixel(x, 413, 0);
        EPD_SetPixel(x, 414, 0);
    }
    /* Left border */
    for (uint16_t y = 0; y < 415; y++) {
        EPD_SetPixel(0, y, 0);
        EPD_SetPixel(1, y, 0);
    }
    /* Right border */
    for (uint16_t y = 0; y < 415; y++) {
        EPD_SetPixel(238, y, 0);
        EPD_SetPixel(239, y, 0);
    }
    
    /* Draw "Hello,World!" starting at position (20, 150) - larger font */
    int x = 20;
    int y = 150;
    int charWidth = 16;
    int spacing = 2;
    
    /* H */
    EPD_DrawChar16(x, y, font_H);
    x += charWidth + spacing;
    
    /* e */
    EPD_DrawChar16(x, y, font_e);
    x += charWidth + spacing;
    
    /* l */
    EPD_DrawChar16(x, y, font_l);
    x += charWidth + spacing;
    
    /* l */
    EPD_DrawChar16(x, y, font_l);
    x += charWidth + spacing;
    
    /* o */
    EPD_DrawChar16(x, y, font_o);
    x += charWidth + spacing;
    
    /* , */
    EPD_DrawChar16(x, y, font_comma);
    x += charWidth + spacing;
    
    /* W */
    EPD_DrawChar16(x, y, font_W);
    x += charWidth + spacing;
    
    /* o */
    EPD_DrawChar16(x, y, font_o);
    x += charWidth + spacing;
    
    /* r */
    EPD_DrawChar16(x, y, font_r);
    x += charWidth + spacing;
    
    /* l */
    EPD_DrawChar16(x, y, font_l);
    x += charWidth + spacing;
    
    /* d */
    EPD_DrawChar16(x, y, font_d);
    x += charWidth + spacing;
    
    /* ! */
    EPD_DrawChar16(x, y, font_excl);
    
    /* Display the result */
    EPD_3IN52_Display();
}

/* DU refresh LUT download - for fast update mode (0.3s refresh) */
static void EPD_3IN52_lut_DU(void)
{
    uint8_t count;
    
    EPD_SendCommand(0x20);  // vcom
    for (count = 0; count < 56; count++) {
        EPD_SendData(EPD_3IN52_lut_R20_DU[count]);
    }
    
    EPD_SendCommand(0x21);  // red not use
    for (count = 0; count < 42; count++) {
        EPD_SendData(EPD_3IN52_lut_R21_DU[count]);
    }
    
    EPD_SendCommand(0x24);  // bb b
    for (count = 0; count < 42; count++) {
        EPD_SendData(EPD_3IN52_lut_R24_DU[count]);
    }
    
    /* ================= 修改这里 ================= */
    /* 删掉原本的 if (EPD_3IN52_Flag == 0) else 判断
       直接固定写入正常的 LUT 表，防止动画时出现反色闪烁 */
       
    EPD_SendCommand(0x22);  // bw r
    for (count = 0; count < 56; count++) {
        EPD_SendData(EPD_3IN52_lut_R22_DU[count]);
    }
    
    EPD_SendCommand(0x23);  // wb w
    for (count = 0; count < 42; count++) {
        EPD_SendData(EPD_3IN52_lut_R23_DU[count]);
    }
    /* =========================================== */
}

/* Initialize EPD for DU (fast) refresh mode */
void EPD_3IN52_Init_DU(void)
{
    EPD_3IN52_Flag = 0;
    EPD_Reset();
    
    int timeout = 0;
    while (timeout < 3000) {
        if (HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_8) == GPIO_PIN_HIGH) break;
        OS_MSleep(1);
        timeout++;
    }
    if (timeout >= 3000) {
        printf("[EPD] Init_DU: busy timeout after reset, retrying reset\n");
        EPD_Reset();
        timeout = 0;
        while (timeout < 3000) {
            if (HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_8) == GPIO_PIN_HIGH) break;
            OS_MSleep(1);
            timeout++;
        }
        if (timeout >= 3000) {
            printf("[EPD] Init_DU: still busy, proceeding anyway\n");
        }
    }
    
    EPD_SendCommand(0x00);  // panel setting PSR
    EPD_SendData(0xFF);
    EPD_SendData(0x01);
    
    EPD_SendCommand(0x01);  // POWER SETTING PWR
    EPD_SendData(0x03);
    EPD_SendData(0x10);
    EPD_SendData(0x3F);
    EPD_SendData(0x3F);
    EPD_SendData(0x03);
    
    EPD_SendCommand(0x06);  // booster soft start BTST
    EPD_SendData(0x37);
    EPD_SendData(0x3D);
    EPD_SendData(0x3D);
    
    EPD_SendCommand(0x60);  // TCON setting
    EPD_SendData(0x22);
    
    EPD_SendCommand(0x82);  // VCOM_DC setting VDCS
    EPD_SendData(0x07);
    
    EPD_SendCommand(0x30);
    EPD_SendData(0x09);
    
    EPD_SendCommand(0xe3);  // power saving PWS
    EPD_SendData(0x88);
    
    // Resolution: 240 x 415
    EPD_SendCommand(0x61);  // resolution setting
    EPD_SendData(0xF0);     // HRES[7:0] = 240
    EPD_SendData(0x01);     // VRES[8]
    EPD_SendData(0x9F);     // VRES[7:0] = 415
    
    EPD_SendCommand(0x50);
    EPD_SendData(0xB7);
    
    EPD_3IN52_ReadBusy();
    printf("[EPD] DU mode initialized\r\n");
}

/* Display using DU (fast) refresh - for partial/full screen updates */
void EPD_3IN52_Display_DU(void)
{
    /* Wait for EPD to be ready (BUSY high) before sending new data */
    int timeout = 0;
    while (timeout < 3000) {
        if (HAL_GPIO_ReadPin(GPIO_PORT_A, GPIO_PIN_8) == GPIO_PIN_HIGH) {
            break;
        }
        OS_MSleep(1);
        timeout++;
    }
    if (timeout >= 3000) {
        printf("[EPD] EPD_3IN52_Display_DU: busy wait timeout\r\n");
        return;
    }
    
    /* Re-initialize panel for DU mode - EPD needs this after GC refresh */
    EPD_SendCommand(0x00);  // panel setting PSR
    EPD_SendData(0xFF);
    EPD_SendData(0x01);
    
    EPD_SendCommand(0x01);  // POWER SETTING PWR
    EPD_SendData(0x03);
    EPD_SendData(0x10);
    EPD_SendData(0x3F);
    EPD_SendData(0x3F);
    EPD_SendData(0x03);
    
    EPD_SendCommand(0x06);  // booster soft start BTST
    EPD_SendData(0x37);
    EPD_SendData(0x3D);
    EPD_SendData(0x3D);
    
    EPD_SendCommand(0x60);  // TCON setting
    EPD_SendData(0x22);
    
    EPD_SendCommand(0x82);  // VCOM_DC setting VDCS
    EPD_SendData(0x07);
    
    EPD_SendCommand(0x30);  // PLL setting
    EPD_SendData(0x09);
    
    EPD_SendCommand(0xe3);  // power saving PWS
    EPD_SendData(0x88);
    
    EPD_SendCommand(0x50);  // VCOM setting
    EPD_SendData(0xB7);
    
    /* Send framebuffer data to EPD */
    EPD_SendCommand(0x13);
    for (int i = 0; i < 240 * 415 / 8; i++) {
        EPD_SendData(framebuffer[i]);
    }
    
    EPD_3IN52_lut_DU();
    EPD_3IN52_refresh();
    OS_MSleep(2);
    printf("EPD_3IN52_Display_DU END\r\n");
    
    /* Note: Deep sleep removed - use EPD_Sleep() separately when needed */
}

/* Put EPD to deep sleep */
void EPD_Sleep(void)
{
    EPD_SendCommand(0X07);  // deep sleep
    EPD_SendData(0xA5);
    printf("[EPD] Entered Deep Sleep\n");
}

/* Display Hello World on lower half of screen using fast (DU) refresh */
void EPD_DisplayHelloWorld_DU(void)
{
    /* Clear to white */
    EPD_ClearBuffer();
    
    /* Draw "Hello,World!" starting at position (20, 280) - lower half */
    int x = 20;
    int y = 280;  /* Lower half of screen */
    int charWidth = 16;
    int spacing = 2;
    
    /* H */
    EPD_DrawChar16(x, y, font_H);
    x += charWidth + spacing;
    
    /* e */
    EPD_DrawChar16(x, y, font_e);
    x += charWidth + spacing;
    
    /* l */
    EPD_DrawChar16(x, y, font_l);
    x += charWidth + spacing;
    
    /* l */
    EPD_DrawChar16(x, y, font_l);
    x += charWidth + spacing;
    
    /* o */
    EPD_DrawChar16(x, y, font_o);
    x += charWidth + spacing;
    
    /* , */
    EPD_DrawChar16(x, y, font_comma);
    x += charWidth + spacing;
    
    /* W */
    EPD_DrawChar16(x, y, font_W);
    x += charWidth + spacing;
    
    /* o */
    EPD_DrawChar16(x, y, font_o);
    x += charWidth + spacing;
    
    /* r */
    EPD_DrawChar16(x, y, font_r);
    x += charWidth + spacing;
    
    /* l */
    EPD_DrawChar16(x, y, font_l);
    x += charWidth + spacing;
    
    /* d */
    EPD_DrawChar16(x, y, font_d);
    x += charWidth + spacing;
    
    /* ! */
    EPD_DrawChar16(x, y, font_excl);
    
    /* Display using DU fast refresh */
    EPD_3IN52_Display_DU();
}

/* ========================================================= */
/* DU 快刷动画演示代码                     */
/* ========================================================= */

/* 基础图形接口：Bresenham 画线算法 */
void EPD_DrawLine(int x0, int y0, int x1, int y1) 
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = x0 < x1 ? 1 : -1;
    int dy = (y1 > y0) ? -(y1 - y0) : -(y0 - y1);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;

    while (1) {
        EPD_SetPixel(x0, y0, 0); // 0为黑色
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Demo 1: 螺旋渐进绘制动画 (模拟画笔轨迹) */
void EPD_Demo_DrawingAnimation(void)
{
    /* 1. 先使用普通(GC)模式清空屏幕，保证有一张干净无残影的"白纸" */
    printf("[Demo] Clearing screen for drawing...\n");
    EPD_3IN52_Init();
    EPD_ClearBuffer();
    EPD_3IN52_Display(); // 全局刷新，会有闪烁

    /* 2. 唤醒并进入快刷(DU)模式 */
    EPD_3IN52_Init_DU();

    /* 画一个不断向外扩展的方形螺旋折线 */
    int cx = 120; // 屏幕X中心附近
    int cy = 200; // 屏幕Y中心附近
    int step = 10;
    int sign = 1;

    printf("[Demo] Start drawing spiral animation...\n");
    for (int i = 0; i < 10; i++) {
        // 增量画一段横线，并立刻快刷
        int next_x = cx + sign * step;
        EPD_DrawLine(cx, cy, next_x, cy);
        EPD_3IN52_Display_DU();
        cx = next_x;

        // 增量画一段竖线，并立刻快刷
        int next_y = cy + sign * step;
        EPD_DrawLine(cx, cy, cx, next_y);
        EPD_3IN52_Display_DU();
        cy = next_y;

        step += 12;   // 边长递增
        sign = -sign; // 变向 (一左一右，一上一下)
    }
    
    /* 动画全部播放完毕后，再让屏幕休眠 */
    EPD_Sleep();
}

/* Demo 2: 打字机动画效果 */
void EPD_Demo_TypingEffect(void)
{
    /* 1. 同样，先备好干净的白屏 */
    EPD_3IN52_Init();
    EPD_ClearBuffer();
    EPD_3IN52_Display();

    /* 2. 进入快刷(DU)模式 */
    EPD_3IN52_Init_DU();

    int x = 20;
    int y = 50;
    int charWidth = 16;
    int spacing = 2;

    /* 将你前面定义的字体打包成数组方便循环遍历 */
    const uint8_t* text_fonts[] = {
        font_H, font_e, font_l, font_l, font_o, font_comma, 
        font_W, font_o, font_r, font_l, font_d, font_excl
    };
    int num_chars = 12;

    printf("[Demo] Start typing text animation...\n");
    /* 逐个字符绘制，每画一个字就快刷一次屏幕 */
    for(int i = 0; i < num_chars; i++) {
        EPD_DrawChar16(x, y, text_fonts[i]);
        EPD_3IN52_Display_DU(); // 快刷！不会黑白闪屏
        x += charWidth + spacing;
    }
    
    /* 动画全部播放完毕后，再让屏幕休眠 */
    EPD_Sleep();
}
