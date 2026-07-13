/*
 * HTTP Server - 简易HTTP服务器实现
 *
 * 修复记录：
 * 2026-03-24: 增加LWIP TCP接收缓冲区的PSRAM优化，
 *             解决大文件上传时heap exhausted问题
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "http_server.h"
#include "http_l1glyf_js.h"
#include "screensaver.h"
#include "epd.h"
#include "fs/fatfs/ff.h"
#include "common/framework/fs_ctrl.h"
#include "kernel/os/os.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "sys/dma_heap.h"  // 添加PSRAM堆支持
#include "heap_debug.h"

/* 调试开关 */
#define HTTP_DEBUG 0
#if HTTP_DEBUG
#define HTTP_LOG(fmt, ...) printf("[HTTP] " fmt "\r\n", ##__VA_ARGS__)
#else
#define HTTP_LOG(fmt, ...)
#endif

/* 常量定义 - 优化内存使用 */
#define HTTP_BUF_SIZE 2048
#define HTTP_MAX_PATH 256
#define HTTP_RESPONSE_SIZE (16 * 1024)  /* PSRAM HTML 缓冲；文件列表过长时截断 */
#define HTTP_THREAD_STACK_SIZE 4096     /* 开机预创建线程栈（SRAM），font warm 后无法再 alloc */
#define HTTP_UPLOAD_IO_SIZE   (32 * 1024)
#define HTTP_UPLOAD_CHUNK     4096
#define HTTP_WORKSPACE_SIZE   (HTTP_BUF_SIZE + HTTP_RESPONSE_SIZE)
/* 浏览连接用小缓冲省 SRAM；上传单独调大 */
#define HTTP_SOCK_RCVBUF      1024
#define HTTP_SOCK_SNDBUF      2048
#define HTTP_UPLOAD_SOCK_RCVBUF 8192
#define HTTP_UPLOAD_SOCK_SNDBUF 4096
#define HTTP_UPLOAD_RECV_SEC    3       /* 单次 recv 超时；stop 时最多等几秒 */
#define HTTP_ACCEPT_POLL_MS     50
#define HTTP_STOP_WAIT_MS       50
#define HTTP_STOP_WAIT_MAX      40      /* 50ms * 40 = 2s */
#define HTTP_MIN_SRAM_FREE    2048

/* 上传 I/O 缓冲：静态 PSRAM，避免每次上传 _dma_malloc */
static uint8_t g_upload_io_buf[HTTP_UPLOAD_IO_SIZE] __attribute__((section(".psram_bss")));
/* HTTP 工作区：静态 PSRAM，start/stop 不再 _dma_malloc */
static char g_http_workspace[HTTP_WORKSPACE_SIZE] __attribute__((section(".psram_bss")));

/*
 * PSRAM接收缓冲区 - 用于大文件上传
 * 使用PSRAM可以避免消耗宝贵的内部SRAM堆内存
 * 注意：当启用PSRAM时，handle_file_upload_streaming函数内部会使用
 * 静态局部变量psram_buffer[BUFFER_SIZE]，放在.psram_data段
 */

/* 上传文件保存路径 */
#define UPLOAD_PATH "0:/Font/"

/* 全局变量 */
static OS_Thread_t g_http_thread;
static volatile int g_http_running = 0;
static int g_http_port = HTTP_SERVER_PORT;
static int g_server_sock = -1;           /* 服务器socket，用于stop时shutdown */
static volatile int g_client_sock = -1;  /* 客户端socket，用于stop时shutdown中断recv */
static char *g_http_buffer = g_http_workspace;
static char *g_http_response = g_http_workspace + HTTP_BUF_SIZE;
static volatile int g_http_serving = 0;  /* 1=正在 accept 服务中 */

static int http_server_thread_active(void)
{
    return OS_ThreadIsValid(&g_http_thread);
}

/* MIME类型表 */
static const HTTP_MIME_Type g_mime_types[] = {
    {".html", "text/html"},
    {".htm", "text/html"},
    {".css", "text/css"},
    {".js", "application/javascript"},
    {".json", "application/json"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".gif", "image/gif"},
    {".ico", "image/x-icon"},
    {".txt", "text/plain"},
    {".c", "text/plain"},
    {".h", "text/plain"},
    {NULL, "application/octet-stream"}
};

static int send_response(int sock, const char *status, const char *content_type,
                        const char *body, int body_len);
static int send_html_response(int sock, const char *html_body);
static int parse_api_upload_params(const char *path, char *out_path, size_t path_sz,
                                   char *out_name, size_t name_sz);
static int parse_request_line_path(const char *req, char *path_out, size_t path_sz);
static int handle_api_upload_raw(int sock, const char *header_buf, int header_recv_len,
                                 const char *upload_path, const char *filename);

static void http_tune_client_socket(int sock)
{
    int v;
    struct timeval recv_tv = {2, 0};

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));
    v = HTTP_SOCK_RCVBUF;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v));
    v = HTTP_SOCK_SNDBUF;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v));
}

static void http_tune_upload_socket(int sock)
{
    int v;
    struct timeval recv_tv = { HTTP_UPLOAD_RECV_SEC, 0 };

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));
    v = HTTP_UPLOAD_SOCK_RCVBUF;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v));
    v = HTTP_UPLOAD_SOCK_SNDBUF;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v));
}

static void http_set_nonblock(int sock, int on)
{
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        return;
    }
    if (on) {
        (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    } else {
        (void)fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
    }
}

static int http_accept_client(int server_sock, struct sockaddr_in *client_addr, socklen_t *addr_len)
{
    int client_sock;

    for (;;) {
        client_sock = accept(server_sock, (struct sockaddr *)client_addr, addr_len);
        if (client_sock >= 0) {
            return client_sock;
        }
        if (!g_http_running) {
            return -1;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            OS_MSleep(HTTP_ACCEPT_POLL_MS);
            continue;
        }
        return -1;
    }
}

static int http_upload_recv_would_retry(void)
{
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT);
}

static void http_drain_extra_clients(int server_sock)
{
    int flags = fcntl(server_sock, F_GETFL, 0);
    int extra;

    if (flags < 0) {
        return;
    }
    (void)fcntl(server_sock, F_SETFL, flags | O_NONBLOCK);
    for (;;) {
        struct sockaddr_in ca;
        socklen_t len = sizeof(ca);
        extra = accept(server_sock, (struct sockaddr *)&ca, &len);
        if (extra < 0) {
            break;
        }
        shutdown(extra, SHUT_RDWR);
        closesocket(extra);
    }
    (void)fcntl(server_sock, F_SETFL, flags);
}

static int parse_content_length(const char *req)
{
    const char *p = strstr(req, "Content-Length:");
    if (!p) {
        return -1;
    }
    p += strlen("Content-Length:");
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return atoi(p);
}

