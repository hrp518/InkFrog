/* ff.h — 主机单测用的 FatFs 桩（仅 gbk.h 所需的最小类型与函数声明）。
 * 真实实现由 tools/test_gbk.c 提供的内存文件版 f_lseek/f_read 替代。 */
#ifndef FF_HOST_STUB_H
#define FF_HOST_STUB_H

#include <stdint.h>

typedef unsigned int UINT;
typedef int FRESULT;
#define FR_OK 0

typedef struct { int pos; } FIL;

FRESULT f_lseek(FIL *fp, int64_t ofs);
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);

#endif /* FF_HOST_STUB_H */
