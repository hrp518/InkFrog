/*
 * WLAN Manager - WIFI连接管理模块
 * 
 * 功能：
 * 1. 连接指定WIFI热点
 * 2. 获取IP地址
 * 3. WiFi扫描（同步/异步）
 * 4. 监控连接状态
 */

#ifndef __WLAN_MANAGER_H__
#define __WLAN_MANAGER_H__

#include <stdint.h>

/* WiFi扫描结果 */
#define WLAN_MAX_SCAN_RESULTS  20
#define WLAN_MGR_MAX_SSID_LEN  33

typedef struct {
    char ssid[WLAN_MGR_MAX_SSID_LEN];
    int rssi;           /* signal level in dbm */
    int is_encrypted;   /* 1=需要密码, 0=开放 */
} WLAN_ScanResult_t;

/* 连接状态 (low-level, 仅给老代码用) */
typedef enum {
    WLAN_STATE_IDLE = 0,
    WLAN_STATE_CONNECTING,
    WLAN_STATE_CONNECTED,
    WLAN_STATE_DISCONNECTED,
    WLAN_STATE_FAILED
} WLAN_State_t;

/* 高层 phase (4 态) - 新的 UI 状态机基于这个 */
typedef enum {
    WLAN_PHASE_NO_CONFIG = 0,    /* INI 无 SSID */
    WLAN_PHASE_DISCONNECTED,     /* 有 SSID 但没连上 / 失败 */
    WLAN_PHASE_CONNECTING,       /* 正在连接 */
    WLAN_PHASE_CONNECTED         /* 已连上 */
} WLAN_Phase_t;

/* Phase 变化回调 - 由 wifi_controller 注册, 在 LVGL 线程中调用 */
typedef void (*WLAN_PhaseCb_t)(WLAN_Phase_t phase, void *user_data);

/* IP信息 */
typedef struct {
    char ip[16];
    char mask[16];
    char gw[16];
} WLAN_IPInfo_t;

/* 异步扫描完成回调 - 在LVGL线程中调用 */
typedef void (*WLAN_ScanDoneCb_t)(int count, WLAN_ScanResult_t *results, void *user_data);

/* 异步连接完成回调 - 在LVGL线程中调用 */
typedef void (*WLAN_ConnectDoneCb_t)(int success, void *user_data);

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
 * 获取连接状态（读内部变量）
 * @return 当前状态
 */
WLAN_State_t wlan_manager_get_state(void);

/*
 * 实时检测WiFi是否真正连接并有IP
 * 不依赖内部变量，直接查询netif状态
 * @return 1已连接有IP, 0未连接
 */
int wlan_manager_is_connected(void);

/*
 * 获取当前连接的SSID
 * @param buf 存放SSID的缓冲区
 * @param buf_size 缓冲区大小
 * @return 0成功, -1未连接或失败
 */
int wlan_manager_get_current_ssid(char *buf, int buf_size);

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
 * WiFi同步扫描 - 扫描周围AP（阻塞调用，不要在LVGL线程中使用！）
 * @param results 扫描结果数组
 * @param max_count 最大结果数
 * @return 实际找到的AP数量, -1表示失败
 */
int wlan_manager_scan(WLAN_ScanResult_t *results, int max_count);

/*
 * WiFi异步扫描 - 在后台线程中扫描，完成后通过wlan_manager_poll()回调
 * 必须在lvgl_task中周期调用wlan_manager_poll()来触发回调
 * @param cb 扫描完成回调
 * @param user_data 传给回调的用户数据
 */
void wlan_manager_scan_async(WLAN_ScanDoneCb_t cb, void *user_data);

/*
 * WiFi异步连接 - 在后台线程中连接，完成后通过wlan_manager_poll()回调
 * @param ssid WIFI名称
 * @param passwd WIFI密码
 * @param cb 连接完成回调
 * @param user_data 传给回调的用户数据
 */
void wlan_manager_connect_async(const char *ssid, const char *passwd,
                                WLAN_ConnectDoneCb_t cb, void *user_data);

/*
 * WLAN轮询 - 必须在LVGL线程中周期调用
 * 检查后台扫描/连接是否完成，完成后安全调用回调
 */
void wlan_manager_poll(void);

/*
 * 取消正在进行的WiFi连接（用于进入FM前释放内存）
 */
void wlan_manager_cancel_connect(void);

/*
 * 设置连接状态（内部使用）
 */
void wlan_manager_set_connected(int connected);

/*
 * 启动WLAN管理任务（内部使用）
 */
void wlan_manager_task(void *arg);

/* ====== Phase 高层状态机接口 (新增) ====== */

/* 获取当前 phase */
WLAN_Phase_t wlan_manager_get_phase(void);

/* 设置 phase (内部用, 状态变化会触发 callback) */
void wlan_manager_set_phase(WLAN_Phase_t phase);

/* 注册 phase 变化回调 (callback 在 LVGL 线程中通过 wlan_manager_poll() 分发) */
void wlan_manager_set_phase_callback(WLAN_PhaseCb_t cb, void *user_data);

#endif /* __WLAN_MANAGER_H__ */