static int generate_screensaver_html(char *html_buf, int buf_size)
{
    int len = 0;
    len += snprintf(html_buf + len, buf_size - len,
        "<!DOCTYPE html>\n"
        "<html><head><meta charset=\"UTF-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        "<title>屏保编辑器</title>\n"
        "<style>\n"
        "body{font-family:Arial,sans-serif;margin:0;background:#eef2f7;color:#222;}\n"
        ".wrap{max-width:1180px;margin:0 auto;padding:20px;}\n"
        ".card{background:#fff;border-radius:12px;box-shadow:0 3px 12px rgba(0,0,0,.08);padding:16px;margin-bottom:16px;}\n"
        ".row{display:flex;gap:16px;flex-wrap:wrap;}\n"
        ".left{flex:1;min-width:320px;}\n"
        ".right{width:340px;max-width:100%%;}\n"
        ".toolbar label{display:block;font-size:13px;color:#555;margin-top:10px;margin-bottom:4px;}\n"
        ".toolbar input,.toolbar select,.toolbar button{width:100%%;box-sizing:border-box;padding:10px;border:1px solid #ccd3db;border-radius:8px;}\n"
        ".toolbar button{background:#2563eb;color:#fff;border:none;cursor:pointer;font-weight:bold;}\n"
        ".toolbar button.secondary{background:#64748b;}\n"
        ".toolbar button.success{background:#16a34a;}\n"
        ".toolbar button:disabled{background:#94a3b8;cursor:not-allowed;}\n"
        ".preview-box{display:flex;justify-content:center;align-items:center;overflow:auto;background:#dfe6ee;border-radius:12px;padding:12px;}\n"
        "canvas{background:#fff;border:1px solid #cbd5e1;border-radius:8px;image-rendering:pixelated;max-width:100%%;}\n"
        ".hint{font-size:12px;color:#64748b;line-height:1.6;}\n"
        ".top-actions{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:14px;}\n"
        ".top-actions a{display:inline-block;padding:10px 14px;background:#475569;color:#fff;text-decoration:none;border-radius:8px;}\n"
        ".status{font-size:13px;padding:10px;border-radius:8px;background:#f8fafc;color:#334155;white-space:pre-wrap;}\n"
        ".small{font-size:12px;color:#6b7280;}\n"
        "</style></head><body><div class=\"wrap\">\n"
        "<div class=\"top-actions\"><a href=\"/\">返回文件管理</a><a href=\"/screensaver/status\" target=\"_blank\">查看状态 JSON</a></div>\n"
        "<div class=\"row\">\n"
        "<div class=\"left card\">\n"
        "<h2>屏保图片编辑器</h2>\n"
        "<div class=\"preview-box\"><canvas id=\"preview\" width=\"240\" height=\"415\"></canvas></div>\n"
        "<p class=\"hint\">支持拖动平移、滚轮缩放。最终输出固定为 240x415、1bpp、%u 字节。白色像素写 1，黑色像素写 0，与当前 EPD 帧缓冲格式一致。</p>\n"
        "</div>\n"
        "<div class=\"right card toolbar\">\n"
        "<label>选择图片</label><input type=\"file\" id=\"fileInput\" accept=\"image/*\">\n"
        "<label>布局模式</label><select id=\"fitMode\"><option value=\"contain\">Contain 完整显示</option><option value=\"cover\">Cover 铺满裁切</option><option value=\"stretch\">Stretch 拉伸</option><option value=\"manual\">Manual 手动</option></select>\n"
        "<label>缩放倍数</label><input type=\"range\" id=\"zoomRange\" min=\"0.1\" max=\"8\" step=\"0.01\" value=\"1\">\n"
        "<label>X 偏移</label><input type=\"number\" id=\"offsetX\" value=\"0\" step=\"1\">\n"
        "<label>Y 偏移</label><input type=\"number\" id=\"offsetY\" value=\"0\" step=\"1\">\n"
        "<label>处理模式</label><select id=\"procMode\"><option value=\"dither\">Floyd-Steinberg 抖动</option><option value=\"level\">Level 阈值</option></select>\n"
        "<label>阈值（Level模式）</label><input type=\"range\" id=\"threshold\" min=\"0\" max=\"255\" step=\"1\" value=\"128\">\n"
        "<div class=\"row\">\n"
        "<div style=\"flex:1\"><button type=\"button\" class=\"secondary\" id=\"btnContain\">一键完整显示</button></div>\n"
        "<div style=\"flex:1\"><button type=\"button\" class=\"secondary\" id=\"btnCover\">一键铺满</button></div>\n"
        "</div>\n"
        "<div class=\"row\">\n"
        "<div style=\"flex:1\"><button type=\"button\" class=\"secondary\" id=\"btnReset\">重置</button></div>\n"
        "<div style=\"flex:1\"><button type=\"button\" class=\"success\" id=\"btnSave\">保存为屏保</button></div>\n"
        "</div>\n"
        "<label>设备状态</label><div id=\"status\" class=\"status\">正在读取状态...</div>\n"
        "<p class=\"small\">说明：浏览器端完成缩放、位移、灰度/抖动与 1bpp 打包；设备端只负责接收并保存结果到 SD 卡。</p>\n"
        "</div></div>\n"
        "<script>\n"
        "const W=240,H=415,EXPECTED=%u;\n"
        "const canvas=document.getElementById('preview');const ctx=canvas.getContext('2d');\n"
        "const work=document.createElement('canvas');work.width=W;work.height=H;const wctx=work.getContext('2d');\n"
        "const fileInput=document.getElementById('fileInput');const fitMode=document.getElementById('fitMode');\n"
        "const zoomRange=document.getElementById('zoomRange');const offsetX=document.getElementById('offsetX');const offsetY=document.getElementById('offsetY');\n"
        "const procMode=document.getElementById('procMode');const threshold=document.getElementById('threshold');const statusEl=document.getElementById('status');\n"
        "let img=null,dragging=false,lastX=0,lastY=0,state={scale:1,dx:0,dy:0};\n"
        "function syncInputs(){zoomRange.value=state.scale;offsetX.value=Math.round(state.dx);offsetY.value=Math.round(state.dy);}\n"
        "function setStatus(t){statusEl.textContent=t;}\n"
        "function loadStatus(){fetch('/screensaver/status').then(r=>r.json()).then(j=>{setStatus(JSON.stringify(j,null,2));}).catch(e=>setStatus('读取状态失败: '+e));}\n"
        "function grayOf(r,g,b){return Math.round(0.299*r+0.587*g+0.114*b);}\n"
        "function applyLevel(imgData,th){const d=imgData.data;for(let i=0;i<d.length;i+=4){const g=grayOf(d[i],d[i+1],d[i+2]);const v=g>th?255:0;d[i]=d[i+1]=d[i+2]=v;d[i+3]=255;}return imgData;}\n"
        "function applyDither(imgData){const d=imgData.data;const gs=new Float32Array(W*H);for(let y=0;y<H;y++){for(let x=0;x<W;x++){const i=(y*W+x)*4;gs[y*W+x]=grayOf(d[i],d[i+1],d[i+2]);}}for(let y=0;y<H;y++){for(let x=0;x<W;x++){const idx=y*W+x;const oldv=gs[idx];const newv=oldv>127?255:0;const err=oldv-newv;gs[idx]=newv;if(x+1<W)gs[idx+1]+=err*7/16;if(y+1<H){if(x>0)gs[idx+W-1]+=err*3/16;gs[idx+W]+=err*5/16;if(x+1<W)gs[idx+W+1]+=err*1/16;}}}for(let y=0;y<H;y++){for(let x=0;x<W;x++){const i=(y*W+x)*4;const v=gs[y*W+x]>127?255:0;d[i]=d[i+1]=d[i+2]=v;d[i+3]=255;}}return imgData;}\n"
        "function pack1bpp(imgData){const d=imgData.data;const out=new Uint8Array(EXPECTED);for(let y=0;y<H;y++){for(let x=0;x<W;x++){const i=(y*W+x)*4;const white=d[i]>127?1:0;const bi=y*(W>>3)+(x>>3);out[bi]|=(white<<(7-(x&7)));}}return out;}\n"
        "function fitContain(){if(!img)return;state.scale=Math.min(W/img.width,H/img.height);state.dx=(W-img.width*state.scale)/2;state.dy=(H-img.height*state.scale)/2;fitMode.value='contain';syncInputs();render();}\n"
        "function fitCover(){if(!img)return;state.scale=Math.max(W/img.width,H/img.height);state.dx=(W-img.width*state.scale)/2;state.dy=(H-img.height*state.scale)/2;fitMode.value='cover';syncInputs();render();}\n"
        "function fitStretch(){if(!img)return;state.scale=1;state.dx=0;state.dy=0;fitMode.value='stretch';syncInputs();render();}\n"
        "function render(){ctx.fillStyle='#fff';ctx.fillRect(0,0,W,H);wctx.fillStyle='#fff';wctx.fillRect(0,0,W,H);if(!img){ctx.fillStyle='#888';ctx.font='16px Arial';ctx.fillText('请选择图片',75,200);return;}const mode=fitMode.value;wctx.save();if(mode==='stretch'){wctx.drawImage(img,0,0,W,H);}else{wctx.imageSmoothingEnabled=true;wctx.drawImage(img,state.dx,state.dy,img.width*state.scale,img.height*state.scale);}wctx.restore();let id=wctx.getImageData(0,0,W,H);if(procMode.value==='level'){id=applyLevel(id,parseInt(threshold.value,10));}else{id=applyDither(id);}wctx.putImageData(id,0,0);ctx.drawImage(work,0,0);}\n"
        "fileInput.addEventListener('change',()=>{const f=fileInput.files[0];if(!f)return;const rd=new FileReader();rd.onload=e=>{const im=new Image();im.onload=()=>{img=im;fitContain();};im.src=e.target.result;};rd.readAsDataURL(f);});\n"
        "fitMode.addEventListener('change',()=>{if(!img)return;if(fitMode.value==='contain')fitContain();else if(fitMode.value==='cover')fitCover();else if(fitMode.value==='stretch')fitStretch();else render();});\n"
        "zoomRange.addEventListener('input',()=>{state.scale=parseFloat(zoomRange.value);fitMode.value='manual';render();});\n"
        "offsetX.addEventListener('input',()=>{state.dx=parseFloat(offsetX.value)||0;fitMode.value='manual';render();});\n"
        "offsetY.addEventListener('input',()=>{state.dy=parseFloat(offsetY.value)||0;fitMode.value='manual';render();});\n"
        "procMode.addEventListener('change',render);threshold.addEventListener('input',render);\n"
        "document.getElementById('btnContain').addEventListener('click',fitContain);document.getElementById('btnCover').addEventListener('click',fitCover);\n"
        "document.getElementById('btnReset').addEventListener('click',()=>{if(!img)return;fitContain();threshold.value=128;procMode.value='dither';render();});\n"
        "canvas.addEventListener('mousedown',e=>{dragging=true;lastX=e.offsetX;lastY=e.offsetY;fitMode.value='manual';});\n"
        "window.addEventListener('mouseup',()=>dragging=false);\n"
        "canvas.addEventListener('mousemove',e=>{if(!dragging||!img||fitMode.value==='stretch')return;state.dx+=e.offsetX-lastX;state.dy+=e.offsetY-lastY;lastX=e.offsetX;lastY=e.offsetY;syncInputs();render();});\n"
        "canvas.addEventListener('wheel',e=>{if(!img||fitMode.value==='stretch')return;e.preventDefault();fitMode.value='manual';const old=state.scale;let next=old*(e.deltaY<0?1.05:0.95);if(next<0.1)next=0.1;if(next>8)next=8;const mx=e.offsetX,my=e.offsetY;state.dx=mx-(mx-state.dx)*(next/old);state.dy=my-(my-state.dy)*(next/old);state.scale=next;syncInputs();render();},{passive:false});\n"
        "document.getElementById('btnSave').addEventListener('click',()=>{if(!img){alert('请先选择图片');return;}const btn=document.getElementById('btnSave');btn.disabled=true;btn.textContent='保存中...';const imgData=wctx.getImageData(0,0,W,H);const packed=pack1bpp(imgData);const xhr=new XMLHttpRequest();xhr.open('POST','/screensaver/upload',true);xhr.setRequestHeader('Content-Type','application/octet-stream');xhr.onload=()=>{btn.disabled=false;btn.textContent='保存为屏保';if(xhr.status===200){alert('保存成功');loadStatus();}else{alert('保存失败: '+xhr.status+' '+xhr.responseText);}};xhr.onerror=()=>{btn.disabled=false;btn.textContent='保存为屏保';alert('网络错误');};xhr.send(packed.buffer);});\n"
        "syncInputs();render();loadStatus();\n"
        "</script></div></body></html>\n",
        (unsigned int)EPD_BUFFER_SIZE,
        (unsigned int)EPD_BUFFER_SIZE);
    return len;
}

