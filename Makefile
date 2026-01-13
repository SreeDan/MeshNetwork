BUILD_DIR := build
BINARY := ./$(BUILD_DIR)/MeshNetworking

# make sure VCPKG_ROOT is set in the PATH
ifndef VCPKG_ROOT
$(error VCPKG_ROOT is not set. Run 'export VCPKG_ROOT=/path/to/vcpkg' or add it to your shell profile)
endif

TOOLCHAIN := $(VCPKG_ROOT)/scripts/buildsystems/vcpkg.cmake

ifeq (run,$(firstword $(MAKECMDGOALS)))
  # Take everything after the first word and make them args
  RUN_ARGS := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))
  $(eval $(RUN_ARGS):;@:)
endif

all: build

# use the toolchain from the environment variable
configure:
	@echo "Using vcpkg at: $(VCPKG_ROOT)"
	cmake -B $(BUILD_DIR) -S . \
		-DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) \
		-DCMAKE_BUILD_TYPE=Debug

build: configure
	cmake --build $(BUILD_DIR) -j$$(nproc)

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "Available commands:"
	@echo "  make        - Build the project (requires VCPKG_ROOT set)"
	@echo "  make clean  - Remove build files"