/*
 * WLAN Manager - WIFI连接管理模块
 *
 * 功能：
 * 1. 连接指定WIFI热点
 * 2. 获取IP地址
 * 3. 监控连接状态
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wlan_manager.h"
#include "net/wlan/wlan.h"
#include "common/framework/net_ctrl.h"
#include "common/framework/sys_ctrl/sys_ctrl.h"
#include "lwip/netif.h"
#include "FreeRTOS.h"
#include "task.h"

/* 调试开关 */
#define WLAN_DEBUG 1
#if WLAN_DEBUG
#define WLAN_LOG(fmt, ...) printf("[WLAN] " fmt "\r\n", ##__VA_ARGS__)
#else
#define WLAN_LOG(fmt, ...) 
#endif

/* 全局变量 */
static volatile WLAN_State_t g_wlan_state = WLAN_STATE_IDLE;
static int g_connected = 0;

/* 获取WLAN netif - 使用SDK内部的g_wlan_netif */
static struct netif* wlan_get_netif(void)
{
    extern struct netif *g_wlan_netif;
    return g_wlan_netif;
}

/*
 * 初始化WLAN模块
 */
int wlan_manager_init(void)
{
    WLAN_LOG("WLAN Manager initializing...");
    
    /* 设置为Station模式 */
    if (net_switch_mode(WLAN_MODE_STA) != 0) {
        WLAN_LOG("Failed to set STA mode");
        return -1;
    }
    
    g_wlan_state = WLAN_STATE_IDLE;
    WLAN_LOG("WLAN Manager initialized");
    return 0;
}

/*
 * 连接WIFI
 */
int wlan_manager_connect(const char *ssid, const char *passwd)
{
    int ret;
    
    if (ssid == NULL) {
        WLAN_LOG("SSID is NULL");
        return -1;
    }
    
    WLAN_LOG("Connecting to SSID: %s", ssid);
    g_wlan_state = WLAN_STATE_CONNECTING;
    
    /* 设置SSID和密码 */
    if (passwd == NULL || strlen(passwd) == 0) {
        ret = wlan_sta_set((unsigned char*)ssid, strlen(ssid), NULL);
    } else {
        ret = wlan_sta_set((unsigned char*)ssid, strlen(ssid), (unsigned char*)passwd);
    }
    
    if (ret != 0) {
        WLAN_LOG("Failed to set SSID/Password");
        g_wlan_state = WLAN_STATE_FAILED;
        return -1;
    }
    
    /* 启用Station并连接 */
    ret = wlan_sta_enable();
    if (ret != 0) {
        WLAN_LOG("Failed to enable STA");
        g_wlan_state = WLAN_STATE_FAILED;
        return -1;
    }
    
    return 0;
}

/*
 * 断开WIFI连接
 */
int wlan_manager_disconnect(void)
{
    WLAN_LOG("Disconnecting...");
    g_connected = 0;
    g_wlan_state = WLAN_STATE_DISCONNECTED;
    wlan_sta_disable();
    return 0;
}

/*
 * 获取连接状态
 */
WLAN_State_t wlan_manager_get_state(void)
{
    return g_wlan_state;
}

/*
 * 获取IP信息
 */
