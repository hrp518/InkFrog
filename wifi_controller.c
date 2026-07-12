/*
 * wifi_controller 实现
 *
 * 状态机线程读 INI, 决定 phase, 调 wlan_manager 做实际连接/断开.
 * 任何 phase 变化通过 wlan_manager_set_phase() 设置, 由 wlan_manager_poll()
 * 在 LVGL 线程里 dispatch 回调.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wifi_controller.h"
#include "settings_storage.h"
#include "http_server.h"
#include "kernel/os/os.h"
#include "FreeRTOS.h"
#include "task.h"

#define WC_DBG 1
#if WC_DBG
#define WC_LOG(fmt, ...) printf("[WIFIC] " fmt "\r\n", ##__VA_ARGS__)
#else
#define WC_LOG(fmt, ...)
#endif

/* TRACE 默认关：wc_task 每秒多条 [TRACE] 会占满串口，拖死 TTF/EPUB 主流程 */
#define WC_TRACE_DBG 0
#if WC_TRACE_DBG
#define WC_TRACE_LOG(fmt, ...) WC_LOG(fmt, ##__VA_ARGS__)
#else
#define WC_TRACE_LOG(fmt, ...) ((void)0)
#endif

#define WC_FM_PAUSE_SLEEP_MS  5000

/* 重试退避 (ms) */
static const uint32_t kBackoffMs[] = { 5000, 10000, 30000, 60000, 60000 };
#define BACKOFF_COUNT  (sizeof(kBackoffMs)/sizeof(kBackoffMs[0]))
#define CONNECT_TIMEOUT_MS  30000
#define POLL_INTERVAL_MS    5000  /* 已连接时检测 link 的周期 */

wifi_ctx_t g_wifi;

static OS_Thread_t g_wc_thread;
static volatile int g_wc_running = 0;
static volatile int g_wc_exited = 0;  /* 线程真正退出标志 (休眠同步用) */
static volatile int g_wc_request_retry = 0;
static volatile int g_wc_request_enable = 0;
static volatile int g_wc_request_disable = 0;
static volatile int g_wc_request_disconnect = 0;

static int wc_has_pending_request(void)
{
    return g_wc_request_retry || g_wc_request_enable ||
           g_wc_request_disable || g_wc_request_disconnect;
}

static int wc_should_abort_sleep(void)
{
    return !g_wc_running || wc_has_pending_request() ||
           g_wifi.fm_paused;
}

static void wc_sleep_interruptible(uint32_t sleep_ms)
{
    while (sleep_ms > 0 && !wc_should_abort_sleep()) {
        uint32_t slice_ms = (sleep_ms > 100) ? 100 : sleep_ms;
        OS_MSleep(slice_ms);
        sleep_ms -= slice_ms;
    }
}

static void wc_save_enabled_to_ini(int enabled)
{
    settings_set_string("wifi", "enabled", enabled ? "1" : "0");
}

static void wc_load_config_from_ini(void)
{
    char enabled_buf[8] = "";

    g_wifi.enabled = 1;
    if (settings_get_string("wifi", "enabled", enabled_buf, sizeof(enabled_buf)) == 0) {
        g_wifi.enabled = (enabled_buf[0] != '0');
    }

    settings_get_string("wifi", "ssid", g_wifi.ssid, sizeof(g_wifi.ssid));
    settings_get_string("wifi", "password", g_wifi.password, sizeof(g_wifi.password));
    WC_LOG("Loaded INI: enabled=%d ssid='%s' pwd_len=%d",
           g_wifi.enabled, g_wifi.ssid, (int)strlen(g_wifi.password));
}

static void wc_set_phase(WLAN_Phase_t p)
{
    g_wifi.phase = p;
    wlan_manager_set_phase(p);
}

static void wc_handle_link_lost(void)
{
    WC_LOG("Link lost, switching to DISCONNECTED");
    g_wifi.ip[0] = '\0';
    wc_set_phase(WLAN_PHASE_DISCONNECTED);
    g_wifi.retry_count = 0;
}

