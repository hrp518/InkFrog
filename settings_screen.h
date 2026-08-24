/**
 * @file settings_screen.h
 * @brief 设置界面模块
 * 
 * 功能：
 * 1. WiFi扫描与连接（密码输入、INI存储）
 * 2. 字体选择（从font文件夹选择TTF）
 */

#ifndef SETTINGS_SCREEN_H
#define SETTINGS_SCREEN_H

#include "lvgl/lvgl.h"
#include "wlan_manager.h"

/**
 * @brief 初始化设置模块（从INI加载已保存的设置）
 */
void settings_screen_init(void);

/**
 * @brief 获取用户选择的字体路径
 * @return 字体文件路径，如未选择返回NULL
 */
const char* settings_get_selected_font(void);

/**
 * @brief 设置并持久化阅读器 TTF 路径（HTTP 上传 / 设置页共用）
 */
void settings_set_reader_font_path(const char *path);

/**
 * @brief 打开WiFi扫描界面（创建新屏幕）
 * @param return_screen 返回时加载的屏幕
 */
void settings_wifi_scan_open(lv_obj_t *return_screen);

/**
 * @brief 打开字体选择界面（创建新屏幕）
 * @param return_screen 返回时加载的屏幕
 */
void settings_font_select_open(lv_obj_t *return_screen);

/**
 * @brief WiFi phase 变化通知 (由 main.c 的 phase callback 调用)
 * @param phase 当前 WiFi phase
 */
void settings_wifi_on_phase_change(WLAN_Phase_t phase);

/**
 * @brief 触摸校准测试界面（调试用）
 *
 * 画 3×4 网格按钮覆盖屏幕，点击任意按钮时打印：
 *   - LVGL 收到的触摸坐标 (data->point)
 *   - 按钮在屏幕上的实际区域坐标 (lv_obj_get_coords)
 * 用于验证触摸面板坐标与 LVGL 渲染坐标是否一致（排查"按G出V"错位）。
 */
void settings_touch_test_open(void);

/**
 * @brief 打开“关于”界面（两层：固件信息 → 作者信息）
 * @param return_screen 返回时加载的屏幕（当前实现返回设置主页）
 */
void settings_about_open(lv_obj_t *return_screen);

#endif /* SETTINGS_SCREEN_H */