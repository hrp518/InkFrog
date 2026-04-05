/*
 * WLAN Manager - WIFI连接管理模块
 * 
 * 功能：
 * 1. 连接指定WIFI热点
 * 2. 获取IP地址
 * 3. 监控连接状态
 */

#ifndef __WLAN_MANAGER_H__
#define __WLAN_MANAGER_H__

#include <stdint.h>

/* WIFI配置 - 请根据实际情况修改 */
#define WIFI_SSID     "OpenWrt"      /* WIFI名称 */
#define WIFI_PASSWD   "lanlele518"   /* WIFI密码 */

/* 连接状态 */
typedef enum {
    WLAN_STATE_IDLE = 0,
    WLAN_STATE_CONNECTING,
    WLAN_STATE_CONNECTED,
    WLAN_STATE_DISCONNECTED,
    WLAN_STATE_FAILED
} WLAN_State_t;

/* IP信息 */
typedef struct {
    char ip[16];
    char mask[16];
    char gw[16];
} WLAN_IPInfo_t;

/* 回调类型 */
typedef void (*WLAN_Callback_t)(WLAN_State_t state, void *user_data);

/*
 * 初始化WLAN模块
 * @return 0成功, 其他失败
 */
int wlan_manager_init(void);

/*
 * 连接WIFI
 * @param ssid WIFI名称
 * @param passwd WIFI密码
 * @return 0成功, 其他失败
 */
int wlan_manager_connect(const char *ssid, const char *passwd);

/*
 * 断开WIFI连接
 * @return 0成功, 其他失败
 */
int wlan_manager_disconnect(void);

/*
 * 获取连接状态
 * @return 当前状态
 */
WLAN_State_t wlan_manager_get_state(void);

/*
 * 获取IP信息
 * @param info IP信息结构体指针
 * @return 0成功, 其他失败
 */
int wlan_manager_get_ip_info(WLAN_IPInfo_t *info);

/*
 * 等待获取IP
 * @param timeout_ms 超时时间(毫秒)
 * @return 0成功获取IP, -1超时
 */
int wlan_manager_wait_for_ip(uint32_t timeout_ms);

/*
 * 注册状态回调
 * @param callback 回调函数
 * @param user_data 用户数据
 */
void wlan_manager_register_callback(WLAN_Callback_t callback, void *user_data);

/*
 * 启动WLAN管理任务（内部使用）
 */
void wlan_manager_task(void *arg);

#endif /* __WLAN_MANAGER_H__ */
