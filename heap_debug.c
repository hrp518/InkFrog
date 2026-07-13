/*
 * Heap调试信息工具
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/dma_heap.h>
#include "driver/chip/psram/psram.h"

/* SRAM堆信息 - 使用_sbrk的逻辑 */
/* PRJCONF_MSP_STACK_SIZE = 1KB，从prj_config.h获取 */
extern char __end__;      /* heap start address */
extern char _estack;      /* stack end address */
extern char __HeapLimit;  /* from linker script .heap section */

/* 打印堆使用情况 - 修正：使用_sbrk相同的逻辑计算堆大小 */
extern void *_sbrk(int incr);
extern void heap_get_space(uint8_t **start, uint8_t **end, uint8_t **current);

uint32_t sram_heap_free_bytes(void)
{
    #define MSP_STACK_SIZE 1024
    uint8_t *start;
    uint8_t *end;
    uint8_t *cur;

    heap_get_space(&start, &end, &cur);
    if (!start || !end || !cur || cur > end) {
        return 0;
    }
    (void)MSP_STACK_SIZE;
    return (uint32_t)(end - cur);
}

void print_heap_info(void) {
    /* MSP栈大小 = 1KB (1024 bytes) - 来自prj_config.h: PRJCONF_MSP_STACK_SIZE */
    #define MSP_STACK_SIZE 1024

    uint32_t heap_start = (uint32_t)&__end__;
    uint32_t heap_end = (uint32_t)&_estack - MSP_STACK_SIZE;
    uint32_t heap_total = heap_end - heap_start;
    uint32_t heap_used = 0;
    uint32_t heap_free = 0;
    void *brk = _sbrk(0);

    if (brk != (void *)-1 && (uint32_t)brk >= heap_start && (uint32_t)brk <= heap_end) {
        heap_used = (uint32_t)brk - heap_start;
        heap_free = heap_end - (uint32_t)brk;
    }

    printf("SRAM Heap Info:\r\n");
    printf("  __end__ (heap start):      0x%08X\r\n", heap_start);
    printf("  _estack-1KB (heap end):    0x%08X\r\n", heap_end);
    printf("  __HeapLimit:               0x%08X\r\n", (unsigned)&__HeapLimit);
    printf("  Total heap space:         %u bytes\r\n", heap_total);
    if (heap_free || heap_used) {
        printf("  Used (sbrk):              %u bytes\r\n", heap_used);
        printf("  Free (sbrk):              %u bytes\r\n", heap_free);
    }
    printf("  Note: .heap section is COPY type, actual heap via _sbrk()\r\n");
}

/* PSRAM堆信息 */
void psram_heap_info(void) {
    printf("PSRAM Info:\r\n");
    printf("  PSRAM Base: 0x%08X\r\n", (unsigned)__PSRAM_BASE);
    printf("  PSRAM Size: %u bytes\r\n", (unsigned)__PSRAM_LENGTH);
    printf("  psram_heap free: %u bytes\r\n", (unsigned)psram_GetFreeHeapSize());
}

/* DMA Heap信息 */
void dma_heap_info(void) {
    printf("DMA Heap (PSRAM) Info:\r\n");
    printf("  Base: 0x%08X\r\n", (unsigned)__DMAHEAP_PSRAM_BASE);
    printf("  Size: %u KB\r\n", (unsigned)__DMAHEAP_PSRAM_LENGTH / 1024);
}
