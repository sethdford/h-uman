JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
BUILD ?= build

.PHONY: all configure build test clean release asan check fmt format-check fuzz bench setup install hooks lint tidy coverage validate ci

all: build test

configure:
	@mkdir -p $(BUILD)
	cmake -B $(BUILD) -DCMAKE_BUILD_TYPE=Debug -DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_CURL=ON

build: $(BUILD)/CMakeCache.txt
	cmake --build $(BUILD) -j$(JOBS)

$(BUILD)/CMakeCache.txt:
	@$(MAKE) configure

test: build
	$(BUILD)/human_tests

asan:
	cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
		-DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_CURL=ON
	cmake --build build-asan -j$(JOBS)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 build-asan/human_tests

release:
	cmake -B $(BUILD) -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON \
		-DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_CURL=ON
	cmake --build $(BUILD) -j$(JOBS)
	@SIZE=$$(stat -c%s $(BUILD)/human 2>/dev/null || stat -f%z $(BUILD)/human); \
	echo "Binary: $$((SIZE / 1024)) KB"

check: build
	@echo "Running tests (use 'make asan' for AddressSanitizer)"
	$(BUILD)/human_tests

lint:
	cmake -B build-tidy -DCMAKE_BUILD_TYPE=Debug -DSC_ENABLE_ALL_CHANNELS=ON \
		-DSC_ENABLE_CURL=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	@find src -name '*.c' | head -50 | xargs clang-tidy -p build-tidy 2>&1 | tail -30

tidy: lint

coverage:
	cmake -B build-cov -DCMAKE_BUILD_TYPE=Debug -DSC_ENABLE_ALL_CHANNELS=ON \
		-DSC_ENABLE_CURL=ON -DCMAKE_C_FLAGS="--coverage -fprofile-arcs -ftest-coverage"
	cmake --build build-cov -j$(JOBS)
	build-cov/human_tests
	@echo "Generating coverage report..."
	@lcov --capture --directory build-cov --output-file build-cov/coverage.info --ignore-errors mismatch 2>/dev/null || true
	@lcov --remove build-cov/coverage.info '/usr/*' --output-file build-cov/coverage.info 2>/dev/null || true
	@if command -v genhtml >/dev/null 2>&1; then \
		genhtml build-cov/coverage.info --output-directory build-cov/html; \
		echo "Coverage report: build-cov/html/index.html"; \
	else echo "Install lcov for HTML report"; fi

clean:
	rm -rf $(BUILD) build-asan build-check build-fuzz build-cov build-tidy

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
	@if [ -x scripts/benchmark.sh ]; then scripts/benchmark.sh $(BUILD)/human; \
	else echo "Binary: $$(stat -c%s $(BUILD)/human 2>/dev/null || stat -f%z $(BUILD)/human) bytes"; fi

install: release
	cmake --install $(BUILD) --prefix $(or $(PREFIX),$(HOME)/.local)
	@echo "Installed to $(or $(PREFIX),$(HOME)/.local)/bin/human"

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

validate: format-check build test
	@echo "Validation passed."

ci: format-check build test
	@scripts/check-untested.sh
	@echo "CI checks passed."
