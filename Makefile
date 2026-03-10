#****************************************************************************
#
# Makefile for benchmark_zrlog
# bolide zhang
# bolidezhang@gmail.com
#
# This is a GNU make (gmake) makefile
#****************************************************************************

# DEBUG can be set to YES to include debugging info, or NO otherwise
DEBUG          := NO

# PROFILE can be set to YES to include profiling info, or NO otherwise
PROFILE        := NO

# USE_STL can be used to turn on STL support. NO, then STL
# will not be used. YES will include the STL files.
USE_STL := YES

# WIN32_ENV
WIN32_ENV := YES
#****************************************************************************

CC     := gcc
CXX    := g++
LD     := g++
AR     := ar rc
RANLIB := ranlib

# ifeq (YES, ${WIN32_ENV})
#   RM     := del
# else
#   RM     := rm -f
# endif

DEBUG_CFLAGS     := -Wall -Wno-format -g -DDEBUG
RELEASE_CFLAGS   := -Wall -Wno-unknown-pragmas -Wno-format -O3

DEBUG_CXXFLAGS   := ${DEBUG_CFLAGS}
RELEASE_CXXFLAGS := ${RELEASE_CFLAGS}

DEBUG_LDFLAGS    := -g
RELEASE_LDFLAGS  := -O3

ifeq (YES, ${DEBUG})
   CFLAGS       := ${DEBUG_CFLAGS}
   CXXFLAGS     := ${DEBUG_CXXFLAGS}
   LDFLAGS      := ${DEBUG_LDFLAGS}
else
   CFLAGS       := ${RELEASE_CFLAGS}
   CXXFLAGS     := ${RELEASE_CXXFLAGS}
   LDFLAGS      := ${RELEASE_LDFLAGS}
endif

ifeq (YES, ${PROFILE})
   CFLAGS   := ${CFLAGS} -pg -O3
   CXXFLAGS := ${CXXFLAGS} -pg -O3
   LDFLAGS  := ${LDFLAGS} -pg
endif

#****************************************************************************
# Preprocessor directives
#****************************************************************************

ifeq (YES, ${USE_STL})
  DEFS := -DUSE_STL
else
  DEFS :=
endif

#****************************************************************************
# Include paths
#****************************************************************************

# 注意：fmt 是 header-only 库，只需添加头文件路径。
# 如果你的 fmt 头文件直接位于 ./fmt 下（即 ./fmt/fmt/core.h），请将路径改为 -I./fmt
INCS := -I./ -I./fmt/include -I/usr/local/include

LIBS := -L/usr/lib -lpthread

#****************************************************************************
# 新增：Release 模式下的额外优化选项（仅在非 DEBUG 时生效）
#****************************************************************************
ifneq (YES, ${DEBUG})
    # -march=native : 针对当前 CPU 架构自动启用最高指令集（如 AVX2、SSE4.2）
    # -flto        : 链接时优化，允许跨文件内联、消除无用代码，需同时用于编译和链接
    CFLAGS   += -march=native -flto
    CXXFLAGS += -march=native -flto
    LDFLAGS  += -flto
endif

#****************************************************************************
# Makefile code common to all platforms
#****************************************************************************

CFLAGS   := ${CFLAGS}   ${DEFS}
CXXFLAGS := ${CXXFLAGS} ${DEFS}

#****************************************************************************
# Targets of the build
#****************************************************************************

OUTPUT := benchmark_zrlog

all: ${OUTPUT}


#****************************************************************************
# Source files
#****************************************************************************

SRCS := benchmark_zrlog.cpp 

# Add on the sources for libraries
SRCS := ${SRCS}

OBJS := $(addsuffix .o,$(basename ${SRCS}))

#****************************************************************************
# Output
#****************************************************************************

${OUTPUT}: ${OBJS}
	${LD} -o $@ ${LDFLAGS} ${OBJS} ${LIBS} ${EXTRA_LIBS}
#****************************************************************************
# common rules
#****************************************************************************

# Rules for compiling source files to object files
%.o : %.cpp
	${CXX} -c -std=c++17 ${CXXFLAGS} ${INCS} $< -o $@

%.o : %.c
	${CC} -c -std=c17 ${CFLAGS} ${INCS} $< -o $@

dist:
	bash makedistlinux

clean:
	${RM} core ${OBJS} ${OUTPUT}

depend:
	#makedepend ${INCS} ${SRCS}

%.o: %.h