static int handle_screensaver_upload_raw(int sock, const char *initial_data, int initial_len)
{
    char *body;
    char *buf = NULL;
    int body_len;
    int content_len;
    int received;
    int ret;

    body = strstr(initial_data, "\r\n\r\n");
    if (!body) {
        send_response(sock, "400 Bad Request", "text/plain", "Missing body", 12);
        return -1;
    }

    body += 4;
    body_len = initial_len - (body - initial_data);
    content_len = parse_content_length(initial_data);
    if (content_len != EPD_BUFFER_SIZE) {
        HTTP_LOG("screensaver upload invalid length: %d", content_len);
        send_response(sock, "400 Bad Request", "text/plain", "Invalid content length", 22);
        return -1;
    }

    buf = (char *)_dma_malloc(EPD_BUFFER_SIZE, DMAHEAP_PSRAM);
    if (!buf) {
        send_response(sock, "500 Internal Server Error", "text/plain", "No memory", 9);
        return -1;
    }

    received = 0;
    if (body_len > 0) {
        if (body_len > EPD_BUFFER_SIZE) {
            body_len = EPD_BUFFER_SIZE;
        }
        memcpy(buf, body, body_len);
        received = body_len;
    }

    while (received < EPD_BUFFER_SIZE) {
        ret = recv(sock, buf + received, EPD_BUFFER_SIZE - received, 0);
        if (ret <= 0) {
            HTTP_LOG("screensaver upload recv interrupted: %d", ret);
            _dma_free(buf, 0);
            send_response(sock, "400 Bad Request", "text/plain", "Incomplete body", 15);
            return -1;
        }
        received += ret;
    }

    ret = screensaver_save_raw_file((const uint8_t *)buf, EPD_BUFFER_SIZE);
    _dma_free(buf, 0);
    if (ret == 0) {
        send_response(sock, "200 OK", "text/plain", "Screensaver saved", 17);
        return 0;
    }

    send_response(sock, "500 Internal Server Error", "text/plain", "Save failed", 11);
    return -1;
}

static int http_build_paths(const char *upload_path, const char *filename,
                            char *final_path, char *temp_path, size_t sz)
{
    if (!upload_path || !filename || !final_path || !temp_path) {
        return -1;
    }
    if (strlen(upload_path) + strlen(filename) + 8 >= sz) {
        return -1;
    }
    if (upload_path[strlen(upload_path) - 1] == '/') {
        snprintf(final_path, sz, "%s%s", upload_path, filename);
    } else {
        snprintf(final_path, sz, "%s/%s", upload_path, filename);
    }
    snprintf(temp_path, sz, "%s.part", final_path);
    return 0;
}

static void url_decode(char *dst, const char *src);

