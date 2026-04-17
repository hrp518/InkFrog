# Memory Layout Guide (manual lookup)

## Static / Global large objects

| Symbol / Object | File | Size | Memory target | Notes |
|---|---|---:|---|---|
| `lv_draw_buf` | `lv_port_disp.c` | 240*415 bytes ~= 99600B | `.psram_bss` | LVGL full draw buffer |
| `framebuffer` | `epd.c` | 12450B | SRAM/BSS | EPD packed framebuffer |
| `EpubViewer.html_buf` | `epub_viewer.c` | 8192B | PSRAM via `_dma_malloc` | HTML chunk |
| `EpubViewer.stripped_buf` | `epub_viewer.c` | 16384B | PSRAM via `_dma_malloc` | stripped HTML |
| `EpubViewer.decoded_buf` | `epub_viewer.c` | 16384B | PSRAM via `_dma_malloc` | decoded canonical chunk |
| `EpubViewer.reflowed_buf` | `epub_viewer.c` | 16384B | PSRAM via `_dma_malloc` | per-block text |
| `EpubViewer.page_text_buf` | `epub_viewer.c` | 16384B | PSRAM via `_dma_malloc` | page slice / temp work buffer |
| `file_entries` | `file_manager.c` | 500 * sizeof(FileEntry) | PSRAM via `psram_malloc` | file manager pagination |
| `s_ttf_data` | `file_manager.c` | TTF file size | PSRAM via `psram_malloc` | raw chosen TTF data |
| `custom_ttf_font*` | `file_manager.c` | dynamic | LVGL heap / internal | font objects, still pressure SRAM |

## Threads / stacks

| Thread | Create site | Stack size | Responsibility |
|---|---|---:|---|
| `disp_task` | `main.c` | 8192 | display refresh / EPD refresh |
| `lvgl_task` | `main.c` | 32768 | `lv_timer_handler`, UI callbacks, FM actions, EPUB open path |
| `http_server` | `http_server.c` | see source | HTTP service |

## Important memory facts

- SRAM heap from boot log: ~95236 bytes
- PSRAM heap from boot log: ~3720768 bytes
- Major risk is not just buffer storage, but temporary SRAM allocations during:
  - FATFS open/read/dir traversal
  - Tiny TTF font object creation
  - LVGL object tree creation
  - task stacks

## Current architecture pressure points

1. FM init creates screen/list/labels in LVGL heap (SRAM pressure)
2. FM TTF path discovery is cheap-ish, but full TTF object creation is expensive in SRAM metadata/cache
3. EPUB viewer allocates ~72KB buffers in PSRAM, which is okay, but opening EPUB happens on `lvgl_task`
4. `lvgl_task` also carries callback depth, making stack pressure visible there
