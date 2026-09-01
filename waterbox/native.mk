# The native reference driver: compiles the adapter with EXACTLY the flags
# CMake gave the libraries (extract-tu-flags.py mines them), links against
# everything build/native produced. Run build-native.sh first.
#
# Usage: make -f native.mk -j$(nproc)

ROOT := ..
B    := $(ROOT)/build/native
O    := obj-native

TUFLAGS := $(shell python3 extract-tu-flags.py $(B)/compile_commands.json Core/Core.cpp)
MB      ?= $(HOME)/chimera/extern/tools/chimera-common-minibox
GLINCS  := -I$(MB)/source/gl -Iglad/include -Igenerated-gl
CXXFLAGS := -O2 -g1 $(TUFLAGS) -DCHIMERA_GL_BRIDGE $(GLINCS) -I.

LIBS := $(shell find $(B) -name '*.a')

all: $(O)/run-native

generated-gl/gl-bridge-guest.cpp: gl-entry-points.txt $(MB)/source/gl/gl-entry-points.txt
	mkdir -p generated-gl
	python3 $(MB)/source/gl/gen-gl-bridge.py glad/include/glad/gl.h \
		$(MB)/source/gl/gl-entry-points.txt generated-gl --only gl-entry-points.txt

$(O)/%.o: %.cpp dolphin-driver.h
	@mkdir -p $(O)
	g++ $(CXXFLAGS) -c -o $@ $<

$(O)/gl-bridge-guest.o: generated-gl/gl-bridge-guest.cpp
	@mkdir -p $(O)
	g++ $(CXXFLAGS) -c -o $@ $<

$(O)/glad-gl.o: glad/src/gl.c
	@mkdir -p $(O)
	gcc -O2 $(GLINCS) -c -o $@ $<

$(O)/gl-host.o: gl-host.c
	@mkdir -p $(O)
	gcc -O2 -DCHIMERA_GL_BRIDGE $(GLINCS) -c -o $@ $<

$(O)/run-native: $(O)/run-native.o $(O)/dolphin-driver.o $(O)/host-stubs.o $(O)/gl-shim.o $(O)/gl-bridge-guest.o $(O)/glad-gl.o $(O)/gl-host.o $(O)/ram-nand.o
	g++ -o $@ $^ -Wl,--start-group $(LIBS) -Wl,--end-group -lpthread -lm -ldl -lrt -lEGL

clean:
	rm -rf $(O)

.PHONY: all clean
