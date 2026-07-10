# ============================================================
#  物联网设备端程序
# ============================================================
TARGET := device_app

# ============================================================
#  编译器
# ============================================================
CC  := gcc
CXX := g++

# ============================================================
#  头文件搜索路径（所有子目录均加入 -I，源代码中的 #include "xxx.h" 无需修改）
# ============================================================
INC_DIRS := . \
            core \
            network \
            protocol \
            devices/ptz \
            devices/led \
            devices/speaker \
            devices/alarm \
            devices/mood \
            devices/system \
            config \
            thirdparty/cjson \
            thirdparty/led

# ============================================================
#  源文件（递归收集所有 .c 和 .cpp）
# ============================================================
C_SRCS   := $(wildcard *.c */*.c */*/*.c */*/*/*.c)
CXX_SRCS := $(wildcard *.cpp */*.cpp */*/*.cpp */*/*/*.cpp)
C_OBJS   := $(C_SRCS:.c=.o)
CXX_OBJS := $(CXX_SRCS:.cpp=.o)
OBJS     := $(C_OBJS) $(CXX_OBJS)
DEPS     := $(C_OBJS:.o=.d) $(CXX_OBJS:.o=.d)

# ============================================================
#  编译选项
# ============================================================
CFLAGS    := -Wall -Wextra -O2 -g -pthread -DMG_TLS=3 $(addprefix -I,$(INC_DIRS))
CXXFLAGS  := -Wall -Wextra -O2 -g -pthread -DMG_TLS=3 $(addprefix -I,$(INC_DIRS))

# LED SDK 库（灵信视觉 ledplayer7）
LED_LIB_DIR  := ./thirdparty/led
LED_LIBS     := -L$(LED_LIB_DIR) -lledplayer7 -Wl,-rpath=$(LED_LIB_DIR)

LDFLAGS := -lpthread -lasound -lm -ldl $(LED_LIBS)

# ============================================================
#  默认目标
# ============================================================
.PHONY: all clean

all: $(TARGET)
	@echo "========================================="
	@echo "  编译完成: ./$(TARGET)"
	@echo "========================================="

$(TARGET): $(OBJS)
	$(CXX) $^ -o $@ $(LDFLAGS)

# 子目录下的 .c → 同目录 .o
%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# mongoose 第三方库：抑制 -Wmaybe-uninitialized 假阳性警告
network/mongoose.o: network/mongoose.c
	$(CC) $(CFLAGS) -Wno-maybe-uninitialized -MMD -MP -c $< -o $@

# 子目录下的 .cpp → 同目录 .o
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)
	@echo "已清理编译产物"

-include $(DEPS)
