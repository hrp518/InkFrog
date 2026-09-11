#
# Rules for building application
#

# ----------------------------------------------------------------------------
# project local config
# ----------------------------------------------------------------------------
include localconfig.mk

# ----------------------------------------------------------------------------
# common rules
# ----------------------------------------------------------------------------
ROOT_PATH := ../../../..

include $(ROOT_PATH)/gcc.mk

# 工程侧放宽警告 (gcc.mk 保持原版, 不再打补丁)
CC_FLAGS += -Wno-error

# ----------------------------------------------------------------------------
# project name and project root path
# ----------------------------------------------------------------------------
PROJECT := $(notdir $(shell cd .. && pwd))
PRJ_ROOT_PATH := $(ROOT_PATH)/project/demo/$(PROJECT)

# ----------------------------------------------------------------------------
# board path, including board_config.c and board_config.h
# 本工程使用工程内 board 目录 (384M 超频等板级定制), 不改 SDK 公共板级
# ----------------------------------------------------------------------------
ifeq ($(__CONFIG_CHIP_TYPE), xr872)
PRJ_BOARD := $(PRJ_ROOT_PATH)/board
else ifeq ($(__CONFIG_CHIP_TYPE), xr808)
PRJ_BOARD := $(ROOT_PATH)/project/common/board/xr808_evb_io
else
PRJ_BOARD := null
endif

# ----------------------------------------------------------------------------
# objects
# ----------------------------------------------------------------------------
INCLUDE_PATHS += -I$(PRJ_ROOT_PATH)
INCLUDE_PATHS += -I$(PRJ_ROOT_PATH)/lvgl/src
INCLUDE_PATHS += -I$(PRJ_ROOT_PATH)/coremark

DIRS_IGNORE := $(PRJ_ROOT_PATH)/gcc%
DIRS_IGNORE += $(PRJ_ROOT_PATH)/mkfs_ff%   # f_mkfs 独立模块 (规则单独编译), 不进工程源码扫描
DIRS_IGNORE += $(PRJ_ROOT_PATH)/image%
DIRS_IGNORE += $(PRJ_ROOT_PATH)/example%
DIRS_IGNORE += $(PRJ_ROOT_PATH)/lvgl/demos%
DIRS_IGNORE += $(PRJ_ROOT_PATH)/lvgl/examples%
DIRS_IGNORE += $(PRJ_ROOT_PATH)/LVGL_withSDandNetwork%
DIRS_IGNORE += $(PRJ_ROOT_PATH)/FontExp_prev%
DIRS_IGNORE += $(ROOT_PATH)/project/common/board/%
DIRS_IGNORE += $(PRJ_BOARD)

DIRS_ALL := $(shell find $(PRJ_ROOT_PATH) $(ROOT_PATH)/project/common -type d)
DIRS := $(filter-out $(DIRS_IGNORE),$(DIRS_ALL))
DIRS += $(PRJ_BOARD)