static void http_ensure_dir(const char *dir)
{
    FRESULT fr;
    char buf[HTTP_MAX_PATH];
    char acc[HTTP_MAX_PATH];
    const char *p;
    int i;

    if (!dir || !dir[0]) {
        return;
    }

    fr = f_mkdir(dir);
    if (fr == FR_OK || fr == FR_EXIST) {
        return;
    }

    strncpy(buf, dir, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    if (strncmp(buf, "0:", 2) == 0) {
        strcpy(acc, "0:");
        p = buf + 2;
        if (*p == '/') {
            strcat(acc, "/");
            p++;
        }
    } else {
        acc[0] = '\0';
        p = buf;
        if (*p == '/') {
            strcat(acc, "/");
            p++;
        }
    }

    while (*p) {
        char segment[64];

        i = 0;
        while (*p && *p != '/' && i < (int)sizeof(segment) - 1) {
            segment[i++] = *p++;
        }
        segment[i] = '\0';
        if (segment[0]) {
            if (acc[0] && acc[strlen(acc) - 1] != '/') {
                strcat(acc, "/");
            }
            strcat(acc, segment);
            (void)f_mkdir(acc);
        }
        while (*p == '/') {
            p++;
        }
    }
}

static int parse_request_line_path(const char *req, char *path_out, size_t path_sz)
{
    const char *p = req;
    int i;

    while (*p && *p != ' ') {
        p++;
    }
    if (*p != ' ') {
        return -1;
    }
    while (*p == ' ') {
        p++;
    }
    i = 0;
    while (*p && *p != ' ' && *p != '\r' && *p != '\n' && i < (int)path_sz - 1) {
        path_out[i++] = *p++;
    }
    path_out[i] = '\0';
    return (i > 0) ? 0 : -1;
}

static int parse_api_upload_params(const char *path, char *out_path, size_t path_sz,
                                   char *out_name, size_t name_sz)
{
    const char *q = strchr(path, '?');
    const char *p;
    char tmp[HTTP_MAX_PATH];

    if (!q || !out_path || !out_name) {
        return -1;
    }
    out_path[0] = '\0';
    out_name[0] = '\0';

    p = strstr(q, "path=");
    if (p) {
        int i = 0;
        p += 5;
        while (*p && *p != '&' && i < (int)sizeof(tmp) - 1) {
            tmp[i++] = *p++;
        }
        tmp[i] = '\0';
        url_decode(out_path, tmp);
    }

    p = strstr(q, "name=");
    if (p) {
        int i = 0;
        p += 5;
        while (*p && *p != '&' && i < (int)sizeof(tmp) - 1) {
            tmp[i++] = *p++;
        }
        tmp[i] = '\0';
        url_decode(out_name, tmp);
    }

    if (!out_path[0] || !out_name[0]) {
        return -1;
    }
    if (strlen(out_name) >= name_sz - 1) {
        return -1;
    }
    return 0;
}

/*
 * PUT /api/upload?path=...&name=...  原始二进制上传（主路径）
 */
static int handle_api_upload_raw(int sock, const char *header_buf, int header_recv_len,
                                 const char *upload_path, const char *filename)
{
    const char *body;
    char final_path[HTTP_MAX_PATH];
    char temp_path[HTTP_MAX_PATH];
    FIL fp;
    FRESULT fr;
    int content_len;
    int body_len;
    int received;
    int n;
    UINT bw;

    body = strstr(header_buf, "\r\n\r\n");
    if (!body) {
        send_response(sock, "400 Bad Request", "text/plain", "Missing header", 14);
        return -1;
    }
    body += 4;
    body_len = header_recv_len - (int)(body - header_buf);
    content_len = parse_content_length(header_buf);
    if (content_len <= 0) {
        send_response(sock, "400 Bad Request", "text/plain", "Bad length", 10);
        return -1;
    }

    if (http_build_paths(upload_path, filename, final_path, temp_path, sizeof(final_path)) != 0) {
        send_response(sock, "400 Bad Request", "text/plain", "Bad path", 8);
        return -1;
    }

    printf("[HTTP] upload %s (%d B) -> %s\r\n", filename, content_len, final_path);

    http_ensure_dir(upload_path);
    http_tune_upload_socket(sock);

    fr = f_open(&fp, temp_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        printf("[HTTP] upload f_open fail %d %s\r\n", fr, temp_path);
        send_response(sock, "500 Internal Server Error", "text/plain", "Open failed", 11);
        return -1;
    }

    received = 0;
    if (body_len > 0) {
        int chunk = body_len;
        if (chunk > content_len) {
            chunk = content_len;
        }
        fr = f_write(&fp, body, chunk, &bw);
        if (fr != FR_OK || (int)bw != chunk) {
            f_close(&fp);
            f_unlink(temp_path);
            send_response(sock, "500 Internal Server Error", "text/plain", "Write failed", 12);
            return -1;
        }
        received += chunk;
    }

    while (received < content_len && g_http_running) {
        int want = content_len - received;
        if (want > HTTP_UPLOAD_CHUNK) {
            want = HTTP_UPLOAD_CHUNK;
        }
        n = recv(sock, g_upload_io_buf, want, 0);
        if (n < 0) {
            if (!g_http_running) {
                printf("[HTTP] upload aborted (stop) at %d/%d\r\n", received, content_len);
                f_close(&fp);
                f_unlink(temp_path);
                return -1;
            }
            if (http_upload_recv_would_retry()) {
                OS_ThreadYield();
                continue;
            }
            printf("[HTTP] upload recv %d errno=%d at %d/%d\r\n",
                   n, errno, received, content_len);
            f_close(&fp);
            f_unlink(temp_path);
            return -1;
        }
        if (n == 0) {
            printf("[HTTP] upload peer closed at %d/%d\r\n", received, content_len);
            f_close(&fp);
            f_unlink(temp_path);
            return -1;
        }
        fr = f_write(&fp, g_upload_io_buf, n, &bw);
        if (fr != FR_OK || (int)bw != n) {
            printf("[HTTP] upload f_write fail %d bw=%u\r\n", fr, (unsigned)bw);
            f_close(&fp);
            f_unlink(temp_path);
            send_response(sock, "500 Internal Server Error", "text/plain", "Write failed", 12);
            return -1;
        }
        received += n;
        OS_ThreadYield();
    }

    if (received != content_len) {
        f_close(&fp);
        f_unlink(temp_path);
        send_response(sock, "400 Bad Request", "text/plain", "Size mismatch", 13);
        return -1;
    }

    if (f_sync(&fp) != FR_OK) {
        f_close(&fp);
        f_unlink(temp_path);
        send_response(sock, "500 Internal Server Error", "text/plain", "Sync failed", 11);
        return -1;
    }
    f_close(&fp);

    f_unlink(final_path);
    fr = f_rename(temp_path, final_path);
    if (fr != FR_OK) {
        printf("[HTTP] upload rename fail %d\r\n", fr);
        f_unlink(temp_path);
        send_response(sock, "500 Internal Server Error", "text/plain", "Rename failed", 13);
        return -1;
    }

    printf("[HTTP] upload OK %s (%d B)\r\n", final_path, received);
    send_response(sock, "200 OK", "text/plain", "Upload OK", 9);
    return 0;
}

/*
 * 获取MIME类型
 */
static const char* get_mime_type(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (ext) {
        for (int i = 0; g_mime_types[i].ext != NULL; i++) {
            if (strcasecmp(ext, g_mime_types[i].ext) == 0) {
                return g_mime_types[i].mime_type;
            }
        }
    }
    return "application/octet-stream";
}

/*
 * URL解码
 */
static void url_decode(char *dst, const char *src)
{
    char a, b;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            a = src[1];
            b = src[2];
            if (a >= 'a') a = a - 'a' + 10;
            else if (a >= 'A') a = a - 'A' + 10;
            else a = a - '0';
            if (b >= 'a') b = b - 'a' + 10;
            else if (b >= 'A') b = b - 'A' + 10;
            else b = b - '0';
            *dst++ = (char)(a * 16 + b);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/*
 * 生成文件列表HTML页面
 * path: 要浏览的目录路径，如"/"或"/Font"
 */
static int generate_file_list_html(char *html_buf, int buf_size, const char *dir_path)
{
    FRESULT res;
    DIR dir;
    FILINFO fno;
    int len = 0;
    int total_files = 0;
    int total_dirs = 0;
    char parent_path[HTTP_MAX_PATH] = {0};
    int font_dir = (strcmp(dir_path, "/Font") == 0 || strcmp(dir_path, "0:/Font") == 0);
    const char * script_tag = font_dir ? "<script src=\"/l1glyf.js\"></script>\n" : "";
    const char * font_hint = font_dir
        ? "<br><strong>Font:</strong> 选择 .ttf 后将自动：浏览器生成 .l1glyf → 上传缓存 → 上传字体（无需单独页面）。"
        : "";
    
    /* 计算父目录路径 */
    if (strcmp(dir_path, "/") != 0) {
        /* 获取父目录 */
        const char *last_slash = strrchr(dir_path, '/');
        if (last_slash && last_slash != dir_path) {
            int parent_len = last_slash - dir_path;
            strncpy(parent_path, dir_path, parent_len);
            parent_path[parent_len] = '\0';
        } else {
            strcpy(parent_path, "/");
        }
    }
    
    /* HTML头部 - 支持多文件上传和进度显示 */
    len += snprintf(html_buf + len, buf_size - len,
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "<meta charset=\"UTF-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        "<title>SD卡文件管理器</title>\n"
        "<style>\n"
        "body { font-family: Arial, sans-serif; margin: 20px; background: #f5f5f5; }\n"
        "h1 { color: #333; }\n"
        ".container { max-width: 900px; margin: 0 auto; background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }\n"
        ".file-list { list-style: none; padding: 0; }\n"
        ".file-item { padding: 10px; border-bottom: 1px solid #eee; display: flex; justify-content: space-between; align-items: center; }\n"
        ".file-item:hover { background: #f9f9f9; }\n"
        ".file-icon { margin-right: 10px; }\n"
        ".file-name { flex: 1; }\n"
        ".file-name a { color: #333; text-decoration: none; }\n"
        ".file-name a:hover { color: #007bff; }\n"
        ".file-size { color: #666; font-size: 0.9em; margin-right: 10px; }\n"
        ".dir-icon { color: #f0ad4e; }\n"
        ".file-icon { color: #5bc0de; }\n"
        ".upload-section { margin-top: 20px; padding: 15px; background: #f8f9fa; border-radius: 4px; }\n"
        "input[type=\"file\"] { margin: 10px 0; }\n"
        "input[type=\"submit\"] { background: #4CAF50; color: white; padding: 10px 20px; border: none; border-radius: 4px; cursor: pointer; }\n"
        "input[type=\"submit\"]:hover { background: #45a049; }\n"
        ".breadcrumb { background: #e9ecef; padding: 10px; border-radius: 4px; margin-bottom: 15px; }\n"
        ".breadcrumb a { color: #007bff; text-decoration: none; margin: 0 5px; }\n"
        ".breadcrumb a:hover { text-decoration: underline; }\n"
        ".info { background: #d1ecf1; color: #0c5460; padding: 10px; border-radius: 4px; margin-bottom: 15px; }\n"
        ".btn-back { background: #6c757d; color: white; padding: 8px 16px; border: none; border-radius: 4px; text-decoration: none; display: inline-block; margin-bottom: 15px; }\n"
        ".btn-back:hover { background: #5a6268; }\n"
        "/* 上传进度样式 */\n"
        ".upload-progress { margin-top: 15px; }\n"
        ".upload-item { background: #fff; border: 1px solid #ddd; border-radius: 4px; padding: 10px; margin-bottom: 10px; }\n"
        ".upload-item-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px; }\n"
        ".upload-item-name { font-weight: bold; color: #333; max-width: 300px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }\n"
        ".upload-item-status { font-size: 0.85em; color: #666; }\n"
        ".upload-item-status.pending { color: #f0ad4e; }\n"
        ".upload-item-status.uploading { color: #17a2b8; }\n"
        ".upload-item-status.completed { color: #28a745; }\n"
        ".upload-item-status.error { color: #dc3545; }\n"
        ".progress-bar-bg { width: 100%%; height: 20px; background: #e9ecef; border-radius: 4px; overflow: hidden; }\n"
        ".progress-bar-fill { height: 100%%; background: linear-gradient(90deg, #4CAF50, #45a049); transition: width 0.3s ease; width: 0%%; }\n"
        ".progress-text { text-align: center; font-size: 0.75em; color: #666; margin-top: 2px; }\n"
        ".overall-progress { background: #fff; border: 2px solid #4CAF50; border-radius: 4px; padding: 10px; margin-bottom: 15px; }\n"
        ".overall-header { display: flex; justify-content: space-between; margin-bottom: 5px; }\n"
        ".overall-title { font-weight: bold; color: #333; }\n"
        ".overall-percent { font-weight: bold; color: #4CAF50; }\n"
        ".file-input-wrapper { position: relative; }\n"
        ".file-list-display { margin-top: 10px; max-height: 150px; overflow-y: auto; }\n"
        ".file-chip { display: inline-block; background: #e9ecef; padding: 5px 10px; margin: 3px; border-radius: 3px; font-size: 0.85em; }\n"
        ".file-chip .remove { margin-left: 5px; cursor: pointer; color: #dc3545; }\n"
        ".file-actions { margin-left: 10px; }\n"
        ".file-actions a, .file-actions button { margin-left: 5px; padding: 3px 8px; border: none; border-radius: 3px; cursor: pointer; font-size: 0.85em; text-decoration: none; display: inline-block; }\n"
        ".btn-download { background: #17a2b8; color: white; }\n"
        ".btn-download:hover { background: #138496; }\n"
        ".btn-delete { background: #dc3545; color: white; }\n"
        ".btn-delete:hover { background: #c82333; }\n"
        "</style>\n"
        "%s"
        "</head>\n"
        "<body>\n"
        "<div class=\"container\">\n"
        "<h1>📁 SD卡文件管理器</h1>\n"
        "<div class=\"info\">\n"
        "<strong>当前目录:</strong> %s<br>\n"
        "<strong>说明:</strong> 点击文件夹名称进入目录。支持多文件上传，按顺序依次传输。"
        "%s<br>\n"
        "<a href=\"/screensaver\" style=\"display:inline-block;margin-top:8px;padding:8px 12px;background:#2563eb;color:#fff;text-decoration:none;border-radius:6px;\">🖼️ 打开屏保编辑器</a>\n"
        "</div>\n", script_tag, dir_path, font_hint);
    
    /* 返回上级目录按钮 */
    if (strcmp(dir_path, "/") != 0) {
        len += snprintf(html_buf + len, buf_size - len,
            "<a class=\"btn-back\" href=\"/?path=%s\">⬅️ 返回上级目录</a>\n", parent_path);
    }
    
    /* ================= 开始替换部分 ================= */
    
    /* 1. 渲染上传表单 HTML 部分 */
    len += snprintf(html_buf + len, buf_size - len,
        "<div class=\"upload-section\">\n"
        "<h3>Upload to current dir</h3>\n"
        "<input type=\"hidden\" name=\"path\" value=\"%s\">\n"
        "<div class=\"file-input-wrapper\">\n"
        "<input type=\"file\" id=\"fileInput\" multiple onchange=\"updateFileList()\">\n"
        "<div class=\"file-list-display\" id=\"fileListDisplay\"></div>\n"
        "</div>\n"
        "<button type=\"button\" onclick=\"startUpload()\" id=\"uploadBtn\" style=\"background: #4CAF50; color: white; padding: 10px 20px; border: none; border-radius: 4px; cursor: pointer; margin-top: 10px;\">Start Upload</button>\n"
        "<div class=\"upload-progress\" id=\"uploadProgress\"></div>\n"
        "</div>\n", dir_path);

    /* 2. 渲染 JS 脚本 - 基础函数（含文件删除） */
    len += snprintf(html_buf + len, buf_size - len,
        "<script>\n"
        "var selectedFiles = [];\n"
        "var currentUploadIndex = 0;\n"
        "var isUploading = false;\n"
        "function updateFileList() {\n"
        "    var input = document.getElementById('fileInput');\n"
        "    var display = document.getElementById('fileListDisplay');\n"
        "    selectedFiles = Array.from(input.files);\n"
        "    display.innerHTML = '';\n"
        "    selectedFiles.forEach(function(file, index) {\n"
        "        var chip = document.createElement('span');\n"
        "        chip.className = 'file-chip';\n"
        "        chip.innerHTML = file.name + ' (' + formatSize(file.size) + ')<span class=\"remove\" onclick=\"removeFile(' + index + ')\">x</span>';\n"
        "        display.appendChild(chip);\n"
        "    });\n"
        "    var currentPath = document.querySelector('input[name=\"path\"]').value;\n"
        "    if (!isUploading && isFontDir(currentPath) && selectedFiles.length > 0 &&\n"
        "        selectedFiles.every(function(f){return isTtfFile(f.name);}) && window.L1GlyfBuilder) {\n"
        "        setTimeout(startUpload, 80);\n"
        "    }\n"
        "}\n"
        "function removeFile(index) {\n"
        "    selectedFiles.splice(index, 1);\n"
        "    updateFileList();\n"
        "}\n"
        "function formatSize(bytes) {\n"
        "    if (bytes < 1024) return bytes + ' B';\n"
        "    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';\n"
        "    return (bytes / (1024 * 1024)).toFixed(1) + ' MB';\n"
        "}\n"
        "function deleteFile(filepath) {\n"
        "    if (!confirm('\\u786E\\u5B9A\\u8981\\u5220\\u9664\\u8BE5\\u6587\\u4EF6\\u5417\\uFF1F' + filepath)) return;\n"
        "    fetch('/delete?path=' + encodeURIComponent(filepath), { method: 'POST' })\n"
        "        .then(function(res) { return res.text(); })\n"
        "        .then(function(data) {\n"
        "            alert('\\u5220\\u9664\\u6210\\u529F');\n"
        "            location.reload();\n"
        "        })\n"
        "        .catch(function(err) {\n"
        "            alert('\\u5220\\u9664\\u5931\\u8D25: ' + err);\n"
        "        });\n"
        "}\n");

    /* 3. 渲染 JS 脚本 - 上传控制逻辑（Font 目录 TTF 走 L1GlyfBuilder） */
    len += snprintf(html_buf + len, buf_size - len,
        "function isFontDir(p){return p&&(p==='/Font'||p==='0:/Font');}\n"
        "function isTtfFile(n){return/\\.ttf$/i.test(n);}\n"
        "function setItemProgress(itemId,loaded,total){\n"
        "    var percent=total?Math.round(loaded*100/total):0;\n"
        "    var bar=document.getElementById(itemId+'-bar');\n"
        "    var text=document.getElementById(itemId+'-text');\n"
        "    if(bar){bar.style.width=percent+String.fromCharCode(37);bar.innerHTML=percent+String.fromCharCode(37);}\n"
        "    if(text&&total)text.textContent=formatSize(loaded)+' / '+formatSize(total);}\n"
        "function setItemStatus(itemId,cls,msg){\n"
        "    var s=document.querySelector('#'+itemId+' .upload-item-status');\n"
        "    if(s){s.className='upload-item-status '+cls;s.textContent=msg;}}\n"
        "function uploadOneXHR(file,path,itemId,done){\n"
        "    var url='/api/upload?path='+encodeURIComponent(path)+'&name='+encodeURIComponent(file.name);\n"
        "    var xhr=new XMLHttpRequest();\n"
        "    xhr.open('PUT',url,true);\n"
        "    xhr.setRequestHeader('Content-Type','application/octet-stream');\n"
        "    xhr.upload.addEventListener('progress',function(e){\n"
        "        if(e.lengthComputable)setItemProgress(itemId,e.loaded,e.total);},false);\n"
        "    xhr.addEventListener('load',function(){\n"
        "        if(xhr.status===200){setItemStatus(itemId,'completed','OK');setItemProgress(itemId,1,1);}\n"
        "        else setItemStatus(itemId,'error','Failed');done();},false);\n"
        "    xhr.addEventListener('error',function(){setItemStatus(itemId,'error','Error');done();},false);\n"
        "    xhr.send(file);}\n"
        "function startUpload() {\n"
        "    if (selectedFiles.length === 0) {\n"
        "        alert('\\u8BF7\\u5148\\u9009\\u62E9\\u6587\\u4EF6\\uFF01');\n"
        "        return;\n"
        "    }\n"
        "    if (isUploading) return;\n"
        "    currentUploadIndex = 0;\n"
        "    isUploading = true;\n"
        "    var btn = document.getElementById('uploadBtn');\n"
        "    btn.disabled = true;\n"
        "    btn.style.background = '#ccc';\n"
        "    btn.innerText = 'Uploading...';\n"
        "    uploadNextFile();\n"
        "}\n"
        "function uploadNextFile() {\n"
        "    if (currentUploadIndex >= selectedFiles.length) {\n"
        "        var progressDiv = document.getElementById('uploadProgress');\n"
        "        progressDiv.innerHTML += '<div style=\"color:#28a745;font-weight:bold;margin-top:10px;\">\\u2713 All files uploaded!</div>';\n"
        "        isUploading = false;\n"
        "        var btn = document.getElementById('uploadBtn');\n"
        "        btn.disabled = false;\n"
        "        btn.style.background = '#4CAF50';\n"
        "        btn.innerText = 'Start Upload';\n"
        "        return;\n"
        "    }\n"
        "    var file = selectedFiles[currentUploadIndex];\n"
        "    var progressDiv = document.getElementById('uploadProgress');\n"
        "    var itemId = 'upload-item-' + currentUploadIndex;\n"
        "    var currentPath = document.querySelector('input[name=\"path\"]').value;\n"
        "    var itemHtml = '<div class=\"upload-item\" id=\"' + itemId + '\">' +\n"
        "        '<div class=\"upload-item-header\">' +\n"
        "            '<span class=\"upload-item-name\">' + file.name + '</span>' +\n"
        "            '<span class=\"upload-item-status uploading\">Pending...</span>' +\n"
        "        '</div>' +\n"
        "        '<div class=\"progress-bar-bg\">' +\n"
        "            '<div class=\"progress-bar-fill\" id=\"' + itemId + '-bar\">0' + String.fromCharCode(37) + '</div>' +\n"
        "        '</div>' +\n"
        "        '<div class=\"progress-text\" id=\"' + itemId + '-text\">0 / ' + formatSize(file.size) + '</div>' +\n"
        "    '</div>';\n"
        "    if (currentUploadIndex === 0) progressDiv.innerHTML = '<h4>Progress</h4>' + itemHtml;\n"
        "    else progressDiv.innerHTML += itemHtml;\n"
        "    var advance = function(){currentUploadIndex++;setTimeout(uploadNextFile,0);};\n"
        "    if (isFontDir(currentPath) && isTtfFile(file.name) && window.L1GlyfBuilder) {\n"
        "        setItemStatus(itemId,'uploading','Build L1...');\n"
        "        L1GlyfBuilder.uploadTtfWithCache(file,currentPath,{\n"
        "            onBuilt:function(st){var t=document.getElementById(itemId+'-text');\n"
        "                if(t)t.textContent='cache '+st.cached+' glyphs, '+formatSize(st.totalBytes);},\n"
        "            onPhase:function(ph,nm){setItemStatus(itemId,'uploading',ph==='l1glyf'?'Up '+nm:'Up '+nm);},\n"
        "            onProgress:function(l,t){setItemProgress(itemId,l,t);}\n"
        "        }).then(function(){setItemStatus(itemId,'completed','OK+cache');advance();})\n"
        "          .catch(function(e){setItemStatus(itemId,'error',String(e));advance();});\n"
        "        return;\n"
        "    }\n"
        "    setItemStatus(itemId,'uploading','Uploading...');\n"
        "    uploadOneXHR(file,currentPath,itemId,advance);\n"
        "}\n"
        "</script>\n"
        "<h3>File List</h3>\n"
        "<ul class=\"file-list\">\n");
    
    /* ================= 替换结束 ================= */
    
    /* 扫描指定目录 */
    HTTP_LOG("Scanning directory: %s", dir_path);
    res = f_opendir(&dir, dir_path);
    HTTP_LOG("f_opendir result: %d", res);
    if (res == FR_OK) {
        int entry_count = 0;
        while (1) {
            res = f_readdir(&dir, &fno);
            if (res != FR_OK || fno.fname[0] == 0) break;
            
            /* 防下溢出检查：确保剩余空间足够 */
            if (len >= buf_size - 200) {
                HTTP_LOG("Buffer full, stopping at %d entries", entry_count);
                break;
            }
            
            /* 防整型下溢检查 */
            if (len < 0 || len >= buf_size) {
                HTTP_LOG("Buffer overflow detected! len=%d, buf_size=%d", len, buf_size);
                break;
            }
            
            entry_count++;
            
            if (fno.fattrib & AM_DIR) {
                /* 跳过.和..目录 */
                if (strcmp(fno.fname, ".") == 0 || strcmp(fno.fname, "..") == 0) {
                    continue;
                }
                /* 构建完整目录路径 */
                char full_path[HTTP_MAX_PATH];
                if (strcmp(dir_path, "/") == 0) {
                    snprintf(full_path, sizeof(full_path), "/%s", fno.fname);
                } else {
                    snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, fno.fname);
                }
                len += snprintf(html_buf + len, buf_size - len,
                    "<li class=\"file-item dir\">\n"
                    "<span class=\"file-icon\">📁</span>\n"
                    "<span class=\"file-name\"><a href=\"/?path=%s\">%s/</a></span>\n"
                    "</li>\n", full_path, fno.fname);
                total_dirs++;
            } else {
                char size_str[32];
                if (fno.fsize < 1024) {
                    snprintf(size_str, sizeof(size_str), "%d B", (int)fno.fsize);
                } else if (fno.fsize < 1024 * 1024) {
                    snprintf(size_str, sizeof(size_str), "%d KB", (int)(fno.fsize / 1024));
                } else {
                    snprintf(size_str, sizeof(size_str), "%d MB", (int)(fno.fsize / (1024 * 1024)));
                }
                /* 显示相对于当前目录的文件名 */
                char display_name[256];
                if (dir_path[strlen(dir_path)-1] == '/') {
                    snprintf(display_name, sizeof(display_name), "%s%s", dir_path, fno.fname);
                } else {
                    snprintf(display_name, sizeof(display_name), "%s/%s", dir_path, fno.fname);
                }
                len += snprintf(html_buf + len, buf_size - len,
                    "<li class=\"file-item file\">\n"
                    "<span class=\"file-icon\">📄</span>\n"
                    "<span class=\"file-name\">%s</span>\n"
                    "<span class=\"file-size\">%s</span>\n"
                    "<span class=\"file-actions\">\n"
                    "<a class=\"btn-download\" href=\"/?download=%s\" target=\"_blank\">Download</a>\n"
                    "<button class=\"btn-delete\" onclick=\"deleteFile('%s')\">Delete</button>\n"
                    "</span>\n"
                    "</li>\n", display_name, size_str, display_name, display_name);
                total_files++;
            }
        }
        f_closedir(&dir);
        HTTP_LOG("Total entries read: %d, files: %d, dirs: %d", entry_count, total_files, total_dirs);
    } else {
        len += snprintf(html_buf + len, buf_size - len,
            "<li class=\"file-item\">无法打开目录 (错误码: %d)</li>\n", res);
    }
    
    /* HTML尾部 */
    len += snprintf(html_buf + len, buf_size - len,
        "</ul>\n"
        "<p style=\"color:#666;margin-top:20px;\">文件数: %d | 文件夹数: %d</p>\n"
        "</div>\n"
        "</body>\n"
        "</html>\n",
        total_files, total_dirs);
    
    return len;
}

/*
 * 发送HTTP响应
 */
static int send_response(int sock, const char *status, const char *content_type,
                        const char *body, int body_len)
{
    char header[256];
    int header_len;
    
    header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        status, content_type, body_len);
    
    send(sock, header, header_len, 0);
    if (body && body_len > 0) {
        send(sock, body, body_len, 0);
    }
    
    return 0;
}

