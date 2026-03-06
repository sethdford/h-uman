JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
BUILD ?= build

.PHONY: all configure build test clean release asan check fmt format-check fuzz bench setup hooks

all: build test

configure:
	@mkdir -p $(BUILD)
	cmake -B $(BUILD) -DCMAKE_BUILD_TYPE=Debug -DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_CURL=ON

build: $(BUILD)/CMakeCache.txt
	cmake --build $(BUILD) -j$(JOBS)

$(BUILD)/CMakeCache.txt:
	@$(MAKE) configure

test: build
	$(BUILD)/seaclaw_tests

asan:
	cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
		-DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_CURL=ON
	cmake --build build-asan -j$(JOBS)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 build-asan/seaclaw_tests

release:
	cmake -B $(BUILD) -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON \
		-DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_CURL=ON
	cmake --build $(BUILD) -j$(JOBS)
	@SIZE=$$(stat -c%s $(BUILD)/seaclaw 2>/dev/null || stat -f%z $(BUILD)/seaclaw); \
	echo "Binary: $$((SIZE / 1024)) KB"

check: build
	@echo "Running tests (use 'make asan' for AddressSanitizer)"
	$(BUILD)/seaclaw_tests

clean:
	rm -rf $(BUILD) build-asan build-check build-fuzz

fmt:
	@find src include tests -name '*.c' -o -name '*.h' | xargs clang-format -i

format-check:
	@find src include tests -name '*.c' -o -name '*.h' | xargs clang-format --dry-run -Werror

fuzz:
	cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DSC_ENABLE_FUZZ=ON \
		-DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_CURL=ON
	cmake --build build-fuzz -j$(JOBS)
	@echo "Fuzz targets built. Run: ./build-fuzz/fuzz_<name> -max_total_time=30"

bench: release
	@if [ -x scripts/benchmark.sh ]; then scripts/benchmark.sh $(BUILD)/seaclaw; \
	else echo "Binary: $$(stat -c%s $(BUILD)/seaclaw 2>/dev/null || stat -f%z $(BUILD)/seaclaw) bytes"; fi

setup:
	@echo "==> Installing dependencies"
	@if [ "$$(uname)" = "Darwin" ]; then echo "  brew install cmake sqlite curl"; \
	else echo "  sudo apt install build-essential cmake libsqlite3-dev libcurl4-openssl-dev"; fi
	@echo ""
	@$(MAKE) configure
	@$(MAKE) hooks
	@echo "==> Ready. Run: make test"

hooks:
	git config core.hooksPath .githooks
	@echo "Git hooks activated (.githooks/)"
