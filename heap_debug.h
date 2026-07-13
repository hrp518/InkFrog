#ifndef HEAP_DEBUG_H
#define HEAP_DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif

void print_heap_info(void);
void psram_heap_info(void);
void dma_heap_info(void);
uint32_t sram_heap_free_bytes(void);

#ifdef __cplusplus
}
#endif

#endif