/*
 * 发送 Flash 中的静态资源（分块 send，不占用大 RAM）
 */
static int send_static_buffer(int sock, const char * content_type,
                              const unsigned char * data, unsigned int len)
{
    char header[256];
    int header_len;
    unsigned int sent;
    unsigned int chunk;

    header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: public, max-age=3600\r\n"
        "\r\n",
        content_type, len);
    send(sock, header, header_len, 0);

    sent = 0;
    while (sent < len) {
        chunk = len - sent;
        if (chunk > (unsigned int)HTTP_BUF_SIZE) {
            chunk = (unsigned int)HTTP_BUF_SIZE;
        }
        send(sock, data + sent, (int)chunk, 0);
        sent += chunk;
    }
    return 0;
}

/*
 * 发送HTML响应
 */
static int send_html_response(int sock, const char *html_body)
{
    return send_response(sock, "200 OK", "text/html; charset=utf-8", 
                       html_body, strlen(html_body));
}

/*
 * 发送重定向响应
 */
static int send_redirect_response(int sock, const char *location)
{
    char header[256];
    int header_len;
    
    header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 302 Found\r\n"
        "Location: %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n",
        location);
    
    return send(sock, header, header_len, 0);
}

/*
 * 发送文件内容 - 分块读取并发送，支持大文件
 * 使用 g_http_buffer[HTTP_BUF_SIZE] 全局缓冲区避免大块malloc
 */
