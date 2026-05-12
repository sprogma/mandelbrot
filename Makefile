CLANG ?= clang

DEBUG = -g
# DEBUG += -fsanitize=address

CFLAGS = -std=gnu2y -O3 -march=native -fms-extensions -Wno-microsoft -Wno-format $(DEBUG)

PKGS = sdl3 vulkan libavcodec libavutil libswscale libavformat
LDFLAGS = $(shell pkg-config --libs $(PKGS)) -lm $(DEBUG)

OPT = -O3 -all-resources-bound
DXC_FLAGS = -Wall -Wextra -spirv -T cs_6_0 -E main $(OPT)

SRCS = $(filter-out src/lli_test.c, $(wildcard src/*.c))
OBJS = $(SRCS:.c=.o)

.PHONY: all clean test shaders

all: frac.exe test shaders

frac.exe: $(OBJS)
	$(CLANG) $(OBJS) -o frac.exe $(LDFLAGS)

%.o: %.c
	$(CLANG) -c $< -o $@ $(CFLAGS)

test: lli_test.c
	@$(CLANG) lli_test.c -o test.exe -lm $(filter-out -O3, $(CFLAGS))
	@./test.exe

shaders: kernel.hlsl
	@echo "Warning: building of shaders may fail. Use precompiled then."
	@dxc $(DXC_FLAGS) kernel.hlsl -Fo kernel.spv && \
	 spirv-opt -O kernel.spv -o kernel_opt.spv && \
	 dxc -DFLOAT64 $(DXC_FLAGS) -fspv-target-env=vulkan1.1 kernel.hlsl -Fo kernel64.spv && \
	 spirv-opt -O kernel64.spv -o kernel64_opt.spv && \
	 dxc -DFLOATFLOAT $(DXC_FLAGS) -fspv-target-env=vulkan1.1 kernel.hlsl -Fo kernelff.spv && \
	 spirv-opt -O kernelff.spv -o kernelff_opt.spv || \
	 (echo "Warning: Using precompiled shaders"; cp spv/* .)

clean:
	rm -f *.o frac.exe test.exe *.spv

