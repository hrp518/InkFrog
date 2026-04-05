/*
 * CHSC6540 Touch Panel Driver
 * 
 * Using Hardware I2C (I2C0) with polling
 * 
 * Hardware Configuration:
 *   I2C0: PA19=SCL, PA20=SDA (auto-configured by HAL_I2C_Init)
 *   RST:  PA04 (GPIO output)
 *   INT:  PA05 (GPIO input with interrupt)
 * 
 * I2C Protocol:
 * 1. Start condition
 * 2. Send device address (0x2E << 1) = 0x5C for write
 * 3. Send 4-byte command: 0x20, 0x00, 0x00, 0x2C
 * 4. RESTART condition
 * 5. Send device address (0x2E << 1 | 0x01) = 0x5D for read
 * 6. Read 28 bytes
 * 7. Stop condition
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "driver/chip/hal_gpio.h"
#include "driver/chip/hal_i2c.h"
#include "kernel/os/os.h"
#include "chsc6540.h"

/* Hardware I2C Configuration */
#define HW_I2C_ID              I2C0_ID
#define HW_I2C_ADDR           CHSC6540_ADDR

/* GPIO Configuration */
#define RST_PORT               GPIO_PORT_A
#define RST_PIN                GPIO_PIN_4
#define INT_PORT               GPIO_PORT_A
#define INT_PIN                GPIO_PIN_5

/* Touch data command */
#define CHSC6540_CMD_LEN       4
static uint8_t chsc6540_cmd[CHSC6540_CMD_LEN] = {0x20, 0x00, 0x00, 0x2C};

/* I2C receive buffer */
static uint8_t g_rx_data[28];
static uint8_t g_tx_data[CHSC6540_CMD_LEN];

/* Initialization flag */
static uint8_t g_initialized = 0;

/**
 * @brief Initialize GPIO for CHSC6540 (RST and INT)
 */
static void CHSC6540_GPIO_Init(void)
{
    GPIO_InitParam param;
    
    /* Configure PA04 as RST output (push-pull), default HIGH */
    param.mode = GPIOx_Pn_F1_OUTPUT;
    param.pull = GPIO_PULL_UP;
    param.driving = GPIO_DRIVING_LEVEL_1;
    HAL_GPIO_Init(RST_PORT, RST_PIN, &param);
    HAL_GPIO_WritePin(RST_PORT, RST_PIN, GPIO_PIN_HIGH);  /* Default HIGH */
    
    /* Note: PA05 INT pin is configured in lv_port_indev.c touch_interrupt_init()
     * as GPIOx_Pn_F6_EINT mode for XR872. Do NOT configure it here as F0_INPUT
     * because it would override the EINT configuration.
     */
    
    printf("[CHSC6540] GPIO initialized (PA04=RST)\n");
}

/**
 * @brief Read data from CHSC6540
 * @param data Buffer to store data
 * @param len Number of bytes to read
 * @return 0 on success, -1 on error
 */
static int CHSC6540_ReadData(uint8_t *data, uint16_t len)
{
    int32_t ret;
    
    /* Copy command to TX buffer */
    memcpy(g_tx_data, chsc6540_cmd, CHSC6540_CMD_LEN);
    
    /* Step 1: Send device address (write) + 4 bytes command */
    ret = HAL_I2C_Master_Transmit_IT(HW_I2C_ID, (uint16_t)HW_I2C_ADDR, g_tx_data, CHSC6540_CMD_LEN);
    if (ret != CHSC6540_CMD_LEN) {
        printf("[CHSC6540] I2C transmit failed: %d\n", ret);
        return -1;
    }
    
    /* Wait for transmission to complete */
    OS_MSleep(20);
    
    /* Step 2: Send device address (read) and receive data */
    ret = HAL_I2C_Master_Receive_IT(HW_I2C_ID, (uint16_t)HW_I2C_ADDR, data, len);
    if (ret != len) {
        printf("[CHSC6540] I2C receive failed: %d\n", ret);
        return -1;
    }
    
    /* Wait for reception to complete */
    OS_MSleep(20);
    
    return 0;
}

/**
 * @brief Initialize CHSC6540 touch panel
 */
