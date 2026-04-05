#include "mem_guard.h"
#include "lvgl/src/misc/lv_timer.h"
#include "lvgl/src/misc/lv_ll.h"
#include "lvgl/src/misc/lv_gc.h"
#include <stdio.h>

void mem_guard_init(void)
{
    printf("[MEM_GUARD] Initialized\r\n");
}

void mem_guard_check_timer_list(const char* tag)
{
    lv_timer_t* head = lv_timer_get_next(NULL);
    printf("[MEM_GUARD] [%s] timer list head = 0x%08X\r\n", tag, (uint32_t)head);
    
    if (head != NULL) {
        lv_timer_t* next = lv_timer_get_next(head);
        void* prev = _lv_ll_get_prev(&LV_GC_ROOT(_lv_timer_ll), head);
        printf("[MEM_GUARD] [%s] head->next = 0x%08X, head->prev = 0x%08X\r\n", 
               tag, (uint32_t)next, (uint32_t)prev);
    }
    
    if (head != NULL && ((uint32_t)head < 0x01000000 || (uint32_t)head > 0x01800000)) {
        printf("[MEM_GUARD] ERROR: Invalid timer head pointer 0x%08X!\r\n", (uint32_t)head);
    }
}
