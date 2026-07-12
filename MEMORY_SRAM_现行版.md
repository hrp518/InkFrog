# FontExp 现行版 SRAM / 堆内存说明（爆 heap 版）

> **分支**: `perf-first-open`（待推送）  
> **固件**: FontExp.axf text=990104 data=906916 bss=16996（2026-07-13 编译）  
> **现象**: HTTP 上传 1.6MB `.l1glyf` 时串口刷屏 `heap exhausted, incr 1700, 0x267ee8 >= 0x267c00`，上传失败或极慢。  
> **结论**: **HTTP Server + LWIP TCP 接收路径仍在消耗 SRAM 堆**；SRAM 堆在开机后几乎已满，上传时无法再分配 ~1.7KB 级别的块。

---

## 1. 芯片 SRAM 总览

| 项目 | 值 |
|------|-----|
| SRAM 区域 | `0x00201000` ~ `0x00268000`（**412 KB**） |
| `.text + .data + .bss` 静态占用 | 至 `0x0024FAE8`（约 **317 KB**） |
| **动态堆（`_sbrk` / `malloc`）** | `0x0024FAE8` ~ `0x00267C00`（约 **94.5 KB**） |
| MSP 保留 | `_estack - 1KB` = `0x00267C00` |

链接符号（`gcc/FontExp.map`）：

```
__end__ / __HeapLimit  = 0x0024FAE8
_estack / __StackTop    = 0x00268000
可用堆上限             = 0x00267C00  (=_estack - 1024)
堆总容量               ≈ 96,728 字节
```

`.heap` 段为 **COPY 类型、大小 0**；所有 `malloc` / `OS_ThreadCreate` 栈均从上述 94.5KB 区域增长。

---

## 2. SRAM 堆主要消费者（开机后、HTTP 上传前）

### 2.1 RTOS 线程栈（全部从 SRAM 堆分配）

| 线程 | 栈 (B) | 创建位置 | 时机 |
|------|--------|----------|------|
| main（框架） | 8192 | SDK | 启动 |
| sys_ctrl | 2048 | SDK | 启动 |
| console | 2048 | SDK | 启动 |
| **http_server** | **4096** | `http_server_reserve_thread()` | font warm **之前** |
| **wifi_ctrl** | **4096** | `wifi_controller_start()` | font warm **之前** |
| **disp_task** | **8192** | `main.c` | UI 后 |
| **lvgl_task** | **32768** | `main.c` | UI 后 |
| wifi_scan（按需） | 4096 | `wlan_manager.c` `xTaskCreate` | 设置页扫描 |

**仅应用+框架常驻栈合计 ≈ 61 KB**（不含 TCB/对齐开销），已占堆容量 **~64%**。

### 2.2 其他 SRAM 动态分配

| 模块 | 分配器 | 说明 |
|------|--------|------|
| WLAN MBUF | `psram_malloc` | `localconfig.mk`: `__CONFIG_MBUF_HEAP_MODE=1` |
| WPA | `psram_malloc` | `__CONFIG_WPA_HEAP_MODE=1`（默认） |
| UMAC/LMAC | `psram_malloc` | `__CONFIG_UMAC/LMAC_HEAP_MODE=1` |
| **LWIP `pbuf` / TCP 接收** | **多为 SRAM `malloc`** | 上传时每个 pbuf ~1.5–2KB，**爆 heap 直接来源** |
| FatFs `f_open` / `FIL` | 栈上 `FIL` | 不在堆上；但目录/缓存可能间接 malloc |
| font warm / tiny_ttf | `psram_malloc` / `_dma_malloc` | 字形在 PSRAM，不占 SRAM 堆 |

---

## 3. HTTP Server 内存行为（现行实现）

### 3.1 线程模型

- 开机 `http_server_reserve_thread()` 预创建 **4KB 栈** 工作线程（避免 font warm 后 `OS_ThreadCreate` 失败）。
- `start` 只置标志；`stop` 不删线程。
- 服务时分配（PSRAM DMA heap）：
  - `g_http_buffer` = 2 KB
  - `g_http_response` = 16 KB
  - 每次上传 `handle_file_upload_streaming()` 再 `_dma_malloc(16KB)` 作 `recv_buffer`

### 3.2 上传路径（multipart `/upload`）

