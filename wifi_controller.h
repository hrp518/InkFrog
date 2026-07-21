/*
 * wifi_controller - WiFi 状态机 (手机风格)
 *
 * 4 态机:
 *   NO_CONFIG          INI 无 SSID, 等用户进 Settings 配
 *   DISCONNECTED       有 SSID 但连不上, 自动重试 (指数退避, 最多 5 次)
 *   CONNECTING         正在连接
 *   CONNECTED          已连上, 监控 link, 掉了回 DISCONNECTED
 *
 * 取代 main.c 里散落的 g_wifi_connected / wifi_connect_task.
 */

#ifndef __WIFI_CONTROLLER_H__
#define __WIFI_CONTROLLER_H__

#include "wlan_manager.h"

/* 全局状态 - 取代散落的 g_wifi_connected/g_http_running/g_ip_text 等 */
typedef struct {
    int  enabled;         /* WiFi 总开关: 1=开启, 0=关闭 */
    WLAN_Phase_t phase;
    char ssid[WLAN_MGR_MAX_SSID_LEN];
    char password[65];
    char ip[16];
    int  retry_count;     /* 当前重试次数 */
    int  http_running;    /* HTTP 服务器是否在跑 (用于联动关闭) */
    int  fm_paused;       /* XR872 修复: FM/EPUB 期间 wc_task 暂停重试, 释放 SRAM 给 inflate */
    int  settings_paused; /* Settings 界面期间暂停后台连 WiFi, 避免与 EPD 刷新竞态卡死 lvgl */
} wifi_ctx_t;

extern wifi_ctx_t g_wifi;

/* 初始化 (主线程, 在 wlan_manager_init 之后调) */
void wifi_controller_init(void);

/* 启动状态机后台线程 */
void wifi_controller_start(void);

/* 停止控制器线程并 idle WLAN (休眠前调用)
 * 取消连接 + 等 wc_task 退出 + wlan_sta_disable + net 栈沉淀。 */
void wifi_controller_stop(void);

/* fix-power-saving v2: 休眠前手动 wlan_sta_disable + 等 Sys3 掉电。
 * 前置: 必须先调 wifi_controller_stop() 停 wc_task。
 * 配合: 调用方需 pm_unregister_wlan_power_onoff() 屏蔽 SDK 重复 teardown。 */
void wifi_controller_poweroff(void);

/* 注册 phase 回调 (LVGL 线程中安全) - 通常 main.c 用来更新 status bar */
void wifi_controller_register_cb(WLAN_PhaseCb_t cb, void *user_data);

/* 用户请求"立即重试" (从 settings scan 后调, 或 status bar 点击) */
void wifi_controller_request_retry(void);

/* 用户请求开启 WiFi 总开关 */
void wifi_controller_request_enable(void);

/* 用户请求关闭 WiFi 总开关 - 保留已保存的网络配置 */
void wifi_controller_request_disable(void);

/* 兼容旧接口: 用户请求"断开" */
void wifi_controller_request_disconnect(void);

/* HTTP 状态变更 (main.c 在 start/stop HTTP 时调) */
void wifi_controller_set_http_running(int running);

/* 状态机轮询 (LVGL 线程中调, 触发 callback) - 替代 main.c 直接调 wlan_manager_poll */
void wifi_controller_poll(void);

#endif /* __WIFI_CONTROLLER_H__ */
