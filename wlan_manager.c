/*
 * WLAN Manager - WIFI连接管理模块
 *
 * 功能：
 * 1. 连接指定WIFI热点
 * 2. 获取IP地址
 * 3. WiFi扫描（同步/异步）
 * 4. 监控连接状态
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
#include "sys/sys_heap.h"

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
static volatile int g_connect_canceled = 0;  /* 取消连接标志 */
static char g_current_ssid[WLAN_MGR_MAX_SSID_LEN] = {0}; /* 当前连接的SSID */

/* 高层 phase + 回调 (新增) */
static volatile WLAN_Phase_t g_wlan_phase = WLAN_PHASE_NO_CONFIG;
static WLAN_PhaseCb_t g_phase_cb = NULL;
static void *g_phase_cb_ud = NULL;
static volatile int g_phase_pending = 0;  /* pending change to dispatch in poll */

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
 * 连接前预扫描：先让 WSM 完成一轮 scan，再 associate（对齐原版）
 */
int wlan_manager_pre_scan(const char *want_ssid, uint32_t wait_ms)
{
    int ret;
    int num = 0;
    uint32_t elapsed = 0;
    int found = 0;

    if (wait_ms == 0) {
        wait_ms = 1500;
    }

    /* 新一轮连接流程：清掉上次 cancel，避免快速重试被误杀 */
    g_connect_canceled = 0;

    WLAN_LOG("Pre-scan start (want='%s', wait=%ums)",
             want_ssid ? want_ssid : "", (unsigned)wait_ms);

    ret = wlan_sta_scan_once();
    if (ret != 0) {
        WLAN_LOG("Pre-scan start failed: %d", ret);
        return -1;
    }

    while (elapsed < wait_ms) {
        if (g_connect_canceled) {
            WLAN_LOG("Pre-scan canceled");
            return -1;
        }
        vTaskDelay(200 / (uint32_t)portTICK_RATE_MS);
        elapsed += 200;

        if (wlan_sta_get_scan_result_num(&num) != 0 || num <= 0) {
            continue;
        }

        if (want_ssid == NULL || want_ssid[0] == '\0') {
            WLAN_LOG("Pre-scan got %d APs in %ums", num, (unsigned)elapsed);
            return num;
        }

        {
            int n = num;
            int i;
            wlan_sta_ap_t *ap_list;
            wlan_sta_scan_results_t scan_results;

            if (n > WLAN_MAX_SCAN_RESULTS) {
                n = WLAN_MAX_SCAN_RESULTS;
            }
            ap_list = (wlan_sta_ap_t *)psram_malloc(n * sizeof(wlan_sta_ap_t));
            if (!ap_list) {
                WLAN_LOG("Pre-scan alloc fail, treat as ready (%d APs)", num);
                return num;
            }
            scan_results.ap = ap_list;
            scan_results.size = n;
            if (wlan_sta_scan_result(&scan_results) == 0) {
                for (i = 0; i < scan_results.num; i++) {
                    if (strncmp((const char *)ap_list[i].ssid.ssid, want_ssid,
                                WLAN_MGR_MAX_SSID_LEN) == 0) {
                        found = 1;
                        break;
                    }
                }
            }
            psram_free(ap_list);
        }

        if (found) {
            WLAN_LOG("Pre-scan found '%s' in %ums (aps=%d)",
                     want_ssid, (unsigned)elapsed, num);
            return num;
        }
        WLAN_LOG("Pre-scan %d APs, target not yet, keep waiting...", num);
    }

    WLAN_LOG("Pre-scan done elapsed=%ums aps=%d found=%d",
             (unsigned)elapsed, num, found);
    return (num > 0) ? num : 0;
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
    g_connect_canceled = 0;
    g_connected = 0;
    g_wlan_state = WLAN_STATE_CONNECTING;
    
    /* 记录当前连接的SSID */
    strncpy(g_current_ssid, ssid, WLAN_MGR_MAX_SSID_LEN - 1);
    g_current_ssid[WLAN_MGR_MAX_SSID_LEN - 1] = '\0';

    /* 设置SSID和密码 */
    WLAN_LOG("[TRACE] wlan_manager_connect: calling wlan_sta_set");
    if (passwd == NULL || strlen(passwd) == 0) {
        ret = wlan_sta_set((unsigned char*)ssid, strlen(ssid), NULL);
    } else {
        ret = wlan_sta_set((unsigned char*)ssid, strlen(ssid), (unsigned char*)passwd);
    }
    WLAN_LOG("[TRACE] wlan_manager_connect: wlan_sta_set returned ret=%d", ret);

    if (ret != 0) {
        WLAN_LOG("Failed to set SSID/Password");
        g_wlan_state = WLAN_STATE_FAILED;
        return -1;
    }

    /* 启用Station并连接 */
    WLAN_LOG("[TRACE] wlan_manager_connect: calling wlan_sta_enable");
    ret = wlan_sta_enable();
    WLAN_LOG("[TRACE] wlan_manager_connect: wlan_sta_enable returned ret=%d", ret);
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
    g_current_ssid[0] = '\0';
    wlan_sta_disable();
    /* 不在此处改 phase — phase 由 controller 根据 INI 决定是 DISCONNECTED 还是 NO_CONFIG */
    return 0;
}