1. 首包 header ~548B 已在 `g_http_buffer`（PSRAM）。
2. 进入 streaming：再申请 **16KB PSRAM** `recv_buffer`。
3. `recv()` 循环读 TCP → **LWIP 为每个 TCP 段在 SRAM 分配 pbuf**（日志中 `incr 1700` 典型）。
4. `f_write` 写 SD；每轮 `OS_MSleep(50)` 等 pbuf 释放（仍不足）。
5. 1.6MB 文件需数百次 recv → SRAM 堆在 **已几乎满** 时无法分配 pbuf → 断链/失败。

### 3.3 已做但与爆 heap 无关的优化

- `http_l1glyf_js.c`：浏览器生成 LGF1（已修 `w16` 小端 bug）。
- Font 目录选 `.ttf` 自动：生成 `.l1glyf` → 上传 `.l1glyf` → 上传 `.ttf`（两次大上传）。
- 删除独立 `/l1glyf` 页面，合并到 Font 文件管理页。

---

## 4. PSRAM 布局（对比，上传不直接爆 PSRAM）

| 区域 | 大小 | 用途 |
|------|------|------|
| `.psram_bss` 等静态 | ~1.06 MB | LVGL 256KB、epub_viewer 300KB、lv_port_disp 97KB… |
| `psram_malloc` 池 | ~2.0 MB | 字体 glyf、file_entries、扫描结果等 |
| DMA heap | 1 MB (`__CONFIG_DMAHEAP_PSRAM_SIZE=1024`) | HTTP 缓冲、EPUB、TTF glyf |

**PSRAM 通常仍有余量；瓶颈在 SRAM 堆 ~95KB。**

---

## 5. 复现日志摘要（用户 2026-07-13）

```
POST /upload  Content-Length: 1626710  Connection: keep-alive
multipart; boundary=----WebKitFormBoundary...
path: 0:/Font/  (实际文件 LXGWWenKaiMono-Medium.l1glyf)
Using PSRAM recv_buffer 16384
f_mkdir result: 6
heap exhausted, incr 1700 ...  (重复数十次)
Filename found: LXGWWenKaiMono-Medium.l1glyf
Streaming file data...
```

说明：**在进入写文件流之后**，SRAM 仍无法满足 LWIP/FatFs 路径上的小块分配。

---

## 6. 配置要点（`gcc/localconfig.mk`）

```makefile
__CONFIG_PSRAM := y
__CONFIG_DMAHEAP_PSRAM_SIZE := 1024
__CONFIG_MBUF_HEAP_MODE := 1
__CONFIG_UMAC_HEAP_MODE := 1
__CONFIG_LMAC_HEAP_MODE := 1
```

`prj_config.h`: `PRJCONF_SYSINFO_ADDR = (1040 * 1024)`（镜像扩至 1040K）。

---

## 7. 给外部模型的审阅要点

1. **问题类型**: SRAM 堆耗尽，不是 PSRAM 不足；**HTTP 大文件上传触发 LWIP pbuf SRAM 分配**。
2. **根因链**: 线程栈占满 61KB+ → 堆剩余极少 → TCP 接收 pbuf（~1700B）失败 → 上传卡死/断链。
3. **不要建议**: 再增大 `lvgl` 栈或再 `OS_ThreadCreate` 而不先腾 SRAM。
4. **应建议**: 上传路径避开 multipart、LWIP pbuf 进 PSRAM、或增大 SRAM 堆（减栈/移栈到 PSRAM）、或专用 raw POST + 零拷贝写 SD。
5. **l1glyf 格式**: 应用端 `LGF1` magic `0x3146474C`，小端；浏览器 builder 曾有大端 `w16` bug，已修，旧文件需重传。

---

## 8. 相关源文件

| 文件 | 作用 |
|------|------|
| `http_server.c` | HTTP 服务、multipart 上传、`handle_file_upload_streaming` |
| `http_server.h` | `http_server_reserve_thread()` |
| `main.c` | 线程栈、`reserve_thread` 时机 |
| `web/l1glyf_builder.js` | 浏览器 LGF1 生成 |
| `heap_debug.c` | `print_heap_info()` |
| `gcc/localconfig.mk` | PSRAM / MBUF 堆模式 |
| `gcc/FontExp.map` | 链接后真实地址 |
