BUILD_TYPE := Release
BUILD_SUBDIR := release
SANITIZER_FLAGS :=
LINKER_FLAGS :=

ifneq ($(filter debug,$(MAKECMDGOALS)),)
    BUILD_TYPE := Debug
    BUILD_SUBDIR := debug
    SANITIZER_FLAGS :=
    LINKER_FLAGS :=
endif

BUILD_DIR := build/$(BUILD_SUBDIR)
BINARY := ./$(BUILD_DIR)/MeshNetworking

# make sure VCPKG_ROOT is set in the PATH
ifndef VCPKG_ROOT
$(error VCPKG_ROOT is not set. Run 'export VCPKG_ROOT=/path/to/vcpkg' or add it to your shell profile)
endif

TOOLCHAIN := $(VCPKG_ROOT)/scripts/buildsystems/vcpkg.cmake

all: build

TRIPLET_NAME := x64-linux-cpp23
TRIPLET_OVERLAY := $(CURDIR)/triplets

configure:
	@echo "Using vcpkg at: $(VCPKG_ROOT)"
	cmake -B $(BUILD_DIR) -S . \
       -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) \
       -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
       -DVCPKG_TARGET_TRIPLET=$(TRIPLET_NAME) \
       -DVCPKG_OVERLAY_TRIPLETS=$(TRIPLET_OVERLAY) \
       -DCMAKE_CXX_FLAGS="$(SANITIZER_FLAGS)" \
       -DCMAKE_EXE_LINKER_FLAGS="$(LINKER_FLAGS)"

build: configure
	cmake --build $(BUILD_DIR) -j$$(nproc)

run: build
	@echo "Running $(BINARY)..."
	$(BINARY) $(RUN_ARGS)

debug: build
	@echo "Starting GDB on $(BINARY)..."
	gdb --args $(BINARY) $(RUN_ARGS)

ifeq (run,$(firstword $(MAKECMDGOALS)))
  RUN_ARGS := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))
  $(eval $(RUN_ARGS):;@:)
endif

clean:
	rm -rf build

help:
	@echo "Targets:"
	@echo "  make        - Build optimized Release version (in build/release)"
	@echo "  make debug  - Build Debug version with ASan (in build/debug) and run GDB"
	@echo "  make clean  - Remove all build folders"