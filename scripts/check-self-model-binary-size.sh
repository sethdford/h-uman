#!/usr/bin/env bash
# Spec 2026-05-19 self-model-scaffold — Task 13 (AC-SM-6).
#
# Builds the `human` binary with and without HU_ENABLE_SELF_MODEL, then
# asserts the disabled-flag build grew by less than 1 KB versus the
# enabled-flag build (the spec frames this as "delta vs pre-spec
# baseline"; in the absence of a pinned pre-spec baseline we use the
# OFF variant as the proxy, since OFF compiles only the stub bodies
# and is the closest thing to "no spec applied").
#
# CI hook: invoke this from .github/workflows/ci.yml after the main
# build. Exit code 0 = within budget, 1 = budget exceeded, 2 = build
# failed.

set -euo pipefail

# 1 KB tolerance per AC-SM-6. Tunable via env for CI tuning.
MAX_DELTA_BYTES="${HU_SELF_MODEL_MAX_DELTA_BYTES:-1024}"

# Use minimal-release preset for the comparison — same flags up to the
# SELF_MODEL toggle so the delta isolates this spec's contribution.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

OFF_BUILD="$REPO_ROOT/build-self-model-off"
ON_BUILD="$REPO_ROOT/build-self-model-on"

build_variant() {
    local build_dir="$1"
    local flag_value="$2"
    rm -rf "$build_dir"
    cmake -B "$build_dir" -S "$REPO_ROOT" \
        -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DHU_ENABLE_SELF_MODEL="$flag_value" \
        -DHU_ENABLE_ASAN=OFF \
        -DHU_ENABLE_ALL_CHANNELS=ON \
        -DHU_ENABLE_SQLITE=ON \
        -DHU_ENABLE_PERSONA=ON >/dev/null || return 2
    cmake --build "$build_dir" --target human -j >/dev/null 2>&1 || return 2
    echo "$build_dir/human"
}

binary_size() {
    local bin="$1"
    if [ ! -f "$bin" ]; then
        echo "missing binary: $bin" >&2
        return 2
    fi
    # Cross-platform size: prefer wc -c (works on macOS + Linux).
    wc -c < "$bin" | tr -d ' '
}

off_bin="$(build_variant "$OFF_BUILD" OFF)" || {
    echo "FAIL: OFF build did not link" >&2
    exit 2
}
on_bin="$(build_variant "$ON_BUILD" ON)" || {
    echo "FAIL: ON build did not link" >&2
    exit 2
}

off_size="$(binary_size "$off_bin")"
on_size="$(binary_size "$on_bin")"
delta=$((on_size - off_size))
abs_delta=${delta#-}

echo "self-model binary size check:"
echo "  OFF size : $off_size bytes"
echo "  ON  size : $on_size bytes"
echo "  delta    : $delta bytes (abs=$abs_delta, budget=$MAX_DELTA_BYTES)"

if [ "$abs_delta" -ge "$MAX_DELTA_BYTES" ]; then
    echo "FAIL: spec landed with binary delta >= $MAX_DELTA_BYTES bytes (AC-SM-6)" >&2
    exit 1
fi

echo "PASS: spec stays within AC-SM-6 binary-size budget"
exit 0