static int send_file_content(int sock, const char *filepath)
{
    FIL fp;
    FRESULT res;
    UINT br;
    int total_sent = 0;
    
    /* 动态分配缓冲区，避免撑爆 4KB 的线程栈 */
    char *file_buf = (char *)_dma_malloc(HTTP_BUF_SIZE, DMAHEAP_PSRAM);
    if (!file_buf) {
        HTTP_LOG("Failed to allocate PSRAM g_http_buffer for file send");
        return -1;
    }
    
    /* 获取文件大小 */
    FILINFO fno;
    res = f_stat(filepath, &fno);
    if (res != FR_OK) {
        HTTP_LOG("f_stat failed for %s: %d", filepath, res);
        _dma_free(file_buf, 0);
        send_response(sock, "404 Not Found", "text/plain", "File not found", 13);
        return -1;
    }
    
    HTTP_LOG("Sending file: %s (size: %lu bytes)", filepath, (unsigned long)fno.fsize);
    
    /* 打开文件 */
    res = f_open(&fp, filepath, FA_READ);
    if (res != FR_OK) {
        HTTP_LOG("f_open failed: %d", res);
        _dma_free(file_buf, 0);
        send_response(sock, "500 Internal Server Error", "text/plain", "Cannot open file", 15);
        return -1;
    }
    
    /* 发送HTTP响应头 */
    char header[256];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %lu\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        (unsigned long)fno.fsize,
        strrchr(filepath, '/') ? strrchr(filepath, '/') + 1 : filepath);
    
    if (send(sock, header, header_len, 0) < 0) {
        f_close(&fp);
        _dma_free(file_buf, 0);
        return -1;
    }
    
    /* 分块读取并发送文件内容 */
    while (g_http_running) {
        res = f_read(&fp, file_buf, HTTP_BUF_SIZE, &br);
        if (res != FR_OK || br == 0) {
            break;
        }
        
        int sent = send(sock, file_buf, br, 0);
        if (sent < 0) {
            HTTP_LOG("send failed, total sent: %d", total_sent);
            f_close(&fp);
            _dma_free(file_buf, 0);
            return -1;
        }
        
        total_sent += sent;
    }
    
    f_close(&fp);
    _dma_free(file_buf, 0);
    HTTP_LOG("File sent successfully: %d bytes", total_sent);
    return 0;
}

static char *find_binary_boundary(char *buf, int buf_len, const char *boundary, int boundary_len)
{
    int i;

    if (!buf || !boundary || buf_len <= 0 || boundary_len <= 0 || buf_len < boundary_len) {
        return NULL;
    }

    for (i = 0; i <= buf_len - boundary_len; i++) {
        if (memcmp(buf + i, boundary, boundary_len) == 0) {
            return buf + i;
        }
    }

    return NULL;
}

/*
 * 处理文件上传 - 零malloc版本，支持PSRAM大缓冲区
 *
 * 重要：对于multipart/form-data上传，filename字段在multipart body的part header中，
 * 不在HTTP header里。HTTP header到\r\n\r\n结束，filename在后续的boundary块中。
 *
 * 处理流程：
 * 1. 在已接收数据中查找filename
 * 2. 找到后创建文件
 * 3. 循环接收并写入数据，直到遇到boundary结束标记
 *
 * PSRAM优化：当启用PSRAM时，使用PSRAM作为接收缓冲区，
 * 避免大文件上传时消耗宝贵的内部SRAM堆内存
 */
static int handle_file_upload_streaming(int sock, const char *boundary,
                                        const char *initial_data, int initial_len,
                                        char *filename, const char *upload_path)
{
    FIL fp;
    FRESULT res;
    char *p, *q;
    char filepath[HTTP_MAX_PATH];
    UINT bw;
    int total_written = 0;
    int found_filename = 0;
    int buf_used = 0;
    char *recv_buffer = (char *)g_upload_io_buf;
    const int BUFFER_SIZE = HTTP_UPLOAD_IO_SIZE;

    HTTP_LOG("Streaming upload path=%s initial_len=%d", upload_path, initial_len);
    
    /* 确保上传目录存在 */
    res = f_mkdir(upload_path);
    HTTP_LOG("f_mkdir result: %d", res);
    
    /* 构建完整文件路径 - 去掉"0:"前缀 */
    char clean_path[HTTP_MAX_PATH];
    if (strncmp(upload_path, "0:", 2) == 0) {
        strcpy(clean_path, upload_path + 2);
    } else {
        strcpy(clean_path, upload_path);
    }
    
    /* 第一阶段：接收数据直到找到filename */
    /* 把initial_data复制到recv_buffer */
    int copy_len = (initial_len < BUFFER_SIZE) ? initial_len : BUFFER_SIZE - 1;
    memcpy(recv_buffer, initial_data, copy_len);
    buf_used = copy_len;
    
    while (!found_filename) {
        /* 检查是否被要求停止 */
        if (!g_http_running) {
            HTTP_LOG("Upload aborted (finding filename): server stopping");
            return -1;
        }
        
        /* 在buffer中搜索filename */
        p = recv_buffer;
        while (p < recv_buffer + buf_used - 10) {
            q = strstr(p, "filename=\"");
            if (q) {
                q += 10;
                int i = 0;
                while (*q && *q != '"' && i < 127) {
                    filename[i++] = *q++;
                }
                filename[i] = '\0';
                found_filename = 1;
                HTTP_LOG("Filename found: %s", filename);
                
                /* 保留从q位置之后的数据在buffer中 */
                int leftover = buf_used - (q - recv_buffer);
                if (leftover > 0) {
                    memmove(recv_buffer, q, leftover);
                    buf_used = leftover;
                } else {
                    buf_used = 0;
                }
                break;
            }
            p++;
        }
        
        if (!found_filename) {
            /* 没找到，需要接收更多数据 */
            if (buf_used > BUFFER_SIZE - 256) {
                /* 压缩：保留最后200字节 */
                memmove(recv_buffer, recv_buffer + buf_used - 200, 200);
                buf_used = 200;
            }
            
            int recv_len = recv(sock, recv_buffer + buf_used, BUFFER_SIZE - buf_used - 1, 0);
            if (recv_len <= 0) {
                HTTP_LOG("recv returned %d while finding filename", recv_len);
                return -1;
            }
            buf_used += recv_len;
            recv_buffer[buf_used] = '\0';
        }
    }
    
    /* 第二阶段：找\r\n\r\n定位part header结束 */
    HTTP_LOG("Looking for part header end...");
    int part_header_end = -1;
    
    while (part_header_end < 0) {
        /* 检查是否被要求停止 */
        if (!g_http_running) {
            HTTP_LOG("Upload aborted (finding header end): server stopping");
            return -1;
        }
        
        p = recv_buffer;
        while (p < recv_buffer + buf_used - 3) {
            if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n') {
                part_header_end = p - recv_buffer + 4;
                HTTP_LOG("Part header ends at %d", part_header_end);
                break;
            }
            p++;
        }
        
        if (part_header_end < 0) {
            /* 接收更多数据 */
            int recv_len = recv(sock, recv_buffer + buf_used, BUFFER_SIZE - buf_used - 1, 0);
            if (recv_len <= 0) {
                HTTP_LOG("recv returned %d while finding header end", recv_len);
                return -1;
            }
            buf_used += recv_len;
            recv_buffer[buf_used] = '\0';
        }
    }
    
    /* 创建文件 */
    if (clean_path[strlen(clean_path)-1] == '/') {
        snprintf(filepath, sizeof(filepath), "%s%s", clean_path, filename);
    } else {
        snprintf(filepath, sizeof(filepath), "%s/%s", clean_path, filename);
    }
    
    HTTP_LOG("Creating file: %s", filepath);
    res = f_open(&fp, filepath, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) {
        HTTP_LOG("f_open failed: %d", res);
        return -1;
    }
    
    /* 修复：把 part header 之后的数据挪到缓冲区头部，不直接写入文件 */
    if (buf_used > part_header_end) {
        int leftover_len = buf_used - part_header_end;
        memmove(recv_buffer, recv_buffer + part_header_end, leftover_len);
        buf_used = leftover_len;
        recv_buffer[buf_used] = '\0';
    } else {
        buf_used = 0;
    }
    
    /* 第三阶段：统一的流式处理与 Boundary 检测 */
    HTTP_LOG("Streaming file data...");
    
    int boundary_len = strlen(boundary);
    int keep_len = boundary_len + 4; // 稍微多保留几个字节，防止 boundary 和 \r\n 被切断
    
    while (1) {
        /* 检查是否被要求停止 */
        if (!g_http_running) {
            HTTP_LOG("Upload aborted (streaming): server stopping, %d bytes written", total_written);
            break;
        }
        
        /* 1. 先在现有的 g_http_buffer 中搜索 boundary */
        if (buf_used > 0) {
            char *boundary_pos = find_binary_boundary(recv_buffer, buf_used, boundary, boundary_len);
            if (boundary_pos) {
                int data_len = boundary_pos - recv_buffer;
                /* 去除 boundary 前面的 \r\n */
                if (data_len >= 2 && boundary_pos[-1] == '\n' && boundary_pos[-2] == '\r') {
                    data_len -= 2;
                }
                if (data_len > 0) {
                    res = f_write(&fp, recv_buffer, data_len, &bw);
                    total_written += bw;
                }
                HTTP_LOG("Upload complete! Total: %d bytes", total_written);
                f_close(&fp);
                send_response(sock, "200 OK", "text/plain", "Upload OK", 9);
                return 0;
            }
        }
        
        /* 2. 如果没找到 boundary，把确认安全的数据写入文件 */
        if (buf_used > keep_len) {
            int write_len = buf_used - keep_len;
            res = f_write(&fp, recv_buffer, write_len, &bw);
            if (res != FR_OK) {
                break;
            }
            total_written += bw;
            
            /* 把保留的数据移到缓冲区头部 */
            memmove(recv_buffer, recv_buffer + write_len, keep_len);
            buf_used = keep_len;
            recv_buffer[buf_used] = '\0';
        }
        
        /* 3. 接收更多数据 */
        int recv_len = recv(sock, recv_buffer + buf_used, BUFFER_SIZE - buf_used - 1, 0);
        if (recv_len <= 0) {
            HTTP_LOG("recv returned %d before boundary, incomplete upload", recv_len);
            break;
        }
        
        buf_used += recv_len;
    }
    
    f_close(&fp);
    HTTP_LOG("Connection closed abruptly or write failed. Total: %d bytes", total_written);
    send_response(sock, "400 Bad Request", "text/plain", "Incomplete upload", 17);
    return -1;
}

