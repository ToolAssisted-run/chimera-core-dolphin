# The native reference driver: compiles the adapter with EXACTLY the flags
# CMake gave the libraries (extract-tu-flags.py mines them), links against
# everything build/native produced. Run build-native.sh first.
#
# Usage: make -f native.mk -j$(nproc)

ROOT := ..
B    := $(ROOT)/build/native
O    := obj-native

TUFLAGS := $(shell python3 extract-tu-flags.py $(B)/compile_commands.json Core/Core.cpp)
CXXFLAGS := -O2 -g1 $(TUFLAGS) -I.

LIBS := $(shell find $(B) -name '*.a')

all: $(O)/run-native

$(O)/%.o: %.cpp dolphin-driver.h
	@mkdir -p $(O)
	g++ $(CXXFLAGS) -c -o $@ $<

$(O)/run-native: $(O)/run-native.o $(O)/dolphin-driver.o $(O)/host-stubs.o
	g++ -o $@ $^ -Wl,--start-group $(LIBS) -Wl,--end-group -lpthread -lm -ldl -lrt

clean:
	rm -rf $(O)

.PHONY: all clean