int CHSC6540_Init(void)
{
    uint8_t trial;
    int ret;
    I2C_InitParam i2c_param;
    
    printf("[CHSC6540] Initializing CHSC6540 (Hardware I2C)...\n");
    
    /* Initialize GPIO (RST=PA04 HIGH, INT=PA05 input) */
    CHSC6540_GPIO_Init();
    
    /* Initialize Hardware I2C controller - this also configures I2C pins */
    i2c_param.addrMode = I2C_ADDR_MODE_7BIT;
    i2c_param.clockFreq = 100000;  /* 100kHz standard mode */
    
    if (HAL_I2C_Init(HW_I2C_ID, &i2c_param) != 0) {
        printf("[CHSC6540] HAL_I2C_Init failed!\n");
        return -1;
    }
    printf("[CHSC6540] Hardware I2C initialized (100kHz)\n");
    
    /* Wait for chip to be ready */
    OS_MSleep(50);
    
    /* Reset sequence */
    printf("[CHSC6540] Resetting CHSC6540...\n");
    HAL_GPIO_WritePin(RST_PORT, RST_PIN, GPIO_PIN_LOW);  /* RST low */
    OS_MSleep(10);  /* 10ms */
    HAL_GPIO_WritePin(RST_PORT, RST_PIN, GPIO_PIN_HIGH); /* RST high */
    OS_MSleep(25);  /* Wait for chip to recover */
    
    /* Try to read touch data to verify communication */
    for (trial = 0; trial < 5; trial++) {
        ret = CHSC6540_ReadData(g_rx_data, 28);
        if (ret == 0) {
            printf("[CHSC6540] Read successful, data[0]=0x%02X\n", g_rx_data[0]);
            if (g_rx_data[0] != 0xFF || g_rx_data[1] != 0xFF) {
                printf("[CHSC6540] CHSC6540 found at 0x2E!\n");
                g_initialized = 1;
                return 0;
            }
        }
        OS_MSleep(20);
    }
    
    printf("[CHSC6540] Warning: Could not verify CHSC6540 communication\n");
    g_initialized = 1;  /* Mark as initialized anyway for polling */
    return 0;
}

/**
 * @brief Read touch data from CHSC6540
 * @param x Pointer to store X coordinate
 * @param y Pointer to store Y coordinate
 * @return Number of touches detected, -1 on error
 */
int CHSC6540_ReadTouchData(uint16_t *x, uint16_t *y)
{
    uint8_t num_touches;
    
    if (!g_initialized) {
        return -1;
    }
    
    /* Read 28 bytes from touch controller */
    int ret = CHSC6540_ReadData(g_rx_data, 28);
    if (ret != 0) {
        return -1;
    }
    
    /* Print all 28 bytes for debugging */
    printf("[CHSC6540] Raw 28 bytes: ");
    for (int i = 0; i < 28; i++) {
        printf("%02X ", g_rx_data[i]);
    }
    printf("\n");
    
    /* Parse touch data according to CHSC5448A protocol */
    num_touches = g_rx_data[1] & 0x0f;
    
    /* Detect Back Button: FF 01 64 28 = pressed, FF 00 64 28 = released */
    if (g_rx_data[0] == 0xFF && g_rx_data[2] == 0x64 && g_rx_data[3] == 0x28) {
        if (g_rx_data[1] == 0x01) {
            printf("[BACK BUTTON] Back Button Pressed!\n");
            *x = 0;
            *y = 0;
            return -2;  /* Special return for Back Button Pressed */
        } else if (g_rx_data[1] == 0x00) {
            printf("[BACK BUTTON] Back Button Released!\n");
            *x = 0;
            *y = 0;
            return -3;  /* Special return for Back Button Released */
        }
    }
    
    printf("[CHSC6540] Num touch points: %d\n", num_touches);
    
    if (num_touches == 0 || num_touches > 10) {
        *x = 0;
        *y = 0;
        return 0;  /* No touch */
    }
    
    /* Parse first touch point (at data[2..6]) */
    /* X axis: directly use data[2] */
    *x = g_rx_data[2];
    
    /* Y axis: data[5] high 4 bits as high byte, data[3] as low byte */
    *y = ((uint16_t)(g_rx_data[5] & 0xF0) << 4) | (uint16_t)g_rx_data[3];
    
    /* Clamp to screen bounds */
    if (*x > CHSC6540_MAX_X) *x = CHSC6540_MAX_X;
    if (*y > CHSC6540_MAX_Y) *y = CHSC6540_MAX_Y;
    
    printf("[CHSC6540] Touch: X=%d, Y=%d, num=%d\n", *x, *y, num_touches);
    
    return num_touches;
}

/**
 * @brief Scan for CHSC6540 device at address 0x2E
 * @return 1 if device responds, 0 if not found
 */
int CHSC6540_ScanDevice(void)
{
    int ret;
    
    printf("[CHSC6540] ScanDevice: Reading data...\n");
    
    ret = CHSC6540_ReadData(g_rx_data, 28);
    if (ret != 0) {
        printf("[CHSC6540] ScanDevice: Read failed\n");
        return 0;
    }
    
    if (g_rx_data[0] != 0xFF || g_rx_data[1] != 0xFF) {
        printf("[CHSC6540] ScanDevice: Device found, ID=0x%02X\n", g_rx_data[0]);
        return 1;
    }
    
    return 0;
}

/**
 * @brief Deinitialize CHSC6540
 */
void CHSC6540_DeInit(void)
{
    g_initialized = 0;
    HAL_I2C_DeInit(HW_I2C_ID);
    HAL_GPIO_DeInit(RST_PORT, RST_PIN);
    HAL_GPIO_DeInit(INT_PORT, INT_PIN);
    printf("[CHSC6540] Deinitialized\n");
}
