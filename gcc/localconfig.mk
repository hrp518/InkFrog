#
# project local config options, override the global config options
#

# ----------------------------------------------------------------------------
# override global config options
# ----------------------------------------------------------------------------
# enable/disable XIP, default to y
export __CONFIG_XIP := y

# enable/disable OTA, default to n
export __CONFIG_OTA := y

# enable/disable PSRAM, default to n
export __CONFIG_PSRAM := y

# PSRAM chip type for OPI32 mode
export __CONFIG_PSRAM_CHIP_OPI32 := y

# Make PSRAM all cacheable to skip DMAHEAP_PSRAM_LENGTH dependency
export __CONFIG_PSRAM_ALL_CACHEABLE := y

# PSRAM DMA Heap size (KB) - 扩大给EPUB章节缓存使用
export __CONFIG_DMAHEAP_PSRAM_SIZE := 1024

# SRAM 堆仅 ~95KB，线程栈+WLAN 极易耗尽；以下模块改用 PSRAM 堆
export __CONFIG_MBUF_HEAP_MODE := 1
export __CONFIG_UMAC_HEAP_MODE := 1
export __CONFIG_LMAC_HEAP_MODE := 1
# malloc 先 SRAM，不足时自动 psram_malloc（网络 pbuf/小包不再 heap exhausted）
export __CONFIG_MIX_HEAP_MANAGE := y

# LVGL source path for include
export PRJ_EXTRA_INCLUDE := -I../../demo/FontExp/lvgl/src -I../../demo/FontExp/third_party/miniz -I../../demo/FontExp/third_party/expat
