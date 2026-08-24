/**
 * @file lv_port_indev.c
 * LVGL输入端口适配 - CHSC6540触摸驱动
 *
 * 特性:
 * - 硬件I2C通信
 * - 轮询模式（Polling）- PA05作为普通输入引脚
 * - 依赖INT pin电平决定何时读取数据
 * - 状态以CHSC6540报告的num为准
 */

#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"
#include "chsc6540.h"
#include "driver/chip/hal_gpio.h"
#include "lv_port_indev.h"
#include "screensaver.h"

// 前向声明
extern void epd_notify_touch_down(void);
extern void epd_notify_touch_up(void);

/*====================
 * 触摸配置
 *===================*/

// 触摸IC的INT引脚连接到PA05
#define TOUCH_INT_PIN   GPIO_PIN_5
#define TOUCH_INT_PORT  GPIO_PORT_A

/*====================
 * 静态变量
 *===================*/

static lv_indev_t *indev;
static int16_t last_x = 0;
static int16_t last_y = 0;

// 记录当前物理触摸状态，供外部查询
static TouchState_t current_touch_state = TOUCH_STATE_IDLE;

// 标志位：是否强制每次都查询CHSC（不依赖INT）
// PRESSED时强制查询确保不漏报，RELEASED后依赖INT触发
static uint8_t force_query_chsc = 0;

// 心跳：每 N 次 poll 打印一次，验证 lvgl_task 仍在调度 touch_read_cb
// touch_read_cb 在 lvgl_task 中被 lv_timer_handler 周期性调用,
// 若 LVGL 线程卡死, 心跳停止, 借此快速定位死锁
static uint32_t s_touch_poll_count = 0;
#define TOUCH_HEARTBEAT_INTERVAL  500   /* 约 2.5s @ 5ms period */

// 返回按键相关
static uint8_t back_btn_pressed = 0;  // 返回按键按下标志
static void (*back_btn_callback)(void) = NULL;  // 返回按键回调函数

// 滑动检测相关
static int32_t swipe_press_x = 0;    // 触摸按下的X坐标
static int32_t swipe_press_y = 0;    // 触摸按下的Y坐标
static int32_t swipe_delta_x = 0;     // 触摸释放时的横向滑动距离 (press_x - release_x, 左滑>0)
static int32_t swipe_delta_y = 0;     // 触摸释放时的纵向滑动距离
static void (*swipe_callback)(int32_t delta_x, int32_t delta_y) = NULL;  // 滑动回调函数
static uint8_t swipe_recorded = 0;    // 是否已记录起始位置

// 滑动区域边界（由UI层设置）
static int16_t swipe_area_y1 = 0;     // 滑动区域上边界
static int16_t swipe_area_y2 = 0;     // 滑动区域下边界（0表示未设置）
static uint8_t swipe_area_enabled = 0; // 是否启用区域限制

/*====================
 * 触摸引脚初始化 (普通输入模式)
 *===================*/

static void touch_pin_init(void)
{
    GPIO_InitParam param;
    
    // 将 PA05 配置为普通输入模式，带上拉
    param.mode = GPIOx_Pn_F0_INPUT;
    param.pull = GPIO_PULL_UP;
    param.driving = GPIO_DRIVING_LEVEL_1;
    HAL_GPIO_Init(TOUCH_INT_PORT, TOUCH_INT_PIN, &param);
    
    printf("[Touch] PA05 configured as input (Polling mode)\r\n");
}

/*====================
 * 导出函数：获取当前触摸状态
 *===================*/

TouchState_t touch_get_state(void) {
    return current_touch_state;
}

/*====================
 * 导出函数：注册返回按键回调
 *===================*/

void touch_register_back_btn_callback(void (*callback)(void)) {
    back_btn_callback = callback;
}

/*====================
 * 导出函数：检查返回按键是否被按下（抬手触发）
 * 返回1表示需要处理返回，0表示无
 *===================*/

uint8_t touch_check_back_btn(void) {
    if (back_btn_pressed && back_btn_callback != NULL) {
        back_btn_pressed = 0;  // 清除标志
        return 1;
    }
    return 0;
}

/*====================
 * 导出函数：注册滑动回调（delta_y: 负数=上滑, 正数=下滑）
 *===================*/

void touch_register_swipe_callback(void (*callback)(int32_t delta_x, int32_t delta_y)) {
    swipe_callback = callback;
}

/*====================
 * 导出函数：获取并清除累积的滑动距离
 * 调用后清除swipe_delta_y，只能调用一次
 *===================*/

int32_t touch_get_and_clear_swipe_delta(void) {
    int32_t delta = swipe_delta_y;
    swipe_delta_y = 0;
    return delta;
}

/*====================
 * 导出函数：设置滑动区域边界
 * 只有触摸起始位置在此区域内才检测滑动
 * @param y1 区域上边界（像素）
 * @param y2 区域下边界（像素）
 *===================*/

void touch_set_swipe_area(int16_t y1, int16_t y2) {
    swipe_area_y1 = y1;
    swipe_area_y2 = y2;
    swipe_area_enabled = (y2 > y1) ? 1 : 0;
    // 频繁调用，不打印避免日志泛滥
}

/*====================
 * 导出函数：清空滑动回调
 *===================*/

void touch_clear_swipe_callback(void) {
    swipe_callback = NULL;
    swipe_delta_y = 0;
    swipe_recorded = 0;
    printf("[TOUCH] Swipe callback cleared\n");
}

