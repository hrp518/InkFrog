/*
 * Copyright (C) 2017 XRADIO TECHNOLOGY CO., LTD. All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the
 *       distribution.
 *    3. Neither the name of XRADIO TECHNOLOGY CO., LTD. nor the names of
 *       its contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "common/cmd/cmd_util.h"
#include "common/cmd/cmd.h"
#include "common/framework/fs_ctrl.h"
#include "fs/fatfs/ff.h"
#include "sd_recovery.h"
#include <stdio.h>
#include <string.h>

#if PRJCONF_NET_EN

#define COMMAND_IPERF       1
#define COMMAND_PING        1

/*
 * net commands
 */
static const struct cmd_data g_net_cmds[] = {
	{ "mode",		cmd_wlan_mode_exec },
#ifdef __CONFIG_WLAN_AP
	{ "ap", 		cmd_wlan_ap_exec },
#endif
#ifdef __CONFIG_WLAN_STA
	{ "sta",		cmd_wlan_sta_exec },
#endif
	{ "ifconfig",	cmd_ifconfig_exec },
#if COMMAND_IPERF
	{ "iperf",		cmd_iperf_exec },
#endif
#if COMMAND_PING
	{ "ping",		cmd_ping_exec },
#endif
};

static enum cmd_status cmd_net_exec(char *cmd)
{
	return cmd_exec(cmd, g_net_cmds, cmd_nitems(g_net_cmds));
}

#endif /* PRJCONF_NET_EN */

/*
 * 格式化 SD 卡 (测试用): 重建 FAT32 卷, 修复 FAT 表损坏 (如 fr=2 FR_INT_ERR)。
 * 用法: format            -> 只打印警告/用法, 不执行
 *       format yes        -> 重建 FAT32 卷 → 重挂 + 重建 /Font,/Inkbook
 * 破坏性操作, 需要第二参数确认才执行。
 * 注意: 格式化会把卡上所有数据清空, 且不能在 WiFi 上传/阅读器读卡时执行。
 */
static enum cmd_status cmd_sd_format_exec(char *cmd)
{
	char *argv[4];
	int argc = cmd_parse_argv(cmd, argv, 4);

	if (argc < 1 || strcmp(argv[0], "yes") != 0) {
		cmd_write_respond(CMD_STATUS_OK,
			"usage: format yes  -> erase whole SD and rebuild FAT32 volume (destructive)");
		return CMD_STATUS_ACKED;
	}

	cmd_write_respond(CMD_STATUS_OK, "formatting SD card, please wait ...");

	/* 格式化是长耗时操作(几十秒), 期间禁止屏保/休眠, 否则 f_mkfs 写盘途中
	 * SD 被挂起 → 卷写坏 (上次事故根因)。 */
	sd_format_set_busy(1);

	/* 格式化由 fs_ctrl_format 完成: 只摘 FatFs 挂载、保留卡对象 (删卡会导致
	 * f_mkfs FR_NOT_READY), f_mkfs 重建后自动重新挂载。 */
	if (fs_ctrl_format() != 0) {
		sd_format_set_busy(0);
		cmd_write_respond(CMD_STATUS_FAIL, "SD format failed");
		return CMD_STATUS_ACKED;
	}

	/* 格式化后挂载状态已重建; 补目录 + 恢复时钟策略 (重挂会把时钟重置回 25MHz) */
	sd_apply_clock_policy();
	f_mkdir("/Font");
	f_mkdir("/Inkbook");
	sd_format_set_busy(0);

	cmd_write_respond(CMD_STATUS_OK, "SD formatted OK: FAT32 volume, /Font + /Inkbook recreated");
	return CMD_STATUS_ACKED;
}

/*
 * main commands
 */
static const struct cmd_data g_main_cmds[] = {
#if PRJCONF_NET_EN
	{ "net",	cmd_net_exec },
#endif
#ifdef __CONFIG_OTA
	{ "ota",	cmd_ota_exec },
#endif
	{ "echo",	cmd_echo_exec },
	{ "mem",	cmd_mem_exec },
	{ "heap",	cmd_heap_exec },
	{ "thread",	cmd_thread_exec },
	{ "upgrade",cmd_upgrade_exec },
	{ "reboot", cmd_reboot_exec },
	{ "efpg",	cmd_efpg_exec },
	{ "format", cmd_sd_format_exec },
};

void main_cmd_exec(char *cmd)
{
	enum cmd_status status;

	if (cmd[0] != '\0') {
#if (!CONSOLE_ECHO_EN)
		if (cmd_strcmp(cmd, "efpg"))
			CMD_LOG(CMD_DBG_ON, "$ %s\n", cmd);
#endif
		status = cmd_exec(cmd, g_main_cmds, cmd_nitems(g_main_cmds));
		if (status != CMD_STATUS_ACKED) {
			cmd_write_respond(status, cmd_get_status_desc(status));
		}
	}
#if (!CONSOLE_ECHO_EN)
	else { /* empty command */
		CMD_LOG(1, "$\n");
	}
#endif
#if CONSOLE_ECHO_EN
	console_write((uint8_t *)"$ ", 2);
#endif
}
