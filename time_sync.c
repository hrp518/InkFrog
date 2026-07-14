/*
 * time_sync - WiFi 连通后从国家授时中心取时，供首页显示
 */

#include "time_sync.h"
#include "net/sntp/sntp.h"
#include "kernel/os/os.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

/* 国家授时中心 NTP；备用国内公共源 */
#define NTSC_NTP_SERVER   "ntp.ntsc.ac.cn"
#define BACKUP_NTP_SERVER "ntp.aliyun.com"
#define CST_OFFSET_SEC    (8 * 3600)

static char s_time_text[16];
static volatile int s_time_pending;
static volatile int s_time_valid;
static volatile int s_syncing;

static void time_sync_format_from_epoch(time_t epoch_utc, char *buf, int buf_size)
{
    time_t local = epoch_utc + CST_OFFSET_SEC;
    struct tm *tm = gmtime(&local); /* 已加 CST，用 gmtime 避免再叠本地 TZ */

    if (!tm || buf_size < 6) {
        if (buf && buf_size > 0) {
            buf[0] = '\0';
        }
        return;
    }
    snprintf(buf, (size_t)buf_size, "%02d:%02d", tm->tm_hour, tm->tm_min);
}

int time_sync_is_valid(void)
{
    return s_time_valid;
}

const char *time_sync_get_text(void)
{
    return s_time_text;
}

int time_sync_take_pending(void)
{
    if (!s_time_pending) {
        return 0;
    }
    s_time_pending = 0;
    return 1;
}

/* 按系统时钟刷新 HH:MM 文本（不访问网络）；有变化返回 1 */
int time_sync_refresh_local(void)
{
    struct timeval tv;
    char buf[16];

    if (!s_time_valid) {
        return 0;
    }
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    time_sync_format_from_epoch(tv.tv_sec, buf, (int)sizeof(buf));
    if (strcmp(buf, s_time_text) == 0) {
        return 0;
    }
    strncpy(s_time_text, buf, sizeof(s_time_text) - 1);
    s_time_text[sizeof(s_time_text) - 1] = '\0';
    s_time_pending = 1;
    return 1;
}

int time_sync_from_ntsc(void)
{
    sntp_arg arg;
    struct timeval ntp_time;
    int ret;

    if (s_syncing) {
        return -1;
    }
    s_syncing = 1;

    printf("[TIME] NTP sync via %s (CST+8)\r\n", NTSC_NTP_SERVER);

    (void)sntp_set_server(0, NTSC_NTP_SERVER);
    (void)sntp_set_server(1, BACKUP_NTP_SERVER);

    memset(&arg, 0, sizeof(arg));
    arg.server_name = NTSC_NTP_SERVER;
    arg.recv_timeout = 3000;
    arg.retry_times = 2;

    ret = sntp_get_time(&arg, &ntp_time);
    if (ret != 0) {
        printf("[TIME] primary failed (%d), try backup\r\n", ret);
        arg.server_name = BACKUP_NTP_SERVER;
        ret = sntp_get_time(&arg, &ntp_time);
    }

    if (ret != 0) {
        printf("[TIME] NTP failed: %d\r\n", ret);
        s_syncing = 0;
        return -1;
    }

    /* 写入系统时间（UTC）；显示时再加 CST */
    if (settimeofday(&ntp_time, NULL) != 0) {
        printf("[TIME] settimeofday failed\r\n");
    }

    time_sync_format_from_epoch(ntp_time.tv_sec, s_time_text, (int)sizeof(s_time_text));
    s_time_valid = 1;
    s_time_pending = 1;
    s_syncing = 0;

    printf("[TIME] synced: %s (CST)\r\n", s_time_text);
    return 0;
}
