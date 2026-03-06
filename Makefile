JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
BUILD ?= build

.PHONY: all configure build test clean release check fmt hooks

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

release:
	cmake -B $(BUILD) -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON
	cmake --build $(BUILD) -j$(JOBS)
	@SIZE=$$(stat -c%s $(BUILD)/seaclaw 2>/dev/null || stat -f%z $(BUILD)/seaclaw); \
	echo "Binary: $$((SIZE / 1024)) KB"

check: build
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 $(BUILD)/seaclaw_tests

clean:
	rm -rf $(BUILD)

fmt:
	@find src include tests -name '*.c' -o -name '*.h' | xargs clang-format -i

hooks:
	git config core.hooksPath .githooks
	@echo "Git hooks activated (.githooks/)"
