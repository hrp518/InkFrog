#ifndef MEM_GUARD_H
#define MEM_GUARD_H

#include <stdint.h>
#include <stddef.h>

void mem_guard_init(void);
void mem_guard_check_timer_list(const char* tag);

#endif