static void wc_try_connect(void)
{
    WC_TRACE_LOG("[TRACE] wc_try_connect enter");
    if (!g_wc_running) {
        return;
    }
    if (g_wifi.fm_paused) {
        wc_set_phase(WLAN_PHASE_DISCONNECTED);
        return;
    }
    if (!g_wifi.enabled) {
        WC_LOG("WiFi is disabled, skip connect");
        wc_set_phase(WLAN_PHASE_DISCONNECTED);
        return;
    }

    if (g_wifi.ssid[0] == '\0') {
        wc_set_phase(WLAN_PHASE_NO_CONFIG);
        return;
    }

    WC_LOG("Attempt %d: connecting to '%s'", g_wifi.retry_count, g_wifi.ssid);
    wc_set_phase(WLAN_PHASE_CONNECTING);

    /* 先确保已断开 */
    if (wlan_manager_is_connected() || wlan_manager_get_state() == WLAN_STATE_CONNECTING) {
        wlan_manager_disconnect();
        OS_MSleep(500);
    }

    WC_TRACE_LOG("[TRACE] calling wlan_manager_connect");
    int ret = wlan_manager_connect(g_wifi.ssid, g_wifi.password);
    WC_TRACE_LOG("[TRACE] wlan_manager_connect returned ret=%d", ret);
    if (ret != 0) {
        WC_LOG("wlan_manager_connect failed: %d", ret);
        g_wifi.retry_count++;
        wc_set_phase(WLAN_PHASE_DISCONNECTED);
        return;
    }
    if (!g_wc_running || g_wifi.fm_paused) {
        return;
    }

    WC_TRACE_LOG("[TRACE] calling wlan_manager_wait_for_ip timeout=%d", CONNECT_TIMEOUT_MS);
    int wait = wlan_manager_wait_for_ip(CONNECT_TIMEOUT_MS);
    WC_TRACE_LOG("[TRACE] wlan_manager_wait_for_ip returned wait=%d", wait);
    if (wait == 0) {
        WLAN_IPInfo_t info;
        if (wlan_manager_get_ip_info(&info) == 0) {
            strncpy(g_wifi.ip, info.ip, sizeof(g_wifi.ip) - 1);
            g_wifi.ip[sizeof(g_wifi.ip) - 1] = '\0';
            WC_LOG("Connected, IP: %s", g_wifi.ip);
            g_wifi.retry_count = 0;
            WC_TRACE_LOG("[TRACE] calling wc_set_phase(CONNECTED)");
            wc_set_phase(WLAN_PHASE_CONNECTED);
            WC_TRACE_LOG("[TRACE] wc_set_phase(CONNECTED) returned");
        } else {
            WC_LOG("No IP after wait");
            g_wifi.retry_count++;
            wc_set_phase(WLAN_PHASE_DISCONNECTED);
        }
    } else {
        WC_LOG("wait_for_ip timeout/canceled");
        g_wifi.retry_count++;
        wc_set_phase(WLAN_PHASE_DISCONNECTED);
    }
}