/*====================
 * LVGL输入设备读取回调
 * 依赖INT pin电平决定何时读取数据
 *===================*/

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    GPIO_PinState int_state = HAL_GPIO_ReadPin(TOUCH_INT_PORT, TOUCH_INT_PIN);

    /* 心跳: 验证 lvgl_task 仍在调度 touch_read_cb.
     * 若日志中此消息停止, 意味着 LVGL 线程卡死. */
    s_touch_poll_count++;
    if ((s_touch_poll_count % TOUCH_HEARTBEAT_INTERVAL) == 0) {
        printf("[TOUCH] poll heartbeat #%u (INT=%d, force=%d)\n",
               s_touch_poll_count, (int)int_state, force_query_chsc);
    }

    // 两种情况需要查询CHSC：
    // 1. force_query_chsc置位（PRESSED状态时强制每次都查，确保能收到RELEASE）
    // 2. INT低电平触发（手指按下时中断必然产生）
    if (force_query_chsc || (int_state == GPIO_PIN_LOW)) {
        uint16_t x = 0, y = 0;
        int touch_cnt = CHSC6540_ReadTouchData(&x, &y);
        
        if (touch_cnt == -2) {
            // 返回按键按下 - 设置标志等待released信号
            back_btn_pressed = 1;
            // 保持之前的状态不变
            data->point.x = last_x;
            data->point.y = last_y;
            data->state = LV_INDEV_STATE_RELEASED;
            printf("[TOUCH] Back button pressed\n");
        } else if (touch_cnt == -3) {
            // 返回按键released - 直接触发回调（无需再等抬手）
            if (back_btn_pressed && back_btn_callback != NULL) {
                back_btn_callback();
            }
            back_btn_pressed = 0;
            data->point.x = last_x;
            data->point.y = last_y;
            data->state = LV_INDEV_STATE_RELEASED;
            printf("[TOUCH] Back button released\n");
        } else if (touch_cnt > 0) {
            // 有触摸数据 - 触摸按下
            last_x = (int16_t)x;
            last_y = (int16_t)y;
            data->point.x = last_x;
            data->point.y = last_y;
            data->state = LV_INDEV_STATE_PRESSED;
            current_touch_state = TOUCH_STATE_PRESSED;
            force_query_chsc = 1; // 保持强制查询模式

            if (screensaver_handle_touch(current_touch_state)) {
                data->state = LV_INDEV_STATE_RELEASED;
                return;
            }
            
            // 滑动检测：只在第一次按下时记录起始X/Y
            if (!swipe_recorded) {
                swipe_press_x = (int32_t)x;
                swipe_press_y = (int32_t)y;
                swipe_recorded = 1;
                printf("[TOUCH] Touch pressed at (%d,%d), swipe start X=%d Y=%d\n", x, y, swipe_press_x, swipe_press_y);
            }
            
            // 【EPD协同】通知EPD触摸按下（会阻断刷新直到释放）
            epd_notify_touch_down();
        } else {
            // 无触摸数据 = 手指离开
            // 如果之前返回按键被按下，现在抬手了，触发回调
            if (back_btn_pressed && back_btn_callback != NULL) {
                back_btn_callback();
            }
            back_btn_pressed = 0;
            
            // 滑动检测：计算滑动距离并通知回调（仅在触摸区域内）
            if (swipe_area_enabled && swipe_area_y2 > 0) {
                if (swipe_press_y >= swipe_area_y1 && swipe_press_y <= swipe_area_y2) {
                    swipe_delta_x = swipe_press_x - (int32_t)last_x;
                    swipe_delta_y = swipe_press_y - (int32_t)last_y;
                } else {
                    swipe_delta_x = 0;
                    swipe_delta_y = 0;
                }
            } else {
                swipe_delta_x = swipe_press_x - (int32_t)last_x;
                swipe_delta_y = swipe_press_y - (int32_t)last_y;
            }
            printf("[TOUCH] Touch released, press=(%d,%d) release=(%d,%d), swipe_dx=%d swipe_dy=%d\n",
                   swipe_press_x, swipe_press_y, last_x, last_y, swipe_delta_x, swipe_delta_y);
            if (swipe_callback != NULL && (swipe_delta_x != 0 || swipe_delta_y != 0)) {
                printf("[TOUCH] Calling swipe callback dx=%d dy=%d\n", swipe_delta_x, swipe_delta_y);
                swipe_callback(swipe_delta_x, swipe_delta_y);
            }
            swipe_recorded = 0;  // 重置滑动记录
            
            data->point.x = last_x;
            data->point.y = last_y;
            data->state = LV_INDEV_STATE_RELEASED;
            current_touch_state = TOUCH_STATE_RELEASED;
            force_query_chsc = 0; // 退出强制查询，依赖INT触发

            if (screensaver_handle_touch(current_touch_state)) {
                data->state = LV_INDEV_STATE_RELEASED;
                return;
            }
            
            // 【EPD协同】通知EPD触摸释放（会触发待处理的刷新）
            epd_notify_touch_up();
        }
    } else {
        // INT高电平且非强制查询模式，保持之前的状态
        data->point.x = last_x;
        data->point.y = last_y;
        data->state = (current_touch_state == TOUCH_STATE_PRESSED) ?
                      LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    }
}

/*====================
 * 输入设备初始化
 *===================*/

void lv_port_indev_init(void)
{
    // 步骤1: 初始化 INT 引脚为普通输入模式
    touch_pin_init();
    
    // 步骤2: 初始化 CHSC6540 触摸 IC
    if (CHSC6540_Init() != 0) {
        printf("[Touch] CHSC6540 init failed!\r\n");
        return;
    }
    
    // 步骤3: 注册给 LVGL
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    
    indev = lv_indev_drv_register(&indev_drv);
    if (indev == NULL) {
        printf("[Touch] Failed to register indev!\r\n");
        return;
    }
    
    printf("[Touch] Input device initialized: CHSC6540 (Polling mode)\r\n");
}
