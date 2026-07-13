/*
 * HTTP Server - 简易HTTP服务器模块
 * 
 * 功能：
 * 1. 建立HTTP服务器监听端口
 * 2. 处理GET请求 - 返回文件列表HTML页面
 * 3. 处理POST请求 - 处理文件上传
 */

#ifndef __HTTP_SERVER_H__
#define __HTTP_SERVER_H__

#include <stdint.h>

/* 默认端口 */
#define HTTP_SERVER_PORT 80

/* 最大上传文件大小 (字节) */
#define HTTP_MAX_UPLOAD_SIZE (2 * 1024 * 1024)  /* 2MB */

/* MIME类型 */
typedef struct {
    const char *ext;
    const char *mime_type;
} HTTP_MIME_Type;

/*
 * 初始化HTTP服务器
 * @param port 监听端口
 * @return 0成功, 其他失败
 */
int http_server_init(int port);

/*
 * 开机预创建工作线程（须在 font warm 之前调用，避免 SRAM 堆耗尽）
 * @return 0成功, 其他失败
 */
int http_server_reserve_thread(void);

/*
 * 启动HTTP服务器
 * @return 0成功, 其他失败
 */
int http_server_start(void);

/*
 * 停止HTTP服务器
 */
void http_server_request_stop(void);

/*
 * 停止HTTP服务器并等待工作线程退出（开书等路径用；UI 请用 request_stop）
 */
void http_server_stop(void);

/*
 * 获取服务器运行状态
 * @return 1运行中, 0未运行
 */
int http_server_is_running(void);

/*
 * 获取服务器绑定的端口
 * @return 端口号
 */
int http_server_get_port(void);

#endif /* __HTTP_SERVER_H__ */
