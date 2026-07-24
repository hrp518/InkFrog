/* sys_stubs.c - libc syscall stubs
 *
 * 为 -O3 -ffast-math 优化 lv_tiny_ttf.c 后引入的 abort()/__assert_func()
 * 路径提供最小 stub 实现。这些符号在 -Os 下会被 --gc-sections 丢弃,
 * 但 -O3 内联后变成可达, 链接器找不到就报 undefined reference。
 *
 * 行为: __assert_func 打印断言信息后原地死循环(不调 _exit, 避免链式
 * 拉入更多 stubs)。_exit/_kill/_getpid/_isatty/_fstat 返回固定值。
 */

#include <stdio.h>
#include <stdint.h>

void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    printf("[ASSERT] %s:%d %s: %s\r\n", file ? file : "?", line,
           func ? func : "?", expr ? expr : "?");
    /* 原地死循环, 不调 abort/_exit 避免拉入更多 stubs */
    while (1) {
    }
}

void _exit(int status)
{
    (void)status;
    while (1) {
    }
}

int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    return -1;
}

int _getpid(void)
{
    return 1;
}

int _isatty(int fd)
{
    return (fd == 1 || fd == 2) ? 1 : 0;
}

int _fstat(int fd, void *st)
{
    (void)fd;
    (void)st;
    return -1;
}
