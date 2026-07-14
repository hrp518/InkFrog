#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

/* 阻塞：在 WiFi 已连通的线程里调用（如 wifi_ctrl），国家授时中心 NTP */
int time_sync_from_ntsc(void);

int time_sync_is_valid(void);
const char *time_sync_get_text(void);

/* LVGL 线程：若有待刷新文本返回 1（已清 pending） */
int time_sync_take_pending(void);

/* 按本地系统钟刷新 HH:MM；文本变化返回 1 并置 pending */
int time_sync_refresh_local(void);

#ifdef __cplusplus
}
#endif

#endif /* TIME_SYNC_H */
