# Include base configuration
include ../mkenv.mk
include $(SDK_RTSMART_SRC_DIR)/libs/mk/toolchain_riscv64_musl.mk

# Build configuration
DIR     := $(shell basename `pwd`)
BUILD   := $(SDK_APPS_BUILD_DIR)/$(DIR)
BIN     := $(SDK_APPS_IMAGE_DIR)/$(DIR)

# Source files handling
SRC_DIRS  := .

# 自动查找当前目录下所有的 C/C++ 源码
SRCS    := $(shell find $(SRC_DIRS) -maxdepth 1 -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.cc" \))
OBJS    := $(addprefix $(BUILD)/, $(addsuffix .o, $(basename $(notdir $(SRCS)))))
DEPS    := $(OBJS:.o=.d)

# Directory creation logic
OBJ_DIRS  := $(sort $(dir $(OBJS) $(BIN)))

# SDK 库配置加载（保持原样，确保变量 LIB_CFLAGS 和 LIB_LDFLAGS 被正确填充）
include $(SDK_RTSMART_SRC_DIR)/libs/mk/libmpp.mk
include $(SDK_RTSMART_SRC_DIR)/libs/mk/libopencv.mk
include $(SDK_RTSMART_SRC_DIR)/libs/mk/libnncase.mk
include $(SDK_RTSMART_SRC_DIR)/libs/mk/librtsmart_hal.mk
include $(SDK_RTSMART_SRC_DIR)/libs/mk/lib3rdparty.mk

# --- Compiler flags ---
C_CXX_FLAGS := -fopenmp -march=rv64imafdcv -mabi=lp64d -mcmodel=medany -Os

# 头文件路径配置
C_CXX_FLAGS += -I/home/ubuntu/canmvk230/src/rtsmart/rtsmart/userapps/sdk/rt-thread/include
C_CXX_FLAGS += -I/home/ubuntu/canmvk230/src/rtsmart/rtsmart/userapps/sdk/rt-thread/components/libc/compilers/common/include
C_CXX_FLAGS += -I/home/ubuntu/canmvk230/src/rtsmart/rtsmart/userapps/sdk/rt-thread/components/libc/posix/pthreads
C_CXX_FLAGS += -I$(SDK_SRC_ROOT_DIR)/mpp/include
C_CXX_FLAGS += -I$(SDK_SRC_ROOT_DIR)/mpp/include/comm
C_CXX_FLAGS += -I. $(LIB_CFLAGS) -I$(SDK_SRC_ROOT_DIR)/include -include "generated/autoconf.h"
C_CXX_FLAGS += -Wall -Wextra -ffunction-sections -fdata-sections

CFLAGS      := $(C_CXX_FLAGS) -std=gnu99
CXX_FLAGS   := $(C_CXX_FLAGS)

# --- Linker flags ---
LDFLAGS := -T $(SDK_RTSMART_SRC_DIR)/libs/mk/link.lds --static -Wl,--gc-sections
LDFLAGS += $(LIB_LDFLAGS)

# 1. 基础运行时库路径
LDFLAGS += -L/home/ubuntu/canmvk230/src/rtsmart/rtsmart/userapps/sdk/rt-thread/lib/risc-v/rv64

# 2. 【关键】添加 libsys.a 所在的绝对路径
LDFLAGS += -L/home/ubuntu/canmvk230/src/rtsmart/mpp/userapps/lib

# 3. 【关键】链接库定义
# 顺序：RT-Thread核心 -> 推理引擎模块 -> 系统底层驱动(sys) -> C++与数学库
# -lsys 必须在 -lnncase 之后，因为 nncase 依赖 sys 提供的 MMZ 缓存函数
LOCAL_LIBS := -lrtthread -lsys -lpthread -lstdc++ -lm

# Phony targets
.PHONY: all clean distclean

all: $(BIN)
	@$(ECHO) [BUILD] applications $(DIR) done.

.PHONY: .FORCE
.FORCE:

# --- 链接规则 ---
$(BIN): $(OBJS) .FORCE | $(OBJ_DIRS)
	@echo "[LD] $@"
	# 核心顺序：CXX + OBJS + LDFLAGS(包含路径) + LOCAL_LIBS(包含具体库)
	$(Q)$(CXX) $(CXX_FLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LOCAL_LIBS)
	$(Q)$(STRIP) $@

# --- 编译规则 ---
vpath %.c $(SRC_DIRS)
$(BUILD)/%.o: %.c | $(OBJ_DIRS)
	@echo "[CC] $@"
	$(Q)$(CC) $(CFLAGS) -MD -MP -MF $(@:.o=.d) -c $< -o $@

vpath %.cc $(SRC_DIRS)
$(BUILD)/%.o: %.cc | $(OBJ_DIRS)
	@echo "[CXX] $@"
	$(Q)$(CXX) $(CXX_FLAGS) -MD -MP -MF $(@:.o=.d) -c $< -o $@

vpath %.cpp $(SRC_DIRS)
$(BUILD)/%.o: %.cpp | $(OBJ_DIRS)
	@echo "[CXX] $@"
	$(Q)$(CXX) $(CXX_FLAGS) -MD -MP -MF $(@:.o=.d) -c $< -o $@

# 目录创建
$(OBJ_DIRS):
	$(Q)mkdir -p $@

clean:
	@rm -rf $(BIN)

distclean: clean
	@rm -rf $(BUILD)

-include $(DEPS)