/*
 * core_portme.h - XR872 CoreMark Port Configuration
 *
 * Ported for XR872 (ARM Cortex-M4F) with FreeRTOS
 * - Uses standard printf for output
 * - Uses FreeRTOS OS_GetTicks() for timing
 * - Uses malloc/free for memory allocation
 * - Single threaded
 */

#ifndef CORE_PORTME_H
#define CORE_PORTME_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/************************/
/* Data types           */
/************************/
typedef signed short       ee_s16;
typedef unsigned short     ee_u16;
typedef signed int         ee_s32;
typedef unsigned int       ee_u32;
typedef signed long long   ee_s64;
typedef unsigned long long ee_u64;
typedef float              ee_f32;
typedef double             ee_f16;
typedef unsigned char      ee_u8;
typedef ee_u32             ee_ptr_int;
typedef size_t             ee_size_t;

#define NULL ((void *)0)

/* align to 32-bit boundary */
#define align_mem(x) (void *)(4 + (((ee_ptr_int)(x)-1) & ~3))

/************************/
/* Platform settings    */
/************************/
#define HAS_FLOAT       1
#define HAS_TIME_H      0
#define USE_CLOCK       0
#define HAS_STDIO       1
#define HAS_PRINTF      1

#define COMPILER_VERSION "GCC arm-none-eabi 4.9.3"
#define COMPILER_FLAGS   "-O2 -mcpu=cortex-m4 -mthumb"
#define MEM_LOCATION     "Heap"

/************************/
/* Timing               */
/************************/
#define CORETIMETYPE ee_u32
typedef ee_u32 CORE_TICKS;

/************************/
/* Seed method          */
/************************/
#define SEED_METHOD SEED_VOLATILE

/************************/
/* Memory method        */
/************************/
#define MEM_METHOD MEM_MALLOC

/************************/
/* Threading            */
/************************/
#define MULTITHREAD 1
#define USE_PTHREAD 0
#define USE_FORK    0
#define USE_SOCKET  0

/************************/
/* Main configuration   */
/************************/
#define MAIN_HAS_NOARGC   1
#define MAIN_HAS_NORETURN 0

/************************/
/* Default contexts     */
/************************/
extern ee_u32 default_num_contexts;

/************************/
/* Portable structure   */
/************************/
typedef struct CORE_PORTABLE_S {
    ee_u8 portable_id;
} core_portable;

/************************/
/* Function prototypes  */
/************************/
void portable_init(core_portable *p, int *argc, char *argv[]);
void portable_fini(core_portable *p);

/************************/
/* Run mode             */
/************************/
#define PERFORMANCE_RUN 1

#endif /* CORE_PORTME_H */