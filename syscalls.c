/*
 * syscalls.c — newlib (nano.specs / libc_nano.a) 系统调用桩
 *
 * 从 SDK project/common/startup/gcc/retarget.c 解耦而来的工程级扩展:
 * SDK 原版 retarget.c 不提供这些桩, 本工程链接时需要, 故随工程携带。
 */
#include <stddef.h>

/* Stub implementations for libc_nano.a (nano.specs) syscalls */
int _read(int fd, void *buf, size_t count)
{
	(void)fd;
	(void)buf;
	(void)count;
	return -1;  /* Return error: not implemented */
}

int _write(int fd, const void *buf, size_t count)
{
	/* Only support stdout/stderr (fd 1, 2), otherwise return error */
	if (fd != 1 && fd != 2) {
		return -1;
	}
	/* For stdout/stderr, we just discard the output or could route to UART */
	(void)buf;
	(void)count;
	return count;  /* Pretend success */
}

int _close(int fd)
{
	(void)fd;
	return -1;  /* Return error: not implemented */
}

int _lseek(int fd, int offset, int whence)
{
	(void)fd;
	(void)offset;
	(void)whence;
	return -1;  /* Return error: not implemented */
}
