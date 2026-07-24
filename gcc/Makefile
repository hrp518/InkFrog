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

# ----------------------------------------------------------------------------
# project name and project root path
# ----------------------------------------------------------------------------
PROJECT := $(notdir $(shell cd .. && pwd))
PRJ_ROOT_PATH := $(ROOT_PATH)/project/demo/$(PROJECT)

# ----------------------------------------------------------------------------
# board path, including board_config.c and board_config.h
# ----------------------------------------------------------------------------
ifeq ($(__CONFIG_CHIP_TYPE), xr872)
PRJ_BOARD := $(ROOT_PATH)/project/common/board/xr872_evb_ai
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

# Browser L1 glyf JS blob (Flash .rodata, zero runtime RAM)
GEN_L1GLYF_JS := $(PRJ_ROOT_PATH)/http_l1glyf_js.c
$(GEN_L1GLYF_JS): $(PRJ_ROOT_PATH)/web/l1glyf_builder.js $(PRJ_ROOT_PATH)/level1_data.c $(PRJ_ROOT_PATH)/tools/gen_l1glyf_web_js.py
	@python $(PRJ_ROOT_PATH)/tools/gen_l1glyf_web_js.py 2>/dev/null || py -3 $(PRJ_ROOT_PATH)/tools/gen_l1glyf_web_js.py

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
IMAGE_CFG := image.cfg

# project make rules - 必须在响应文件规则之后include
include $(PRJ_MAKE_RULES)

# ----------------------------------------------------------------------------
# 覆盖project.mk中的链接规则，使用响应文件
# ----------------------------------------------------------------------------
$(PROJECT).$(ELF_EXT): $(GEN_L1GLYF_JS) $(OBJS) $(RESPONSE_FILE)
	@echo "Linking $@..."
ifeq ($(__CONFIG_ROM), y)
	$(Q)$(CC) -E -P -CC $(CC_SYMBOLS) -o $(ROM_SYMBOL_NAME) - < $(ROM_SYMBOL_FILE)
endif
	$(Q)$(CC) -E -P -CC $(CC_SYMBOLS) -o $(PROJECT_LD) - < $(LINKER_SCRIPT) && \
	$(Q)$(CC) $(LD_FLAGS) -T$(PROJECT_LD) $(LIBRARY_PATHS) -o $@ @$(RESPONSE_FILE) $(LIBRARIES)
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
