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
export __CONFIG_DMAHEAP_PSRAM_SIZE := 384

# LVGL source path for include
export PRJ_EXTRA_INCLUDE := -I../../demo/LVGL_withSDandNetwork/lvgl/src -I../../demo/LVGL_withSDandNetwork/third_party/miniz
