# Human development task runner
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
    @ls -lh build/human

# Clean build directory
clean:
    rm -rf build

# Reconfigure from scratch and build
rebuild: clean build

# ── Test ─────────────────────────────────────────────────────────────────

# Run all tests
test: build
    ./build/human_tests

# Run tests matching a pattern (e.g. just test-grep imessage)
test-grep pattern: build
    ./build/human_tests 2>&1 | grep -E "PASS|FAIL|Results" | grep -i "{{pattern}}" || true
    ./build/human_tests 2>&1 | tail -1

# ── Benchmark ────────────────────────────────────────────────────────────

# Run full benchmark suite
bench:
    cmake -B build -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON -DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_BENCH=ON
    cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
    scripts/benchmark.sh build/human
    @echo "---"
    ./build/human_bench

# Compare benchmarks against a saved baseline
bench-compare baseline:
    scripts/compare-benchmarks.sh {{baseline}} benchmark-results.json

# ── Fuzz ─────────────────────────────────────────────────────────────────

# Build and run fuzz harnesses (requires clang)
fuzz duration="30":
    #!/usr/bin/env bash
    set -euo pipefail
    CC="${FUZZ_CC:-$(command -v /opt/homebrew/opt/llvm/bin/clang || echo clang)}"
    cmake -B build-fuzz -DSC_ENABLE_FUZZ=ON -DSC_ENABLE_FUZZING=ON -DCMAKE_C_COMPILER="$CC"
    cmake --build build-fuzz -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
    for harness in fuzz_json_parse fuzz_config_load fuzz_tool_params fuzz_base64 fuzz_json fuzz_config fuzz_url_encode fuzz_persona_json fuzz_sse fuzz_http_parse; do
        if [ -f "build-fuzz/$harness" ]; then
            echo "==> Fuzzing $harness for {{duration}}s"
            timeout $(({{duration}} + 5)) ./build-fuzz/$harness -max_total_time={{duration}} || true
        fi
    done

# ── Format / Lint ────────────────────────────────────────────────────────

# Check C formatting
fmt-check:
    @find src include tests -name '*.c' -o -name '*.h' | head -80 | xargs clang-format --dry-run -Werror

# Fix C formatting
fmt:
    @find src include tests -name '*.c' -o -name '*.h' | xargs clang-format -i
    @echo "Formatted all C sources and tests"

# Check for source files without tests
check-untested:
    @scripts/check-untested.sh

# ── Service ──────────────────────────────────────────────────────────────

# Install and start as macOS LaunchAgent
install: release
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p ~/bin
    cp build/human ~/bin/human
    xattr -d com.apple.provenance ~/bin/human 2>/dev/null || true
    xattr -d com.apple.quarantine ~/bin/human 2>/dev/null || true
    # Code-sign so macOS preserves Full Disk Access across rebuilds
    if security find-identity -v -p codesigning 2>/dev/null | grep -q "Human Local Dev"; then
        codesign -f -s "Human Local Dev" ~/bin/human
        echo "Signed with Human Local Dev identity"
    else
        codesign -f -s - ~/bin/human
        echo "Ad-hoc signed (FDA will be revoked on each rebuild. Run: just setup-codesign)"
    fi
    # Restart LaunchAgent if loaded
    if launchctl list 2>/dev/null | grep -q com.human.agent; then
        launchctl bootout gui/$(id -u)/com.human.agent 2>/dev/null || true
        sleep 2
    fi
    launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.human.agent.plist 2>/dev/null || \
        launchctl load -w ~/Library/LaunchAgents/com.human.agent.plist 2>/dev/null || true
    echo "Installed and service restarted"

