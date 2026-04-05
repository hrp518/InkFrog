/**
 * @file lv_port_indev.h
 * LVGL输入端口适配头文件
 */

#ifndef LV_PORT_INDEV_H
#define LV_PORT_INDEV_H

#include <stdint.h>

/*====================
 * 触摸状态枚举
 *===================*/

typedef enum {
    TOUCH_STATE_IDLE = 0,
    TOUCH_STATE_PRESSED,
    TOUCH_STATE_RELEASED
} TouchState_t;

/*====================
 * 函数声明
 *===================*/

/**
 * 初始化输入设备
 */
void lv_port_indev_init(void);

/**
 * 获取当前触摸状态
 */
TouchState_t touch_get_state(void);

/**
 * 注册返回按键回调函数
 */
void touch_register_back_btn_callback(void (*callback)(void));

/**
 * 检查返回按键是否被按下（抬手触发）
 * 返回1表示需要处理返回，0表示无
 */
uint8_t touch_check_back_btn(void);

/**
 * 注册滑动回调函数
 * @param callback 回调函数，delta_y > 0 表示上滑，delta_y < 0 表示下滑
 */
void touch_register_swipe_callback(void (*callback)(int32_t delta_y));

/**
 * 获取并清除累积的滑动距离
 * 在滑动区域（fm_list等）释放触摸后调用，判断是否需要翻页
 * @return 滑动距离（像素），正值=上滑，负值=下滑，0=无滑动
 */
int32_t touch_get_and_clear_swipe_delta(void);

/**
 * 设置滑动区域边界
 * 只有触摸起始位置在此区域内才检测滑动
 * @param y1 区域上边界（像素）
 * @param y2 区域下边界（像素）
 */
void touch_set_swipe_area(int16_t y1, int16_t y2);

/**
 * 清空滑动回调（关闭UI时调用，防止驱动层访问已销毁的回调）
 */
void touch_clear_swipe_callback(void);

#endif /* LV_PORT_INDEV_H */
