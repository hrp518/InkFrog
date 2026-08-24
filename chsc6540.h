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

/*
 * ==================== 触摸坐标仿射校准 ====================
 * 面板原始坐标范围 > 屏幕分辨率, 驱动不做映射直接透传给 LVGL
 * 会导致"按 G 出 V"这类错位, 且底部/右侧误差随坐标增大。
 * 校准映射:  display = raw * num / den + off   (每个轴独立)
 * 默认恒等 (num=den=1000, off=0), 不校准时行为不变。
 * =======================================================
 */

/**
 * @brief 设置触摸校准参数 (每轴: display = raw * num / den + off)
 * @param sx_num,sx_den,sx_off  X 轴比例分子/分母/偏移
 * @param sy_num,sy_den,sy_off  Y 轴比例分子/分母/偏移
 */
void CHSC6540_SetCalibration(int32_t sx_num, int32_t sx_den, int32_t sx_off,
                             int32_t sy_num, int32_t sy_den, int32_t sy_off);

/**
 * @brief 重置为恒等映射 (关闭校准)
 */
void CHSC6540_ResetCalibration(void);

/**
 * @brief 获取最近一次成功读取的原始(未校准)坐标
 * @param x Pointer to store raw X
 * @param y Pointer to store raw Y
 */
void CHSC6540_GetLastRaw(uint16_t *x, uint16_t *y);

#endif /* _CHSC6540_H_ */