# One-time setup: create a local code-signing certificate so FDA persists across rebuilds
setup-codesign:
    #!/usr/bin/env bash
    set -euo pipefail
    if security find-identity -v -p codesigning 2>/dev/null | grep -q "Human Local Dev"; then
        echo "Human Local Dev certificate already exists"
        exit 0
    fi
    echo "Creating self-signed code-signing certificate..."
    openssl req -x509 -newkey rsa:2048 \
        -keyout /tmp/sc-key.pem -out /tmp/sc-cert.pem \
        -days 3650 -nodes \
        -subj "/CN=Human Local Dev" \
        -addext "keyUsage=digitalSignature" \
        -addext "extendedKeyUsage=codeSigning" 2>/dev/null
    openssl pkcs12 -export -out /tmp/sc-sign.p12 \
        -inkey /tmp/sc-key.pem -in /tmp/sc-cert.pem \
        -passout pass:sc -certpbe PBE-SHA1-3DES -keypbe PBE-SHA1-3DES -macalg SHA1 2>/dev/null
    security import /tmp/sc-sign.p12 -k ~/Library/Keychains/login.keychain-db \
        -T /usr/bin/codesign -P sc
    security find-certificate -c "Human Local Dev" -p ~/Library/Keychains/login.keychain-db > /tmp/sc-cert-export.pem
    security add-trusted-cert -d -r trustRoot -p codeSign \
        -k ~/Library/Keychains/login.keychain-db /tmp/sc-cert-export.pem
    rm -f /tmp/sc-key.pem /tmp/sc-cert.pem /tmp/sc-sign.p12 /tmp/sc-cert-export.pem
    echo "Done! Certificate valid for 10 years."
    security find-identity -v -p codesigning

# Uninstall LaunchAgent
uninstall:
    launchctl bootout gui/$(id -u)/com.human.agent 2>/dev/null || true
    echo "Service stopped"

# Show service status
status:
    @launchctl list 2>/dev/null | grep human || echo "Not in launchctl"
    @ps aux | grep "[s]eaclaw service" | grep -v zsh || echo "Not running"

# View service logs
logs:
    tail -f ~/.human/human.log

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
    ./build/human persona list

# Show persona by name
persona-show name: build
    ./build/human persona show {{name}}

# Validate persona by name
persona-validate name: build
    ./build/human persona validate {{name}}

# Run persona-related tests
persona-test: build
    cd build && ./human_tests 2>&1 | grep -E "persona|Results"

# Create a test persona and validate it (for local smoke test)
persona-create-test: build
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p "$HOME/.human/personas"
    echo '{"version":1,"name":"just_test","core":{"identity":"Justfile test persona","traits":["direct"]}}' > "$HOME/.human/personas/just_test.json"
    ./build/human persona validate just_test
    ./build/human persona show just_test
    echo "Persona smoke test passed"

# ── Stats ────────────────────────────────────────────────────────────────

# Update metrics in AGENTS.md, README.md, PROJECT_STATUS.md
update-stats: build
    @scripts/update-stats.sh --apply

# Dry-run stats (show what would change)
stats: build
    @scripts/update-stats.sh

# ── Info ─────────────────────────────────────────────────────────────────

# Show binary size and test count
info: build
    @stat -f '%z' build/human 2>/dev/null || stat -c '%s' build/human
    @echo " bytes (dev build)"
    @./build/human_tests 2>&1 | tail -1

# Full local health check
doctor: build
    #!/usr/bin/env bash
    set -euo pipefail
    echo "==> Running tests..."
    RESULT=$(./build/human_tests 2>&1 | tail -1)
    echo "    $RESULT"
    if ! echo "$RESULT" | grep -q "passed"; then
        echo "FAIL: tests did not pass"
        exit 1
    fi
    echo "==> Binary size..."
    SIZE=$(stat -f '%z' build/human 2>/dev/null || stat -c '%s' build/human)
    echo "    $SIZE bytes (dev)"
    echo "==> Config check..."
    if [ -f ~/.human/config.json ]; then
        echo "    config found"
        ./build/human doctor 2>&1 | sed 's/^/    /' || true
    else
        echo "    no config (~/.human/config.json)"
    fi
    echo "==> Service status..."
    ps aux 2>/dev/null | grep "[s]eaclaw service" | head -1 || echo "    not running"
    echo "==> Done. All checks passed."
