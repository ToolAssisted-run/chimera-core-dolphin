# Guest driver objects: the same adapter sources as native.mk, compiled with
# the miniBox musl toolchain and the flags CMake gave the guest libraries.
# build-guest.sh must have run first (build/guest holds the archives and
# compile_commands.json); build-core.sh links core.wbx from all of it.
#
# Usage: make -f guest.mk -j$(nproc)

ROOT   := ..
B      := $(ROOT)/build/guest
O      := obj-guest
MB     ?= $(HOME)/chimera/extern/chimera-common-minibox
MBUILD := $(MB)/build/meson-cpp
SR     := $(MBUILD)/guest-sysroot
GCCVER := $(shell gcc -dumpfullversion)

TUFLAGS := $(shell python3 extract-tu-flags.py $(B)/compile_commands.json Core/Core.cpp)

WBFLAGS := -fvisibility=hidden -mcmodel=large -mstack-protector-guard=global -fno-stack-protector \
        -fno-pic -fno-pie -fcf-protection=none -O2
SPECS   := -specs $(SR)/lib/musl-gcc.specs
CXXINCS := -nostdinc++ -I$(SR)/include/c++/$(GCCVER) -I$(SR)/include/c++/$(GCCVER)/x86_64-linux-musl
MBINCS  := -I$(MB)/extern/emulibc -I$(MB)/source/guest/include -I$(MB)/extern/jsmn
GLINCS  := -I$(MB)/source/gl -Iglad/include -Igenerated-gl
VKINCS  := -I$(MB)/source/vk -I$(MB)/extern/vulkan-headers/include -Igenerated-vk

CXXFLAGS := $(WBFLAGS) $(TUFLAGS) -DCHIMERA_GUEST -DCHIMERA_GL_BRIDGE $(MBINCS) $(GLINCS) -I. $(CXXINCS)
CFLAGS   := $(WBFLAGS) -DCHIMERA_GUEST $(GLINCS)

OBJS := $(O)/dolphin-driver.o $(O)/host-stubs.o $(O)/wbx-entry.o $(O)/guest-syscalls.o \
        $(O)/gl-shim.o $(O)/gl-bridge-guest.o $(O)/glad-gl.o $(O)/ram-nand.o $(O)/zip-reader.o \
        $(O)/vk-shim.o $(O)/vk-guest.o $(O)/vk-bridge-guest.o

all: $(OBJS)

$(O)/%.o: %.cpp dolphin-driver.h
	@mkdir -p $(O)
	g++ $(SPECS) $(CXXFLAGS) -c -o $@ $<

$(O)/gl-bridge-guest.o: generated-gl/gl-bridge-guest.cpp
	@mkdir -p $(O)
	g++ $(SPECS) $(CXXFLAGS) -c -o $@ $<

$(O)/glad-gl.o: glad/src/gl.c
	@mkdir -p $(O)
	gcc $(SPECS) $(CFLAGS) -std=gnu11 -c -o $@ $<

# the guest half of miniBox's Vulkan bridge: its own code and the wrappers
# build-core.sh generates from the list the frontend's half is generated from
$(O)/vk-guest.o: $(MB)/source/vk/vk-guest.c
	@mkdir -p $(O)
	gcc $(SPECS) $(CFLAGS) $(VKINCS) -std=gnu11 -c -o $@ $<

$(O)/vk-bridge-guest.o: generated-vk/vk-bridge-guest.c
	@mkdir -p $(O)
	gcc $(SPECS) $(CFLAGS) $(VKINCS) -std=gnu11 -c -o $@ $<

clean:
	rm -rf $(O)

.PHONY: all clean