/*
 * 解析HTTP请求
 * 返回值: 0成功, -1失败
 * 如果请求包含?path=xxx，会从path中提取目录路径到dir_path参数
 */
static int parse_request(const char *req, char *method, char *path, char *version, char *dir_path)
{
    const char *p = req;
    int i;

    /* 1. 安全提取 Method */
    i = 0;
    while (*p && *p != ' ' && i < 31) {
        method[i++] = *p++;
    }
    method[i] = '\0';
    if (*p != ' ') return -1;
    while (*p == ' ') p++; /* 跳过空格 */

    /* 2. 安全提取 Path */
    i = 0;
    while (*p && *p != ' ' && i < 255) {
        path[i++] = *p++;
    }
    path[i] = '\0';
    if (*p != ' ') return -1;
    while (*p == ' ') p++; /* 跳过空格 */

    /* 3. 安全提取 Version (遇到 \r, \n 或空格停止) */
    i = 0;
    while (*p && *p != ' ' && *p != '\r' && *p != '\n' && i < 15) {
        version[i++] = *p++;
    }
    version[i] = '\0';
    
    /* URL解码 */
    url_decode(path, path);
    
    /* 提取?download=参数（用于文件下载） */
    const char *download_param = strstr(path, "?download=");
    if (download_param) {
        /* 找到?download=，提取文件路径 */
        char *equals = strchr(download_param, '=');
        if (equals) {
            equals++;
            char file_path[HTTP_MAX_PATH] = {0};
            url_decode(file_path, equals);
            
            /* 清理路径：移除任何查询参数后面的内容 */
            char *amp = strchr(file_path, '&');
            if (amp) *amp = '\0';
            
            /* 将download参数值保存到dir_path（作为文件路径返回） */
            if (dir_path) {
                strncpy(dir_path, file_path, HTTP_MAX_PATH - 1);
                dir_path[HTTP_MAX_PATH - 1] = '\0';
            }
            
            /* 将path中的?download=xxx部分移除，作为标记表示这是下载请求 */
            char path_copy[HTTP_MAX_PATH];
            strncpy(path_copy, path, sizeof(path_copy) - 1);
            path_copy[sizeof(path_copy) - 1] = '\0';
            char *qm = strstr(path_copy, "?download=");
            if (qm) *qm = '\0';
            strcpy(path, path_copy);
            
            HTTP_LOG("Download request: %s", file_path);
        }
    } else if (dir_path) {
        /* ?path= 用于目录浏览；/api/upload 的 query 由上传处理自行解析 */
        const char *path_param = strstr(path, "?path=");
        if (path_param && strncmp(path, "/api/upload", 11) != 0) {
            /* 找到?path=，提取路径部分 */
            char path_copy[HTTP_MAX_PATH];
            strncpy(path_copy, path, sizeof(path_copy) - 1);
            path_copy[sizeof(path_copy) - 1] = '\0';
            
            /* 提取路径值 */
            char *equals = strchr(path_param, '=');
            if (equals) {
                equals++;
                char path_value[HTTP_MAX_PATH] = {0};
                url_decode(path_value, equals);
                
                /* 清理路径：移除任何查询参数后面的内容 */
                char *amp = strchr(path_value, '&');
                if (amp) *amp = '\0';
                
                strncpy(dir_path, path_value, HTTP_MAX_PATH - 1);
                dir_path[HTTP_MAX_PATH - 1] = '\0';
                
                /* 将path中的?path=xxx部分移除，只保留基础路径 */
                char *qm = strstr(path_copy, "?path=");
                if (qm) *qm = '\0';
                strcpy(path, path_copy);
            }
        } else {
            /* 没有path参数，默认浏览根目录 */
            strcpy(dir_path, "/");
        }
    }
    
    HTTP_LOG("Request: %s %s %s (dir: %s)", method, path, version, dir_path ? dir_path : "N/A");
    return 0;
}

/*
 * HTTP服务器线程
 */
