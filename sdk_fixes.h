#ifndef _SDK_FIXES_H_
#define _SDK_FIXES_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 置位后, 下一次 pm_enter_mode() 期间 HAL_PRCM_DisableInter32KCalib 被
 * 跳过 (32K 校准保持), 修复 HIBERNATION 唤醒后 boot ROM flash 初始化
 * 失败的问题。调用 pm_enter_mode(PM_MODE_HIBERNATION) 前置 1。
 * 唤醒为冷启动, 标志自动复位, 无需手动清除。
 */
void sdk_keep_32k_calib(int on);

#ifdef __cplusplus
}
#endif

#endif /* _SDK_FIXES_H_ */
