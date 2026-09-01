# Guest driver objects: the same adapter sources as native.mk, compiled with
# the miniBox musl toolchain and the flags CMake gave the guest libraries.
# build-guest.sh must have run first (build/guest holds the archives and
# compile_commands.json); build-core.sh links core.wbx from all of it.
#
# Usage: make -f guest.mk -j$(nproc)

ROOT   := ..
B      := $(ROOT)/build/guest
O      := obj-guest
MB     ?= $(HOME)/chimera/extern/tools/chimera-common-minibox
MBUILD := $(MB)/build/meson-cpp
SR     := $(MBUILD)/guest-sysroot
GCCVER := $(shell gcc -dumpfullversion)

TUFLAGS := $(shell python3 extract-tu-flags.py $(B)/compile_commands.json Core/Core.cpp)

WBFLAGS := -fvisibility=hidden -mcmodel=large -mstack-protector-guard=global -fno-stack-protector \
        -fno-pic -fno-pie -fcf-protection=none -O2
SPECS   := -specs $(SR)/lib/musl-gcc.specs
CXXINCS := -nostdinc++ -I$(SR)/include/c++/$(GCCVER) -I$(SR)/include/c++/$(GCCVER)/x86_64-linux-musl
MBINCS  := -I$(MB)/extern/emulibc -I$(MB)/source/guest/include -I$(MB)/extern/jsmn

CXXFLAGS := $(WBFLAGS) $(TUFLAGS) -DCHIMERA_GUEST $(MBINCS) -I. $(CXXINCS)

OBJS := $(O)/dolphin-driver.o $(O)/host-stubs.o $(O)/wbx-entry.o $(O)/guest-syscalls.o

all: $(OBJS)

$(O)/%.o: %.cpp dolphin-driver.h
	@mkdir -p $(O)
	g++ $(SPECS) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf $(O)

.PHONY: all clean
