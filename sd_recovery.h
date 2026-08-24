/*
 * sd_recovery - SD 数据错误后的完整子系统重建
 *
 * 背景: XR872 的 SD 主机驱动在预编译 librom.a 里, CMD25 多块写遇到
 * 数据 CRC 错误 (DCE, 如充电时 WiFi 并发/电源噪声导致) 后只复位
 * 主机控制器, 不会向卡发 CMD12 中止命令, 卡一直停在接收数据状态,
 * 后续 CMD16 全部 RTO, 整个文件系统卡死直到重启 (日志特征:
 *   [ERR] SDC err, cmd 25, DCE
 *   [ERR] __sdmmc_block_rw ... Err!!
 *   [ERR] SDC err, cmd 16, RTO  ... 重复
 * )。
 *
 * 应用层无法改 ROM 驱动, 唯一可靠的恢复是整卡重建:
 *   卸载卷 → 关主机 → 重建主机 → 重新扫描挂载。
 * 该序列与 charge_mode 的 charge_ensure_sd_ready 一致。
 *
 * 并发模型: 恢复期间不能有其它上下文访问 SD。系统里会并发访问 SD
 * 的只有 http 上传线程 (调用方) 和 lvgl_task (font_warm/bookshelf/
 * settings 都在 LVGL 上下文)。因此恢复被安排在 LVGL 任务内执行:
 *   1. 上传线程调用 sd_recovery_request_wait() 置请求标志并等待;
 *   2. LVGL 定时器轮询到标志后在 LVGL 上下文执行完整重建;
 *   3. 重建完成后递增代数计数, 上传线程退出等待并重试写入。
 * 这样恢复期间 LVGL 自己的 f_* 是串行的 (定时器逐个执行), 上传线程
 * 在等待不碰 SD, 天然无竞争, 也不需要挂起任务/抢 FatFs 锁。
 */
#ifndef __SD_RECOVERY_H__
#define __SD_RECOVERY_H__

#include "kernel/os/os.h"

/* 在 LVGL 就绪后调用一次 (幂等): 创建信号量/代数 + 恢复定时器。 */
void sd_recovery_init(void);

/* 请求一次 SD 完整重建并阻塞等待完成。
 * 返回 0 表示恢复完成 (SD 已重新挂载), -1 表示超时或服务未就绪。 */
int sd_recovery_request_wait(void);

/* DCE 探测用的时钟策略: 把协商后的 SD 时钟降频以拉开 DAT 时序余量。
 * 幂等; 在 boot 挂载完成、以及每次恢复重挂成功后调用一次。 */
void sd_apply_clock_policy(void);

/* 格式化进行中标志: 外部(屏保/休眠)用它拒绝进 hibernation,
 * 防止 f_mkfs 写盘途中 SD 被挂起导致卷损坏。 */
void sd_format_set_busy(int busy);
int sd_format_is_busy(void);

/* 完整重建 SD 子系统 (卸载→关主机→重建→重扫→重挂)。
 * 通常经由 sd_recovery_request_wait() 在 LVGL 任务内执行;
 * boot 自愈等单线程阶段也可直接调用。 */
void sd_recovery_rebuild_now(void);

#endif /* __SD_RECOVERY_H__ */