SRCS := $(basename $(foreach dir,$(DIRS),$(wildcard $(dir)/*.[csS])))

# 主机单测只在 PC 上编译运行（gcc -I host_stub ...），不进固件
SRCS := $(filter-out $(PRJ_ROOT_PATH)/tools/test_gbk,$(SRCS))

# Browser L1 glyf JS blob (Flash .rodata, zero runtime RAM)
GEN_L1GLYF_JS := $(PRJ_ROOT_PATH)/http_l1glyf_js.c
$(GEN_L1GLYF_JS): $(PRJ_ROOT_PATH)/web/l1glyf_builder.js $(PRJ_ROOT_PATH)/level1_data.c $(PRJ_ROOT_PATH)/tools/gen_l1glyf_web_js.py
	@python $(PRJ_ROOT_PATH)/tools/gen_l1glyf_web_js.py 2>/dev/null || py -3 $(PRJ_ROOT_PATH)/tools/gen_l1glyf_web_js.py

# fs_ctrl 已整体工程化 (含 fs_ctrl_format), 排除 SDK 原版避免重复符号
SRCS := $(filter-out $(ROOT_PATH)/project/common/framework/fs_ctrl,$(SRCS))
OBJS := $(addsuffix .o,$(SRCS))

# extra libraries searching path
PRJ_EXTRA_LIBS_PATH := 
 
# extra libraries - 不使用liblvgl.a
PRJ_EXTRA_LIBS :=

# ----------------------------------------------------------------------------
# 链接器响应文件 - 解决467个目标文件导致的"Argument list too long"问题
# ----------------------------------------------------------------------------
RESPONSE_FILE := objects_response.txt

# 响应文件生成规则
$(RESPONSE_FILE): $(OBJS)
	@echo "Creating response file with $(words $(OBJS)) object files..."
	@rm -f $(RESPONSE_FILE)
	@$(foreach obj,$(OBJS),echo $(obj) >> $(RESPONSE_FILE);)

# image.cfg relative to ../image/xr872/ — 8MB dual-bank (2044K / OTA@2048K)
# 用 mkimage 自动重排的 image_auto_cal.cfg: 固件体积增长后原 image.cfg 的固定偏移
# 会出现 app/app_xip 段重叠 (mkimage 报 "bin were overlaped" 并自动生成重排版),
# 改读重排版即可直接出图 (与 FontExp_gen1 同款做法)。
IMAGE_CFG := image_auto_cal.cfg

# 本项目专用链接脚本: SDK 共享 appos.ld 的 XIP/PSRAM 段白名单按
# *project/demo/FontExp/ 路径通配, 属本工程专属布局, 随工程携带 (解耦),
# 不再改动 SDK 公共链接脚本。project.mk 中 LINKER_SCRIPT 为 ?= 赋值,
# 此处占位即可覆盖。
LINKER_SCRIPT := $(PRJ_ROOT_PATH)/gcc/appos.ld

# SDK bugfix 链接期外移, 实现见工程 sdk_fixes.c (SD 容量换算 + HIBERNATION 保持 32K 校准)
LD_FLAGS += -Wl,--wrap,SDMMC_ioctl -Wl,--wrap,HAL_PRCM_DisableInter32KCalib

# f_mkfs 独立模块: FatFs 原版源文件 + _USE_MKFS=1 副本, objcopy 只保留
# f_mkfs 全局符号单独链接。SDK libfs/ffconf.h 保持原版 (_USE_MKFS=0),
# SDK 零改动 (原版 sdmmc_diskio.c 的 mkfs 分支有 case 后声明的编译错误,
# _USE_MKFS=0 时被预处理器裁掉不会触发; 本模块绕开整段)。
MKFS_DIR := $(PRJ_ROOT_PATH)/mkfs_ff
MKFS_OBJ := $(MKFS_DIR)/f_mkfs.o
$(MKFS_OBJ): $(MKFS_DIR)/ff.c $(MKFS_DIR)/fs/fatfs/ff.h $(MKFS_DIR)/fs/fatfs/ffconf.h
	$(Q)$(CC) $(CPU) -c $(DBG_FLAG) -fno-common -fmessage-length=0 -fno-exceptions -ffunction-sections -fdata-sections -fomit-frame-pointer -Os -std=gnu99 -I$(MKFS_DIR) -I$(ROOT_PATH)/include -o $@ $(MKFS_DIR)/ff.c
	$(Q)$(OBJCOPY) --keep-global-symbol=f_mkfs $@ $@.tmp && mv $@.tmp $@

# project make rules - 必须在响应文件规则之后include
include $(PRJ_MAKE_RULES)

# ----------------------------------------------------------------------------
# 覆盖project.mk中的链接规则，使用响应文件
# ----------------------------------------------------------------------------
$(PROJECT).$(ELF_EXT): $(GEN_L1GLYF_JS) $(OBJS) $(RESPONSE_FILE) $(MKFS_OBJ)
	@echo "Linking $@..."
ifeq ($(__CONFIG_ROM), y)
	$(Q)$(CC) -E -P -CC $(CC_SYMBOLS) -o $(ROM_SYMBOL_NAME) - < $(ROM_SYMBOL_FILE)
endif
	$(Q)$(CC) -E -P -CC $(CC_SYMBOLS) -o $(PROJECT_LD) - < $(LINKER_SCRIPT) && \
	$(Q)$(CC) $(LD_FLAGS) -T$(PROJECT_LD) $(LIBRARY_PATHS) -o $@ @$(RESPONSE_FILE) $(MKFS_OBJ) $(LIBRARIES)
	@echo "Linking $@...done"

# ----------------------------------------------------------------------------
# Per-file 优化: stbtt 字形光栅化是浮点密集型(M4F FPU), 全局 -Os 太保守。
# 单独给 lv_tiny_ttf.c 开 -O3 -ffast-math:
#   -O3              自动循环展开/向量化, stbtt 内层循环受益最大
#   -ffast-math      放松 IEEE 精度约束(坐标/缩放计算无影响), 让 FPU 跑满
#   -fno-trapping-math 告诉编译器无浮点陷阱, 避免拉入 abort()->_exit() 链
#                     (否则 -ffast-math 会引入浮点异常处理路径, 链接时缺 stubs)
#   保留 softfp ABI, 和 SDK 其余模块/预编译库 ABI 兼容, 安全。
# 实测单字渲染 100-200ms -> 预期 40-80ms。
# ----------------------------------------------------------------------------
TINY_TTF_SRC := $(PRJ_ROOT_PATH)/lvgl/src/extra/libs/tiny_ttf/lv_tiny_ttf.c
TINY_TTF_OBJ := $(PRJ_ROOT_PATH)/lvgl/src/extra/libs/tiny_ttf/lv_tiny_ttf.o

$(TINY_TTF_OBJ): $(TINY_TTF_SRC)
	@echo "[FPU-OPT] Compiling $< with -O3 -ffast-math"
	$(Q)$(CC) $(CPU) -c $(DBG_FLAG) -fno-common -fmessage-length=0 \
		-fno-exceptions -ffunction-sections -fdata-sections -fomit-frame-pointer \
		-Wall -Wpointer-arith -Wno-error -MMD -MP \
		-O3 -ffast-math -fno-trapping-math \
		-std=gnu99 $(CC_SYMBOLS) $(INCLUDE_PATHS) -o $@ $<