/*
 * 获取连接状态（读内部变量）
 */
WLAN_State_t wlan_manager_get_state(void)
{
    return g_wlan_state;
}

/*
 * 实时检测WiFi是否真正连接并有IP
 * 不依赖内部变量，直接查询netif状态
 */
int wlan_manager_is_connected(void)
{
    struct netif *netif = wlan_get_netif();
    if (netif == NULL) return 0;
    
    /* 检查netif是否up且有IP */
    if (netif_is_up(netif) && netif->ip_addr.addr != 0) {
        /* 同步内部状态 */
        if (!g_connected) {
            g_connected = 1;
            g_wlan_state = WLAN_STATE_CONNECTED;
        }
        return 1;
    }
    return 0;
}

/*
 * 获取当前连接的SSID
 */
int wlan_manager_get_current_ssid(char *buf, int buf_size)
{
    if (buf == NULL || buf_size <= 0) return -1;
    
    if (!wlan_manager_is_connected()) {
        return -1;
    }
    
    if (g_current_ssid[0] == '\0') {
        return -1;
    }
    
    strncpy(buf, g_current_ssid, buf_size - 1);
    buf[buf_size - 1] = '\0';
    return 0;
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

/* 事件观察者，用于监听 network / wlan 事件 */
static observer_base *g_network_up_observer = NULL;
static observer_base *g_wlan_event_observer = NULL;
static volatile int g_wlan_assoc_ok = 0;   /* 已关联 (WPA done / connected) */
static volatile int g_wlan_assoc_fail = 0; /* 关联失败事件 */

/*
 * network up事件回调
 */
static void network_up_callback(uint32_t event, uint32_t data, void *arg)
{
    (void)event;
    (void)data;
    (void)arg;
    WLAN_LOG("Network up event received!");
    g_wlan_state = WLAN_STATE_CONNECTED;
    g_connected = 1;
    g_wlan_assoc_ok = 1;
    WLAN_LOG("[TRACE] network_up_callback set g_connected=1");
}

static void wlan_event_callback(uint32_t event, uint32_t data, void *arg)
{
    uint16_t type = EVENT_SUBTYPE(event);
    (void)data;
    (void)arg;

    switch (type) {
    case NET_CTRL_MSG_WLAN_CONNECTED:
        g_wlan_assoc_ok = 1;
        g_wlan_assoc_fail = 0;
        WLAN_LOG("WLAN_CONNECTED (assoc ok)");
        break;
    case NET_CTRL_MSG_WLAN_DISCONNECTED:
    case NET_CTRL_MSG_WLAN_CONNECT_FAILED:
    case NET_CTRL_MSG_WLAN_4WAY_HANDSHAKE_FAILED:
    case NET_CTRL_MSG_CONNECTION_LOSS:
        if (!g_connected) {
            g_wlan_assoc_fail = 1;
            WLAN_LOG("WLAN link fail event=%u", (unsigned)type);
        }
        break;
    default:
        break;
    }
}

static void wlan_manager_ensure_observers(void)
{
    if (g_network_up_observer == NULL) {
        g_network_up_observer = callback_observer_create(
            CTRL_MSG_TYPE_NETWORK << 16 | NET_CTRL_MSG_NETWORK_UP,
            network_up_callback,
            NULL);
        if (g_network_up_observer != NULL) {
            sys_ctrl_attach(g_network_up_observer);
            WLAN_LOG("Registered network up observer");
        }
    }
    if (g_wlan_event_observer == NULL) {
        g_wlan_event_observer = callback_observer_create(
            CTRL_MSG_TYPE_NETWORK << 16 | NET_CTRL_MSG_ALL,
            wlan_event_callback,
            NULL);
        if (g_wlan_event_observer != NULL) {
            sys_ctrl_attach(g_wlan_event_observer);
            WLAN_LOG("Registered wlan event observer");
        }
    }
}

/*
 * 等待获取IP - 使用事件驱动方式
 */
int wlan_manager_wait_for_ip(uint32_t timeout_ms)
{
    return wlan_manager_wait_for_ip_ex(timeout_ms, timeout_ms);
}

int wlan_manager_wait_for_ip_ex(uint32_t no_assoc_ms, uint32_t after_assoc_ms)
{
    struct netif *netif;
    uint32_t start;
    uint32_t assoc_at = 0;

    if (no_assoc_ms == 0) {
        no_assoc_ms = 1000;
    }
    if (after_assoc_ms == 0) {
        after_assoc_ms = 5000;
    }

    g_wlan_assoc_ok = 0;
    g_wlan_assoc_fail = 0;

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

    wlan_manager_ensure_observers();

    start = xTaskGetTickCount();
    WLAN_LOG("Wait IP: no_assoc=%ums after_assoc=%ums",
             (unsigned)no_assoc_ms, (unsigned)after_assoc_ms);

    while (1) {
        uint32_t elapsed = xTaskGetTickCount() - start;

        if (g_connected) {
            netif = wlan_get_netif();
            if (netif != NULL && netif->ip_addr.addr != 0) {
                WLAN_IPInfo_t ip_info;
                wlan_manager_get_ip_info(&ip_info);
                WLAN_LOG("Got IP: %s (elapsed=%u ticks)", ip_info.ip, elapsed);
                return 0;
            }
        }

        if (g_connect_canceled) {
            WLAN_LOG("WiFi connect canceled by user");
            return -1;
        }

        if (g_wlan_assoc_ok && assoc_at == 0) {
            assoc_at = xTaskGetTickCount();
            WLAN_LOG("Assoc reached, DHCP window %ums", (unsigned)after_assoc_ms);
        }

        /* 未关联：超时或失败事件 → 立即返回，供上层快速重连 */
        if (!g_wlan_assoc_ok && !g_connected) {
            if (g_wlan_assoc_fail && elapsed >= (200 / (uint32_t)portTICK_RATE_MS)) {
                WLAN_LOG("Abort wait: assoc fail event (elapsed=%u)", elapsed);
                return -1;
            }
            if (elapsed >= (no_assoc_ms / (uint32_t)portTICK_RATE_MS)) {
                WLAN_LOG("Abort wait: no assoc in %ums", (unsigned)no_assoc_ms);
                return -1;
            }
        } else {
            uint32_t since_assoc = xTaskGetTickCount() - (assoc_at ? assoc_at : start);
            if (since_assoc >= (after_assoc_ms / (uint32_t)portTICK_RATE_MS)) {
                netif = wlan_get_netif();
                if (netif != NULL && netif->ip_addr.addr != 0) {
                    WLAN_IPInfo_t ip_info;
                    wlan_manager_get_ip_info(&ip_info);
                    WLAN_LOG("Got IP at DHCP deadline: %s", ip_info.ip);
                    return 0;
                }
                WLAN_LOG("DHCP timeout after assoc (elapsed=%u)", elapsed);
                return -1;
            }
        }

        vTaskDelay(50 / (uint32_t)portTICK_RATE_MS);
    }
}

/*
 * 取消正在进行的WiFi连接
 */
void wlan_manager_cancel_connect(void)
{
    WLAN_State_t state = g_wlan_state;
    
    /* XR872 修复: 即使已 CONNECTED 也要设停止标志 (不再 skip)。
     * wifi_controller_stop 调本函数是为了让 wc_task 主循环尽快退出，
     * 若已连接就 skip，wc_task 会卡在 CONNECTED 分支的 OS_MSleep 里不退出，
     * 导致休眠时 net_sys_onoff teardown 与本线程竞争 wlan 驱动 ->
     * BUG at wsm_remove_key_request:1027 断言崩溃。 */
    WLAN_LOG("Cancel requested (current state=%d)", state);
    
    /* 设置取消标志 (wait_for_ip 轮询 + wc_task 主循环都会检查) */
    g_connect_canceled = 1;
    
    if (state != WLAN_STATE_CONNECTED) {
        /* 重置状态 (不停 STA，留给 disconnect/net_sys_onoff) */
        g_wlan_state = WLAN_STATE_IDLE;
        g_connected = 0;
    }
    
    WLAN_LOG("Cancel requested (STA disable deferred to net_sys_onoff)");
}

/*
 * WiFi同步扫描 - 扫描周围AP（阻塞调用！）
 */
int wlan_manager_scan(WLAN_ScanResult_t *results, int max_count)
{
    int ret, num, i;
    wlan_sta_ap_t *ap_list = NULL;
    
    if (!results || max_count <= 0) {
        WLAN_LOG("Invalid scan params");
        return -1;
    }
    
    WLAN_LOG("Starting WiFi scan...");
    
    /* 启动扫描 */
    ret = wlan_sta_scan_once();
    if (ret != 0) {
        WLAN_LOG("Scan failed to start: %d", ret);
        return -1;
    }
    
    /* 等待扫描完成 */
    vTaskDelay(3000 / (uint32_t)portTICK_RATE_MS);
    
    /* 获取扫描结果数量 */
    ret = wlan_sta_get_scan_result_num(&num);
    if (ret != 0) {
        WLAN_LOG("Failed to get scan result num: %d", ret);
        return -1;
    }
    
    WLAN_LOG("Scan found %d APs", num);
    
    if (num <= 0) {
        return 0;
    }
    
    /* 限制最大数量 */
    if (num > max_count) {
        num = max_count;
    }
    
    /* 分配内存存放扫描结果 */
    ap_list = (wlan_sta_ap_t *)psram_malloc(num * sizeof(wlan_sta_ap_t));
    if (!ap_list) {
        WLAN_LOG("Failed to allocate scan results");
        return -1;
    }
    
    /* 获取扫描结果 */
    wlan_sta_scan_results_t scan_results;
    scan_results.ap = ap_list;
    scan_results.size = num;
    
    ret = wlan_sta_scan_result(&scan_results);
    if (ret != 0) {
        WLAN_LOG("Failed to get scan results: %d", ret);
        psram_free(ap_list);
        return -1;
    }
    
    /* 转换结果 - 同 SSID 多个信道只保留 rssi 最大的 (空 SSID 隐藏网络不过滤) */
    int count = scan_results.num;
    if (count > max_count) count = max_count;

    int out = 0;
    for (i = 0; i < count; i++) {
        int ssid_len = ap_list[i].ssid.ssid_len;
        if (ssid_len > WLAN_MGR_MAX_SSID_LEN - 1) {
            ssid_len = WLAN_MGR_MAX_SSID_LEN - 1;
        }

        /* 跳过隐藏网络（空 SSID） */
        if (ap_list[i].ssid.ssid[0] == '\0') continue;

        /* dedup: 同 SSID 只保留信号最强的 */
        {
            int j;
            for (j = 0; j < out; j++) {
                if (strcmp(results[j].ssid, ap_list[i].ssid.ssid) == 0) {
                    if (ap_list[i].level > results[j].rssi) {
                        results[j].rssi = ap_list[i].level;
                    }
                    goto next_ap;
                }
            }
        }

        memcpy(results[out].ssid, ap_list[i].ssid.ssid, ssid_len);
        results[out].ssid[ssid_len] = '\0';
        results[out].rssi = ap_list[i].level;
        results[out].is_encrypted = (ap_list[i].wpa_flags & (WPA_FLAGS_WPA | WPA_FLAGS_WPA2 | WPA_FLAGS_WEP)) ? 1 : 0;

        WLAN_LOG("  AP[%d]: %s (rssi=%d, enc=%d)", out, results[out].ssid, results[out].rssi, results[out].is_encrypted);
        out++;
    next_ap:
        continue;
    }

    psram_free(ap_list);
    WLAN_LOG("Scan complete, %d results", out);
    return out;
}

/* ====== 异步扫描/连接支持 ====== */

/* 异步扫描上下文 - 在PSRAM中分配 */
typedef struct {
    WLAN_ScanResult_t *results;
    int count;
    WLAN_ScanDoneCb_t cb;
    void *user_data;
} ScanAsyncCtx_t;

/* 异步连接上下文 */
typedef struct {
    char ssid[WLAN_MGR_MAX_SSID_LEN];
    char password[64];
    int success;
    WLAN_ConnectDoneCb_t cb;
    void *user_data;
} ConnectAsyncCtx_t;

/* 线程安全的回调调度：后台线程完成工作后不直接调用lv_async_call，
 * 而是存入pending变量设置标志，由LVGL线程的wlan_manager_poll()检查并调用回调。
 * 这样所有LVGL操作都在LVGL线程中执行，避免竞态条件导致崩溃。
 */
static volatile int g_scan_done = 0;
static ScanAsyncCtx_t *volatile g_pending_scan_ctx = NULL;

static volatile int g_connect_done = 0;
static ConnectAsyncCtx_t *volatile g_pending_connect_ctx = NULL;

/*
 * 异步扫描线程函数
 * 扫描完成后设置标志位，由wlan_manager_poll()在LVGL线程中调用回调
 */
static void scan_async_task(void *arg)
{
    ScanAsyncCtx_t *ctx = (ScanAsyncCtx_t *)arg;
    
    WLAN_LOG("[ASYNC_SCAN] Starting background scan...");
    
    /* 分配扫描结果缓冲区 */
    WLAN_ScanResult_t *results = (WLAN_ScanResult_t *)psram_malloc(
        WLAN_MAX_SCAN_RESULTS * sizeof(WLAN_ScanResult_t));
    if (!results) {
        WLAN_LOG("[ASYNC_SCAN] Failed to allocate results");
        ctx->results = NULL;
        ctx->count = -1;
        g_pending_scan_ctx = ctx;
        g_scan_done = 1;
        vTaskDelete(NULL);
        return;
    }
    
    int count = wlan_manager_scan(results, WLAN_MAX_SCAN_RESULTS);
    
    WLAN_LOG("[ASYNC_SCAN] Scan done, count=%d", count);
    
    /* 存储结果，设置标志让LVGL线程poll处理 */
    ctx->results = results;
    ctx->count = count;
    g_pending_scan_ctx = ctx;
    g_scan_done = 1;
    
    /* 注意：ctx由wlan_manager_poll释放 */
    vTaskDelete(NULL);
}

/*
 * WiFi异步扫描
 */
void wlan_manager_scan_async(WLAN_ScanDoneCb_t cb, void *user_data)
{
    WLAN_LOG("[SCAN_ASYNC] enter, cb=%p ud=%p", (void*)cb, user_data);

    if (cb == NULL) return;

    ScanAsyncCtx_t *ctx = (ScanAsyncCtx_t *)psram_malloc(sizeof(ScanAsyncCtx_t));
    if (!ctx) {
        WLAN_LOG("[ASYNC_SCAN] Failed to allocate context");
        return;
    }
    
    ctx->results = NULL;
    ctx->cb = cb;
    ctx->user_data = user_data;
    
    /* 创建后台扫描线程 */
    xTaskCreate(scan_async_task, "wifi_scan",
                4096, ctx, tskIDLE_PRIORITY + 1, NULL);
}

/*
 * 异步连接线程函数
 * 连接完成后设置标志位，由wlan_manager_poll()在LVGL线程中调用回调
 */
static void connect_async_task(void *arg)
{
    ConnectAsyncCtx_t *ctx = (ConnectAsyncCtx_t *)arg;
    int success = 0;
    
    WLAN_LOG("[ASYNC_CONNECT] Connecting to %s...", ctx->ssid);
    
    /* 先断开旧连接 */
    if (wlan_manager_is_connected()) {
        wlan_manager_disconnect();
        vTaskDelay(500 / (uint32_t)portTICK_RATE_MS);
    }
    
    /* 开始连接 */
    g_connect_canceled = 0;
    int ret = wlan_manager_connect(ctx->ssid, ctx->password);
    if (ret == 0) {
        /* 等待IP */
        ret = wlan_manager_wait_for_ip(30000);
        if (ret == 0) {
            WLAN_IPInfo_t ip_info;
            if (wlan_manager_get_ip_info(&ip_info) == 0) {
                WLAN_LOG("[ASYNC_CONNECT] Connected! IP: %s", ip_info.ip);
            }
            success = 1;
        } else {
            WLAN_LOG("[ASYNC_CONNECT] Failed to get IP");
        }
    } else {
        WLAN_LOG("[ASYNC_CONNECT] Failed to start connection");
    }
    
    /* 存储结果，设置标志让LVGL线程poll处理 */
    ctx->success = success;
    g_pending_connect_ctx = ctx;
    g_connect_done = 1;
    
    /* 注意：ctx由wlan_manager_poll释放 */
    vTaskDelete(NULL);
}

/*
 * WiFi异步连接
 */
void wlan_manager_connect_async(const char *ssid, const char *passwd,
                                WLAN_ConnectDoneCb_t cb, void *user_data)
{
    if (ssid == NULL || cb == NULL) return;
    
    ConnectAsyncCtx_t *ctx = (ConnectAsyncCtx_t *)psram_malloc(sizeof(ConnectAsyncCtx_t));
    if (!ctx) {
        WLAN_LOG("[ASYNC_CONNECT] Failed to allocate context");
        return;
    }
    
    strncpy(ctx->ssid, ssid, WLAN_MGR_MAX_SSID_LEN - 1);
    ctx->ssid[WLAN_MGR_MAX_SSID_LEN - 1] = '\0';
    
    if (passwd) {
        strncpy(ctx->password, passwd, sizeof(ctx->password) - 1);
    } else {
        ctx->password[0] = '\0';
    }
    ctx->password[sizeof(ctx->password) - 1] = '\0';
    
    ctx->cb = cb;
    ctx->user_data = user_data;
    
    /* 创建后台连接线程 */
    xTaskCreate(connect_async_task, "wifi_conn",
                4096, ctx, tskIDLE_PRIORITY + 1, NULL);
}

/*
 * WLAN轮询函数 - 必须在LVGL线程中调用（如lvgl_task主循环中）
 * 检查后台扫描/连接是否完成，完成后在LVGL线程安全地调用回调
 */
void wlan_manager_poll(void)
{
    /* 分发 phase 变化回调 (在 LVGL 线程里) */
    if (g_phase_pending) {
        g_phase_pending = 0;
        WLAN_PhaseCb_t cb = g_phase_cb;
        if (cb) {
            cb(g_wlan_phase, g_phase_cb_ud);
        }
    }

    /* 检查异步扫描是否完成 */
    if (g_scan_done) {
        g_scan_done = 0;
        ScanAsyncCtx_t *ctx = g_pending_scan_ctx;
        g_pending_scan_ctx = NULL;
        if (ctx && ctx->cb) {
            WLAN_LOG("[POLL] Dispatching scan callback (count=%d)", ctx->count);
            ctx->cb(ctx->count, ctx->results, ctx->user_data);
        }
        if (ctx) {
            psram_free(ctx);
        }
    }
    
    /* 检查异步连接是否完成 */
    if (g_connect_done) {
        g_connect_done = 0;
        ConnectAsyncCtx_t *ctx = g_pending_connect_ctx;
        g_pending_connect_ctx = NULL;
        if (ctx && ctx->cb) {
            WLAN_LOG("[POLL] Dispatching connect callback (success=%d)", ctx->success);
            ctx->cb(ctx->success, ctx->user_data);
        }
        if (ctx) {
            psram_free(ctx);
        }
    }
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

/*
 * 启动WLAN管理任务（内部使用）
 */
void wlan_manager_task(void *arg)
{
    /* 当前未使用独立任务 */
    vTaskDelete(NULL);
}

/* ====== Phase 高层状态机 (新增) ====== */

WLAN_Phase_t wlan_manager_get_phase(void)
{
    return g_wlan_phase;
}

void wlan_manager_set_phase(WLAN_Phase_t phase)
{
    if (g_wlan_phase == phase) return;
    WLAN_LOG("Phase change: %d -> %d", (int)g_wlan_phase, (int)phase);
    g_wlan_phase = phase;
    g_phase_pending = 1;  /* 让 poll() 在 LVGL 线程里分发 callback */
}

void wlan_manager_set_phase_callback(WLAN_PhaseCb_t cb, void *user_data)
{
    g_phase_cb = cb;
    g_phase_cb_ud = user_data;
}