static void wc_task(void *arg)
{
    WC_LOG("Controller task started");

    /* 初始读 INI 决定 phase */
    wc_load_config_from_ini();
    if (!g_wifi.enabled) {
        wc_set_phase(WLAN_PHASE_DISCONNECTED);
    } else if (g_wifi.ssid[0] == '\0') {
        wc_set_phase(WLAN_PHASE_NO_CONFIG);
    } else {
        wc_set_phase(WLAN_PHASE_DISCONNECTED);
        g_wifi.retry_count = 0;
    }

    g_wc_running = 1;
    g_wc_exited = 0;

    while (g_wc_running) {
        /* FM/EPUB 阅读：静默休眠，零 TRACE 输出，把串口让给 TTF/EPUB */
        if (g_wifi.fm_paused && !wc_has_pending_request()) {
            wc_sleep_interruptible(WC_FM_PAUSE_SLEEP_MS);
            continue;
        }

        WC_TRACE_LOG("[TRACE] wc_task loop top, phase=%d", (int)g_wifi.phase);
        /* 处理外部请求 */
        if (g_wc_request_disable) {
            g_wc_request_disable = 0;
            WC_LOG("User request: disable WiFi");
            g_wifi.enabled = 0;
            wc_save_enabled_to_ini(0);
            wlan_manager_cancel_connect();
            wlan_manager_disconnect();
            g_wifi.ip[0] = '\0';
            g_wifi.retry_count = 0;
            wc_set_phase(WLAN_PHASE_DISCONNECTED);
            wc_sleep_interruptible(200);
            continue;
        }

        if (g_wc_request_enable) {
            g_wc_request_enable = 0;
            WC_LOG("User request: enable WiFi");
            g_wifi.enabled = 1;
            wc_save_enabled_to_ini(1);
            wc_load_config_from_ini();
            g_wifi.retry_count = 0;
            if (g_wifi.ssid[0] == '\0') {
                wc_set_phase(WLAN_PHASE_NO_CONFIG);
            } else {
                wc_set_phase(WLAN_PHASE_DISCONNECTED);
            }
            continue;
        }

        if (g_wc_request_disconnect) {
            g_wc_request_disconnect = 0;
            WC_LOG("User request: disconnect");
            wlan_manager_disconnect();
            g_wifi.ip[0] = '\0';
            g_wifi.retry_count = 0;
            if (!g_wifi.enabled) {
                wc_set_phase(WLAN_PHASE_DISCONNECTED);
            } else if (g_wifi.ssid[0] == '\0') {
                wc_set_phase(WLAN_PHASE_NO_CONFIG);
            } else {
                wc_set_phase(WLAN_PHASE_DISCONNECTED);
            }
            wc_sleep_interruptible(200);
            continue;
        }

        if (g_wc_request_retry) {
            g_wc_request_retry = 0;
            WC_LOG("User request: retry");
            g_wifi.retry_count = 0;
            wlan_manager_disconnect();
            wc_sleep_interruptible(200);
            /* 重新读 INI (可能用户在 settings 改了) */
            wc_load_config_from_ini();
            if (!g_wifi.enabled) {
                wc_set_phase(WLAN_PHASE_DISCONNECTED);
            } else if (g_wifi.ssid[0] == '\0') {
                wc_set_phase(WLAN_PHASE_NO_CONFIG);
            } else {
                wc_set_phase(WLAN_PHASE_DISCONNECTED);
            }
            continue;
        }

        switch (g_wifi.phase) {
        case WLAN_PHASE_NO_CONFIG:
            WC_TRACE_LOG("[TRACE] branch=NO_CONFIG, sleeping 1s");
            wc_sleep_interruptible(1000);
            wc_load_config_from_ini();
            if (!g_wifi.enabled) {
                wc_set_phase(WLAN_PHASE_DISCONNECTED);
            } else if (g_wifi.ssid[0] != '\0') {
                WC_LOG("SSID appeared in INI, switching to DISCONNECTED");
                g_wifi.retry_count = 0;
                wc_set_phase(WLAN_PHASE_DISCONNECTED);
            }
            break;

        case WLAN_PHASE_DISCONNECTED:
            WC_TRACE_LOG("[TRACE] branch=DISCONNECTED");
            if (!g_wifi.enabled) {
                wc_sleep_interruptible(1000);
                break;
            }
            if (g_wifi.ssid[0] == '\0') {
                wc_set_phase(WLAN_PHASE_NO_CONFIG);
                break;
            }
            /* fm_paused 在循环顶部统一静默处理 */
            if (g_wifi.retry_count >= (int)BACKOFF_COUNT) {
                WC_LOG("Max retries exhausted, staying DISCONNECTED. User must tap to retry.");
                /* 停在这, 每 60s 打印一次, 等用户主动 retry */
                wc_sleep_interruptible(60000);
                break;
            }
            /* 退避 */
            {
                uint32_t backoff = kBackoffMs[g_wifi.retry_count];
                WC_LOG("Backoff %ums before retry %d", backoff, g_wifi.retry_count);
                wc_sleep_interruptible(backoff);
                if (!g_wc_running || wc_has_pending_request() || !g_wifi.enabled ||
                    g_wifi.fm_paused) {
                    break;
                }
            }
            wc_try_connect();
            break;

        case WLAN_PHASE_CONNECTING:
            if (g_wifi.fm_paused) {
                wlan_manager_cancel_connect();
                wlan_manager_disconnect();
                wc_set_phase(WLAN_PHASE_DISCONNECTED);
                break;
            }
            WC_TRACE_LOG("[TRACE] branch=CONNECTING, sleeping 1s");
            /* wait_for_ip 在 wc_try_connect 内部阻塞, 这里不应该停留 */
            wc_sleep_interruptible(1000);
            break;

        case WLAN_PHASE_CONNECTED:
            if (g_wifi.fm_paused) {
                wlan_manager_disconnect();
                g_wifi.ip[0] = '\0';
                wc_set_phase(WLAN_PHASE_DISCONNECTED);
                break;
            }
            WC_TRACE_LOG("[TRACE] branch=CONNECTED, polling link every 100ms (stop-responsive)");
            /* 监控 link: 每 5s 检查一次，但以 100ms 粒度轮询以便快速响应停止请求。
             * XR872 修复: 原来用 OS_MSleep(5000) 会卡 5 秒不检测 g_wc_running，
             *   导致休眠时 wifi_controller_stop() 超时，wlan 驱动在 disconnect
             *   期间被本线程访问，触发 BUG at wsm_remove_key_request:1027 断言。 */
            for (int i = 0; i < (POLL_INTERVAL_MS / 100) && g_wc_running; i++) {
                if (g_wifi.fm_paused) {
                    break;
                }
                OS_MSleep(100);
            }
            if (!g_wc_running) {
                WC_TRACE_LOG("[TRACE] stop requested during CONNECTED poll, exiting loop");
                break;
            }
            WC_TRACE_LOG("[TRACE] CONNECTED woke, calling is_connected");
            if (!wlan_manager_is_connected()) {
                WC_TRACE_LOG("[TRACE] link lost");
                wc_handle_link_lost();
            } else {
                WC_TRACE_LOG("[TRACE] link ok, checking IP");
                /* 更新 IP (DHCP 可能续约) */
                WLAN_IPInfo_t info;
                if (wlan_manager_get_ip_info(&info) == 0) {
                    if (strcmp(g_wifi.ip, info.ip) != 0) {
                        strncpy(g_wifi.ip, info.ip, sizeof(g_wifi.ip) - 1);
                        g_wifi.ip[sizeof(g_wifi.ip) - 1] = '\0';
                        WC_LOG("IP changed: %s", g_wifi.ip);
                    }
                }
                WC_TRACE_LOG("[TRACE] CONNECTED branch end");
            }
            break;
        }
    }

    WC_LOG("Controller task exiting");
    g_wc_exited = 1;
    OS_ThreadDelete(&g_wc_thread);
}

