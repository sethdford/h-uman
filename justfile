# SeaClaw development task runner
# Install: brew install just  (or cargo install just)
# Usage:  just <recipe>

default:
    @just --list

# ── Build ────────────────────────────────────────────────────────────────

# Dev build (ASan enabled)
build:
    cmake -B build -DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_SQLITE=ON
    cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)

# Release build (MinSizeRel + LTO)
release:
    cmake -B build -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON -DSC_ENABLE_ALL_CHANNELS=ON
    cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
    @ls -lh build/seaclaw

# Clean build directory
clean:
    rm -rf build

# Reconfigure from scratch and build
rebuild: clean build

# ── Test ─────────────────────────────────────────────────────────────────

# Run all tests
test: build
    ./build/seaclaw_tests

# Run tests matching a pattern (e.g. just test-grep imessage)
test-grep pattern: build
    ./build/seaclaw_tests 2>&1 | grep -E "PASS|FAIL|Results" | grep -i "{{pattern}}" || true
    ./build/seaclaw_tests 2>&1 | tail -1

# ── Benchmark ────────────────────────────────────────────────────────────

# Run full benchmark suite
bench:
    cmake -B build -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON -DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_BENCH=ON
    cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
    scripts/benchmark.sh build/seaclaw
    @echo "---"
    ./build/seaclaw_bench

# Compare benchmarks against a saved baseline
bench-compare baseline:
    scripts/compare-benchmarks.sh {{baseline}} benchmark-results.json

# ── Fuzz ─────────────────────────────────────────────────────────────────

# Build and run fuzz harnesses (requires clang)
fuzz duration="30":
    #!/usr/bin/env bash
    set -euo pipefail
    CC="${FUZZ_CC:-$(command -v /opt/homebrew/opt/llvm/bin/clang || echo clang)}"
    cmake -B build-fuzz -DSC_ENABLE_FUZZ=ON -DCMAKE_C_COMPILER="$CC"
    cmake --build build-fuzz -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
    mkdir -p corpus/json corpus/config corpus/tool_params
    for harness in fuzz_json_parse fuzz_config_load fuzz_tool_params; do
        if [ -f "build-fuzz/$harness" ]; then
            echo "==> Fuzzing $harness for {{duration}}s"
            ./build-fuzz/$harness -max_total_time={{duration}} corpus/${harness#fuzz_}/ || true
        fi
    done

# ── Format / Lint ────────────────────────────────────────────────────────

# Check C formatting
fmt-check:
    @find src include -name '*.c' -o -name '*.h' | head -50 | xargs clang-format --dry-run -Werror

# Fix C formatting
fmt:
    @find src include -name '*.c' -o -name '*.h' | xargs clang-format -i
    @echo "Formatted all C sources"

# ── Service ──────────────────────────────────────────────────────────────

# Install and start as macOS LaunchAgent
install: release
    cp build/seaclaw ~/.local/bin/seaclaw
    ~/.local/bin/seaclaw service install

# Uninstall LaunchAgent
uninstall:
    ~/.local/bin/seaclaw service uninstall

# Show service status
status:
    @~/.local/bin/seaclaw service status 2>&1 || true
    @ps aux | grep "[s]eaclaw service" | grep -v zsh || echo "Not running"

# View service logs
logs:
    tail -f ~/.seaclaw/seaclaw.log

# ── Release ──────────────────────────────────────────────────────────────

# Full release workflow: test, benchmark, tag
tag version: test
    @echo "Tagging {{version}}"
    git tag -a "{{version}}" -m "Release {{version}}"
    git push origin "{{version}}"

# Run the full pre-push check
check:
    scripts/check.sh

# ── Persona ───────────────────────────────────────────────────────────────

# List personas
persona-list: build
    ./build/seaclaw persona list

# Show persona by name
persona-show name: build
    ./build/seaclaw persona show {{name}}

# Validate persona by name
persona-validate name: build
    ./build/seaclaw persona validate {{name}}

# Run persona-related tests
persona-test: build
    cd build && ./seaclaw_tests 2>&1 | grep -E "persona|Results"

# Create a test persona and validate it (for local smoke test)
persona-create-test: build
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p "$HOME/.seaclaw/personas"
    echo '{"version":1,"name":"just_test","core":{"identity":"Justfile test persona","traits":["direct"]}}' > "$HOME/.seaclaw/personas/just_test.json"
    ./build/seaclaw persona validate just_test
    ./build/seaclaw persona show just_test
    echo "Persona smoke test passed"

# ── Info ─────────────────────────────────────────────────────────────────

# Show binary size and test count
info: build
    @stat -f '%z' build/seaclaw 2>/dev/null || stat -c '%s' build/seaclaw
    @echo " bytes (dev build)"
    @./build/seaclaw_tests 2>&1 | tail -1

# Full local health check
doctor: build
    #!/usr/bin/env bash
    set -euo pipefail
    echo "==> Running tests..."
    RESULT=$(./build/seaclaw_tests 2>&1 | tail -1)
    echo "    $RESULT"
    if ! echo "$RESULT" | grep -q "passed"; then
        echo "FAIL: tests did not pass"
        exit 1
    fi
    echo "==> Binary size..."
    SIZE=$(stat -f '%z' build/seaclaw 2>/dev/null || stat -c '%s' build/seaclaw)
    echo "    $SIZE bytes (dev)"
    echo "==> Config check..."
    if [ -f ~/.seaclaw/config.json ]; then
        echo "    config found"
        ./build/seaclaw doctor 2>&1 | sed 's/^/    /' || true
    else
        echo "    no config (~/.seaclaw/config.json)"
    fi
    echo "==> Service status..."
    ps aux 2>/dev/null | grep "[s]eaclaw service" | head -1 || echo "    not running"
    echo "==> Done. All checks passed."
