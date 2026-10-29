# NXStation — devkitPro Docker build
# Usage (PowerShell):
#   function dmake { docker run --rm -v ${PWD}:/src --workdir /src devkitpro/devkita64 make $args }
#   dmake
#   dmake clean
#   dmake -j8

.PHONY: all configure clean NXStation.nro

BUILD_DIR  ?= build_switch
CMAKE_FLAGS ?= -DPLATFORM_SWITCH=ON -DSF_ENABLE_FFMPEG=ON -DCMAKE_BUILD_TYPE=Release
JOBS       ?= $(shell nproc 2>/dev/null || echo 4)
SHELL := /bin/bash
SWITCH_ENV = source /opt/devkitpro/switchvars.sh

# libromfs-generator must run on the build host. When cross-compiling for Switch
# inside Docker the toolchain-built generator is a Switch ELF that crashes under
# emulation, so build the host-native one here and point CMake at it.
GEN_DIR   ?= build_libromfs_generator
GEN_BIN   ?= $(GEN_DIR)/libromfs-generator

all: NXStation.nro

generator:
	@mkdir -p $(GEN_DIR)
	@g++ -std=c++20 -O2 -o $(GEN_BIN) library/borealis/library/lib/extern/libromfs/generator/source/main.cpp

configure: generator
	@$(SWITCH_ENV) && cmake -B $(BUILD_DIR) $(CMAKE_FLAGS) -DLIBROMFS_PREBUILT_GENERATOR=$(CURDIR)/$(GEN_BIN)

$(BUILD_DIR)/Makefile:
	@$(MAKE) configure

NXStation.nro: $(BUILD_DIR)/Makefile
	@$(SWITCH_ENV) && cmake --build $(BUILD_DIR) --target NXStation.nro -j$(JOBS)

clean:
	rm -rf $(BUILD_DIR)