int wlan_manager_get_ip_info(WLAN_IPInfo_t *info)
{
    struct netif *netif = wlan_get_netif();
    if (netif == NULL) {
        return -1;
    }
    
    /* 检查是否有有效IP */
    if (netif->ip_addr.addr == 0) {
        return -1;
    }
    
    /* 格式化IP地址 */
    uint8_t *ip = (uint8_t *)&netif->ip_addr.addr;
    uint8_t *mask = (uint8_t *)&netif->netmask.addr;
    uint8_t *gw = (uint8_t *)&netif->gw.addr;
    
    sprintf(info->ip, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    sprintf(info->mask, "%d.%d.%d.%d", mask[0], mask[1], mask[2], mask[3]);
    sprintf(info->gw, "%d.%d.%d.%d", gw[0], gw[1], gw[2], gw[3]);
    
    return 0;
}

/* 事件观察者，用于监听network up事件 */
static observer_base *g_network_up_observer = NULL;

/*
 * network up事件回调
 */
static void network_up_callback(uint32_t event, uint32_t data, void *arg)
{
    WLAN_LOG("Network up event received!");
    g_wlan_state = WLAN_STATE_CONNECTED;
    g_connected = 1;
}

/*
 * 等待获取IP - 使用事件驱动方式
 */
int wlan_manager_wait_for_ip(uint32_t timeout_ms)
{
    struct netif *netif;
    
    /* 首先检查是否已经有IP（快速路径） */
    netif = wlan_get_netif();
    if (netif != NULL && netif->ip_addr.addr != 0) {
        WLAN_IPInfo_t ip_info;
        wlan_manager_get_ip_info(&ip_info);
        WLAN_LOG("Already have IP: %s", ip_info.ip);
        g_wlan_state = WLAN_STATE_CONNECTED;
        g_connected = 1;
        return 0;
    }
    
    /* 注册network up事件观察者 */
    if (g_network_up_observer == NULL) {
        g_network_up_observer = callback_observer_create(
            CTRL_MSG_TYPE_NETWORK << 16 | NET_CTRL_MSG_NETWORK_UP,
            network_up_callback,
            NULL);
        if (g_network_up_observer != NULL) {
            sys_ctrl_attach(g_network_up_observer);
            WLAN_LOG("Registered network up observer");
        } else {
            WLAN_LOG("Failed to create network up observer");
        }
    }
    
    /* 等待network up事件或超时 */
    uint32_t start = xTaskGetTickCount();
    uint32_t timeout_ticks = timeout_ms / (uint32_t)portTICK_RATE_MS;
    if (timeout_ticks == 0) timeout_ticks = 1;
    
    WLAN_LOG("Waiting for network up event (timeout=%ums, %u ticks)...", timeout_ms, timeout_ticks);
    
    while (1) {
        uint32_t elapsed = xTaskGetTickCount() - start;
        
        /* 检查是否已收到network up事件 */
        if (g_connected) {
            /* 立即尝试获取IP */
            netif = wlan_get_netif();
            if (netif != NULL && netif->ip_addr.addr != 0) {
                WLAN_IPInfo_t ip_info;
                wlan_manager_get_ip_info(&ip_info);
                WLAN_LOG("Got IP: %s (elapsed=%u ticks)", ip_info.ip, elapsed);
                return 0;
            }
            /* IP还没到，稍后重试（可能DHCP还在处理中）*/
            WLAN_LOG("Network up but IP not ready, waiting...");
        }
        
        /* 检查超时 */
        if (elapsed >= timeout_ticks) {
            /* 最后再检查一次IP */
            netif = wlan_get_netif();
            if (netif != NULL && netif->ip_addr.addr != 0) {
                WLAN_IPInfo_t ip_info;
                wlan_manager_get_ip_info(&ip_info);
                WLAN_LOG("Got IP at last moment: %s (elapsed=%u ticks)", ip_info.ip, elapsed);
                return 0;
            }
            WLAN_LOG("Wait for IP timeout (elapsed=%u ticks)", elapsed);
            return -1;
        }
        
        /* 等待事件或轮询 */
        vTaskDelay(100 / (uint32_t)portTICK_RATE_MS);
    }
}

/*
 * 注册状态回调
 */
void wlan_manager_register_callback(WLAN_Callback_t callback, void *user_data)
{
    /* 回调功能暂未实现 */
}

/*
 * 设置连接状态（内部使用）
 */
void wlan_manager_set_connected(int connected)
{
    g_connected = connected;
    if (connected) {
        g_wlan_state = WLAN_STATE_CONNECTED;
    }
}