static int http_server_alloc_buffers(void)
{
    g_http_buffer = g_http_workspace;
    g_http_response = g_http_workspace + HTTP_BUF_SIZE;
    return 0;
}

 static void http_server_thread(void *arg)
 {
     int client_sock;
     struct sockaddr_in server_addr, client_addr;
     socklen_t addr_len = sizeof(client_addr);
     int recv_len;
     char method[32], path[256], version[16];

     (void)arg;
     HTTP_LOG("Worker thread ready (idle until start)");

     for (;;) {
         while (!g_http_running) {
             OS_MSleep(50);
         }

         if (http_server_alloc_buffers() != 0) {
             g_http_running = 0;
             continue;
         }

         g_http_serving = 1;
    
    HTTP_LOG("Server starting on port %d...", g_http_port);
    
    /* 创建socket */
    g_server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_sock < 0) {
        HTTP_LOG("Failed to create socket");
        goto serve_cleanup;
    }
    
    /* 设置地址重用 */
    int opt = 1;
    setsockopt(g_server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    /* 绑定地址 */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(g_http_port);
    
    if (bind(g_server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        HTTP_LOG("Failed to bind socket");
        closesocket(g_server_sock);
        g_server_sock = -1;
        goto serve_cleanup;
    }
    
    /* 监听 */
    if (listen(g_server_sock, 1) < 0) {
        HTTP_LOG("Failed to listen");
        closesocket(g_server_sock);
        g_server_sock = -1;
        goto serve_cleanup;
    }

    printf("[HTTP] listening :%d (rcvbuf=%d sndbuf=%d sram_free=%u)\r\n",
           g_http_port, HTTP_SOCK_RCVBUF, HTTP_SOCK_SNDBUF,
           (unsigned)sram_heap_free_bytes());
    print_heap_info();
    HTTP_LOG("Server listening on port %d", g_http_port);
    http_set_nonblock(g_server_sock, 1);
    
    while (g_http_running) {
        client_sock = http_accept_client(g_server_sock, &client_addr, &addr_len);
        if (client_sock < 0) {
            break;
        }
        
        g_client_sock = client_sock;
        http_tune_client_socket(client_sock);

        printf("[HTTP] client %s:%d\r\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        HTTP_LOG("Client connected: %s:%d",
                inet_ntoa(client_addr.sin_addr),
                ntohs(client_addr.sin_port));
        
        /* 接收请求 */
        recv_len = recv(client_sock, g_http_buffer, HTTP_BUF_SIZE - 1, 0);

        if (!g_http_running) {
            closesocket(client_sock);
            g_client_sock = -1;
            break;
        }
        
        if (recv_len > 0) {
            g_http_buffer[recv_len] = '\0';
            
            /* 解析请求 */
            char dir_path[HTTP_MAX_PATH] = "/";
            FRESULT res = FR_OK;
            if (parse_request(g_http_buffer, method, path, version, dir_path) == 0) {
                if (strcmp(method, "GET") == 0) {
                    if (strcmp(path, "/screensaver") == 0) {
                        (void)generate_screensaver_html(g_http_response, HTTP_RESPONSE_SIZE);
                        send_html_response(client_sock, g_http_response);
                    } else if (strcmp(path, "/l1glyf.js") == 0) {
                        send_static_buffer(client_sock, "application/javascript; charset=utf-8",
                                           http_l1glyf_js, http_l1glyf_js_len);
                    } else if (strcmp(path, "/screensaver/status") == 0) {
                        char status_json[256];
                        screensaver_get_status_json(status_json, sizeof(status_json));
                        send_response(client_sock, "200 OK", "application/json", status_json, strlen(status_json));
                    } else {
                    /* 检查是否是文件下载请求：path为"/"且dir_path不是"/"（说明dir_path包含文件路径） */
                    FILINFO fno;
                    res = f_stat(dir_path, &fno);
                    if (res == FR_OK && !(fno.fattrib & AM_DIR)) {
                        /* 是文件，发送文件内容 */
                        HTTP_LOG("GET request for file download: %s", dir_path);
                        send_file_content(client_sock, dir_path);
                    } else {
                        /* 是目录，生成HTML页面 */
                        (void)generate_file_list_html(g_http_response, HTTP_RESPONSE_SIZE, dir_path);
                        send_html_response(client_sock, g_http_response);
                    }
                    }
                    } else if (strcmp(method, "PUT") == 0 &&
                               strncmp(path, "/api/upload", 11) == 0) {
                        char up_path[HTTP_MAX_PATH];
                        char up_name[128];
                        char raw_path[256];
                        if (parse_request_line_path(g_http_buffer, raw_path, sizeof(raw_path)) != 0 ||
                            parse_api_upload_params(raw_path, up_path, sizeof(up_path),
                                                    up_name, sizeof(up_name)) != 0) {
                            send_response(client_sock, "400 Bad Request", "text/plain",
                                          "Bad upload query", 16);
                        } else {
                            handle_api_upload_raw(client_sock, g_http_buffer, recv_len,
                                                  up_path, up_name);
                        }
                    } else if (strcmp(method, "POST") == 0) {
                    /* 检查是否是删除请求 */
                    if (strncmp(path, "/delete", 7) == 0) {
                        HTTP_LOG("DELETE request: %s", dir_path);
                        res = f_unlink(dir_path);
                        if (res == FR_OK) {
                            HTTP_LOG("File deleted: %s", dir_path);
                            send_response(client_sock, "200 OK", "text/plain", "Delete OK", 9);
                        } else {
                            HTTP_LOG("Delete failed: %d", res);
                            send_response(client_sock, "500 Internal Server Error", "text/plain", "Delete failed", 12);
                        }
                    } else if (strcmp(path, "/screensaver/upload") == 0) {
                        handle_screensaver_upload_raw(client_sock, g_http_buffer, recv_len);
                    } else if (strstr(g_http_buffer, "multipart/form-data")) {
                        char *header_end = strstr(g_http_buffer, "\r\n\r\n");
                        if (!header_end) {
                            int total_received = recv_len;
                            while (!header_end && total_received < HTTP_BUF_SIZE - 1 && g_http_running) {
                                int more = recv(client_sock, g_http_buffer + total_received,
                                                HTTP_BUF_SIZE - total_received - 1, 0);
                                if (more <= 0) {
                                    break;
                                }
                                total_received += more;
                                g_http_buffer[total_received] = '\0';
                                header_end = strstr(g_http_buffer, "\r\n\r\n");
                            }
                            recv_len = total_received;
                        }

                        if (header_end && g_http_running) {
                            const char *boundary_start = strstr(g_http_buffer, "boundary=");
                            char boundary[64] = {0};
                            char upload_path[HTTP_MAX_PATH] = UPLOAD_PATH;
                            char uploaded_filename[128] = {0};
                            int header_total_len = (int)(header_end + 4 - g_http_buffer);
                            int body_initial_len = recv_len - header_total_len;
                            const char *path_start;

                            if (boundary_start) {
                                boundary_start += 9;
                                int i = 0;
                                while (*boundary_start && *boundary_start != '\r' && i < (int)sizeof(boundary) - 3) {
                                    boundary[i++] = *boundary_start++;
                                }
                                if (boundary[0] != '-' || boundary[1] != '-') {
                                    memmove(boundary + 2, boundary, i + 1);
                                    boundary[0] = '-';
                                    boundary[1] = '-';
                                }
                            }

                            path_start = strstr(g_http_buffer, "name=\"path\"");
                            if (path_start) {
                                const char *value_start = strstr(path_start, "\r\n\r\n");
                                if (value_start) {
                                    int i = 0;
                                    value_start += 4;
                                    while (*value_start && *value_start != '\r' && i < (int)sizeof(upload_path) - 1) {
                                        upload_path[i++] = *value_start++;
                                    }
                                    upload_path[i] = '\0';
                                }
                            }

                            if (body_initial_len < 0) {
                                body_initial_len = 0;
                            }

                            if (handle_file_upload_streaming(client_sock, boundary,
                                    g_http_buffer + header_total_len, body_initial_len,
                                    uploaded_filename, upload_path) == 0) {
                                HTTP_LOG("Multipart upload OK: %s", uploaded_filename);
                            } else {
                                send_response(client_sock, "500 Internal Server Error", "text/plain",
                                              "Upload failed", 13);
                            }
                        } else {
                            send_response(client_sock, "400 Bad Request", "text/plain",
                                       "Bad Request: incomplete header", 28);
                        }
                    } else {
                        /* 简单POST处理 */
                        snprintf(g_http_response, HTTP_RESPONSE_SIZE,
                            "<html><body><h1>POST Received</h1><a href=\"/\">Back</a></body></html>");
                        send_html_response(client_sock, g_http_response);
                    }
                } else {
                    /* 不支持的方法 */
                    send_response(client_sock, "501 Not Implemented", "text/plain",
                                 "Not Implemented", 15);
                }
            } else {
                /* 解析失败 */
                send_response(client_sock, "400 Bad Request", "text/plain",
                           "Bad Request", 12);
            }
        }
        
        shutdown(client_sock, SHUT_RDWR);
        closesocket(client_sock);
        g_client_sock = -1;
        if (g_server_sock >= 0) {
            http_drain_extra_clients(g_server_sock);
        }

        if (!g_http_running) {
            break;
        }
    }
     
serve_cleanup:
    http_set_nonblock(g_server_sock, 0);
    if (g_server_sock >= 0) {
        closesocket(g_server_sock);
        g_server_sock = -1;
    }

    printf("[HTTP] stopped\r\n");
    print_heap_info();

    g_http_serving = 0;
    /* 保留工作线程，回到 idle 等待下次 start */
     }
 }

/*
 * 初始化HTTP服务器
 */
int http_server_init(int port)
{
    g_http_port = port > 0 ? port : HTTP_SERVER_PORT;
    g_http_running = 0;
    HTTP_LOG("HTTP server initialized (port=%d)", g_http_port);
    return 0;
}

/*
 * 开机预创建工作线程（font warm 前 SRAM 尚充足时调用一次）
 */
int http_server_reserve_thread(void)
{
    if (http_server_thread_active()) {
        HTTP_LOG("Worker thread already reserved");
        return 0;
    }

    HTTP_LOG("Reserving worker thread (stack=%d bytes)...", HTTP_THREAD_STACK_SIZE);
    if (OS_ThreadCreate(&g_http_thread, "http_server",
                        http_server_thread, NULL,
                        OS_PRIORITY_NORMAL, HTTP_THREAD_STACK_SIZE) != 0) {
        HTTP_LOG("Failed to reserve worker thread");
        return -1;
    }

    HTTP_LOG("Worker thread reserved, handle=0x%08x", (unsigned int)g_http_thread.handle);
    return 0;
}

/*
 * 启动HTTP服务器
 */
int http_server_start(void)
{
    HTTP_LOG("start: running=%d serving=%d handle=0x%08x",
             g_http_running, g_http_serving, (unsigned int)g_http_thread.handle);

    if (!http_server_thread_active()) {
        HTTP_LOG("Worker thread not reserved; call http_server_reserve_thread() at boot");
        return -1;
    }

    if (g_http_running || g_http_serving) {
        HTTP_LOG("Server already running");
        return 0;
    }

    if (sram_heap_free_bytes() < HTTP_MIN_SRAM_FREE) {
        printf("[HTTP] WARN: SRAM free low (%u B), network buffers use PSRAM fallback\r\n",
               (unsigned)sram_heap_free_bytes());
    }

    g_http_running = 1;
    HTTP_LOG("HTTP server start requested (worker will bind port %d)", g_http_port);
    return 0;
}

/*
 * 停止HTTP服务器
 */
void http_server_request_stop(void)
{
    if (!g_http_running && !g_http_serving) {
        return;
    }

    printf("[HTTP] stopping...\r\n");
    g_http_running = 0;

    if (g_client_sock >= 0) {
        shutdown(g_client_sock, SHUT_RDWR);
    }

    if (g_server_sock >= 0) {
        shutdown(g_server_sock, SHUT_RDWR);
    }
}

void http_server_stop(void)
{
    if (!g_http_running && !g_http_serving) {
        return;
    }

    http_server_request_stop();

    for (int wait_count = 0; wait_count < HTTP_STOP_WAIT_MAX && g_http_serving; wait_count++) {
        OS_MSleep(HTTP_STOP_WAIT_MS);
    }
    if (g_http_serving) {
        printf("[HTTP] WARN: worker still serving after %dms\r\n",
               HTTP_STOP_WAIT_MAX * HTTP_STOP_WAIT_MS);
    }

    if (g_client_sock >= 0) {
        closesocket(g_client_sock);
        g_client_sock = -1;
    }

    print_heap_info();
    printf("[HTTP] stop done (running=%d serving=%d)\r\n",
           g_http_running, g_http_serving);
}

/*
 * 获取服务器运行状态
 */
int http_server_is_running(void)
{
    return g_http_running || g_http_serving;
}

/*
 * 获取服务器端口
 */
int http_server_get_port(void)
{
    return g_http_port;
}