void wifi_controller_init(void)
{
    memset(&g_wifi, 0, sizeof(g_wifi));
    g_wifi.enabled = 1;
    g_wifi.phase = WLAN_PHASE_NO_CONFIG;
    g_wifi.retry_count = 0;
    g_wifi.http_running = 0;
}

void wifi_controller_start(void)
{
    if (OS_ThreadIsValid(&g_wc_thread)) {
        return;
    }
    if (OS_ThreadCreate(&g_wc_thread, "wifi_ctrl", wc_task, NULL,
                        OS_PRIORITY_NORMAL, 4096) != 0) {
        WC_LOG("Failed to create controller thread");
    }
}

/* XR872 修复: 休眠前必须停止控制器线程
 * 现象: 用户在 WiFi 连接中途 (scan/assoc/dhcp) 按 PA6 触发休眠,
 *   screensaver 在活跃的 net 子系统上做 teardown ->
 *   net_sys_onoff poweroff 崩溃 (PC=0x00200024, UFSR=0x1 UNDEFINSTR)。
 * 修复: 休眠前先取消连接 + 停控制器线程 + 等 net 栈沉淀, 再进 hibernation。 */
void wifi_controller_stop(void)
{
    WC_LOG("Stopping controller (for hibernation)");

    /* 1. 取消进行中的连接 (让 wait_for_ip 轮询退出) */
    wlan_manager_cancel_connect();

    /* 2. 让 wc_task 主循环退出 */
    g_wc_running = 0;

    /* 3. 等线程真正退出 (wc_task 在 OS_MSleep 间检测 g_wc_running,
     *    最坏情况卡在 wait_for_ip 的轮询, 被 cancel_connect 解开) */
    int wait_ms = 0;
    while (!g_wc_exited && wait_ms < 2000) {
        OS_MSleep(20);
        wait_ms += 20;
    }
    if (wait_ms >= 2000) {
        WC_LOG("WARN: controller thread did not exit within 2s");
    } else {
        WC_LOG("Controller thread exited (waited %dms)", wait_ms);
    }
}

void wifi_controller_register_cb(WLAN_PhaseCb_t cb, void *user_data)
{
    wlan_manager_set_phase_callback(cb, user_data);
}

void wifi_controller_request_retry(void)
{
    g_wc_request_retry = 1;
}

void wifi_controller_request_enable(void)
{
    g_wc_request_enable = 1;
}

void wifi_controller_request_disable(void)
{
    g_wc_request_disable = 1;
}

void wifi_controller_request_disconnect(void)
{
    g_wc_request_disconnect = 1;
}

void wifi_controller_set_http_running(int running)
{
    g_wifi.http_running = running;
    /* 如果刚断开 HTTP, controller 在 CONNECTED 状态下不会主动断 WiFi;
     * 如果刚启动 HTTP, 什么都不做. */
}

void wifi_controller_poll(void)
{
    wlan_manager_poll();
}
