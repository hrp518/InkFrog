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

DIRS_IGNORE := $(PRJ_ROOT_PATH)/gcc%
DIRS_IGNORE += $(PRJ_ROOT_PATH)/image%
DIRS_IGNORE += $(PRJ_ROOT_PATH)/example%
DIRS_IGNORE += $(ROOT_PATH)/project/common/board/%
DIRS_IGNORE += $(PRJ_BOARD)

DIRS_ALL := $(shell find $(PRJ_ROOT_PATH) $(ROOT_PATH)/project/common -type d)
DIRS := $(filter-out $(DIRS_IGNORE),$(DIRS_ALL))
DIRS += $(PRJ_BOARD)

SRCS := $(basename $(foreach dir,$(DIRS),$(wildcard $(dir)/*.[csS])))

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

# project make rules - 必须在响应文件规则之后include
include $(PRJ_MAKE_RULES)

# ----------------------------------------------------------------------------
# 覆盖project.mk中的链接规则，使用响应文件
# ----------------------------------------------------------------------------
$(PROJECT).$(ELF_EXT): $(OBJS) $(RESPONSE_FILE)
	@echo "Linking $@..."
ifeq ($(__CONFIG_ROM), y)
	$(Q)$(CC) -E -P -CC $(CC_SYMBOLS) -o $(ROM_SYMBOL_NAME) - < $(ROM_SYMBOL_FILE)
endif
	$(Q)$(CC) -E -P -CC $(CC_SYMBOLS) -o $(PROJECT_LD) - < $(LINKER_SCRIPT) && \
	$(Q)$(CC) $(LD_FLAGS) -T$(PROJECT_LD) $(LIBRARY_PATHS) -o $@ @$(RESPONSE_FILE) $(LIBRARIES)
	@echo "Linking $@...done"
