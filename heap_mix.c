/*
 * SRAM/PSRAM 混合堆：malloc 先走 SRAM，失败自动 psram_malloc。
 * 与 localconfig.mk __CONFIG_MIX_HEAP_MANAGE 配套（链接 --wrap,malloc 等）。
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/reent.h>
#include "driver/chip/psram/psram.h"
#include "sys/sram_heap.h"
#include "kernel/os/os_thread.h"

#define RANGEOF(num, start, end) (((num) <= (end)) && ((num) >= (start)))

extern uint8_t __end__[];
extern uint8_t _estack[];

static int is_rangeof_sramheap(void *ptr)
{
    return RANGEOF((uint32_t)ptr, (uint32_t)__end__, (uint32_t)_estack);
}

static int is_rangeof_psramheap(void *ptr)
{
    return RANGEOF((uint32_t)ptr, (uint32_t)__psram_end__, PSRAM_END_ADDR - DMAHEAP_PSRAM_LENGTH);
}

static void mix_heap_lock(void)
{
    OS_ThreadSuspendScheduler();
}

static void mix_heap_unlock(void)
{
    OS_ThreadResumeScheduler();
}

void *__wrap__malloc_r(struct _reent *reent, size_t size)
{
    void *ptr;

    mix_heap_lock();
    ptr = sram_malloc_r(reent, size);
    if (ptr == NULL) {
        ptr = psram_malloc(size);
    }
    mix_heap_unlock();
    return ptr;
}

void *__wrap__realloc_r(struct _reent *reent, void *ptr, size_t size)
{
    void *new_ptr = NULL;

    mix_heap_lock();
    if (ptr == NULL) {
        new_ptr = sram_malloc_r(reent, size);
        if (new_ptr == NULL) {
            new_ptr = psram_malloc(size);
        }
    } else if (is_rangeof_sramheap(ptr)) {
        new_ptr = sram_realloc_r(reent, ptr, size);
        if (new_ptr == NULL) {
            new_ptr = psram_malloc(size);
            if (new_ptr != NULL) {
                memcpy(new_ptr, ptr, size);
                sram_free_r(reent, ptr);
            }
        }
    } else {
        new_ptr = psram_realloc(ptr, size);
    }
    mix_heap_unlock();
    return new_ptr;
}

void __wrap__free_r(struct _reent *reent, void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    mix_heap_lock();
    if (is_rangeof_sramheap(ptr)) {
        sram_free_r(reent, ptr);
    } else if (is_rangeof_psramheap(ptr)) {
        psram_free(ptr);
    } else {
        sram_free_r(reent, ptr);
    }
    mix_heap_unlock();
}

void *__wrap_malloc(size_t size)
{
    void *ptr = sram_malloc(size);
    if (ptr == NULL) {
        ptr = psram_malloc(size);
    }
    return ptr;
}

void *__wrap_realloc(void *ptr, size_t size)
{
    void *new_ptr;

    if (ptr == NULL) {
        return __wrap_malloc(size);
    }
    if (is_rangeof_sramheap(ptr)) {
        new_ptr = sram_realloc(ptr, size);
        if (new_ptr == NULL) {
            new_ptr = psram_malloc(size);
            if (new_ptr == NULL) {
                return NULL;
            }
            memcpy(new_ptr, ptr, size);
            sram_free(ptr);
        }
        return new_ptr;
    }
    return psram_realloc(ptr, size);
}

void *__wrap_calloc(size_t nmemb, size_t size)
{
    void *ptr = sram_calloc(nmemb, size);
    if (ptr == NULL) {
        ptr = psram_calloc(nmemb, size);
    }
    return ptr;
}

void __wrap_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    if (is_rangeof_sramheap(ptr)) {
        sram_free(ptr);
    } else {
        psram_free(ptr);
    }
}
