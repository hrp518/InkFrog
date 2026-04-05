/*
 * CHSC6540 Touch Panel Driver
 *
 * Connection:
 *   PA04: RST (复位输出，推挽，默认拉高)
 *   PA05: INT (中断输入，上拉)
 *   PA20: I2C SDA
 *   PA19: I2C SCL
 *
 * I2C Address: 0x2E (7-bit)
 */

#ifndef _CHSC6540_H_
#define _CHSC6540_H_

#include <stdint.h>

/* CHSC6540 I2C Address */
#define CHSC6540_ADDR           0x2E

/* CHSC6540 Device ID */
#define CHSC6540_ID             0x02

/* Touch event flags */
#define CHSC6540_TOUCH_EVT_FLAG_PRESS_DOWN  0x20
#define CHSC6540_TOUCH_EVT_FLAG_LIFT_UP    0x60
#define CHSC6540_TOUCH_EVT_FLAG_CONTACT    0x80
#define CHSC6540_TOUCH_EVT_FLAG_NO_EVENT   0x00

/* Point registers */
#define CHSC6540_P1_XH_REG      0x09
#define CHSC6540_P1_XL_REG      0x0A
#define CHSC6540_P1_YH_REG      0x0B
#define CHSC6540_P1_YL_REG      0x0C

/* Max detectable touches */
#define CHSC6540_MAX_DETECTABLE_TOUCH  10

/* Display resolution */
#define CHSC6540_MAX_X          240
#define CHSC6540_MAX_Y          415

/**
 * @brief Initialize CHSC6540 touch panel
 * @return 0 on success, -1 on error
 */
int CHSC6540_Init(void);

/**
 * @brief Read touch data
 * @param x Pointer to store X coordinate
 * @param y Pointer to store Y coordinate
 * @return Number of touches detected, -1 on error
 */
int CHSC6540_ReadTouchData(uint16_t *x, uint16_t *y);

/**
 * @brief Scan for CHSC6540 device at address 0x2E
 * @return 1 if device responds with ACK, 0 if not found
 */
int CHSC6540_ScanDevice(void);

/**
 * @brief Deinitialize CHSC6540
 */
void CHSC6540_DeInit(void);

#endif /* _CHSC6540_H_ */