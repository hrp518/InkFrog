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

#endif /* SETTINGS_SCREEN_H */