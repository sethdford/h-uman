#!/bin/sh
# Human local validation script — single source of truth for pre-commit/pre-push style.
# Usage: ./scripts/check.sh [--help]

set -eu

die() { echo "Error: $1" >&2; exit 1; }
info() { echo "$1"; }
warn() { echo "Warning: $1" >&2; }

show_help() {
    cat << EOF
usage: $(basename "$0") [--help]

Runs local validation steps:
  1. clang-format --dry-run -Werror on all .c/.h in src/ and include/
  2. cmake configure (Debug, all channels) in build-check/
  3. cmake build
  4. run human_tests

Exit 0 if all pass, 1 otherwise.
EOF
}

case "${1:-}" in
    -h|--help) show_help; exit 0 ;;
esac

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || die "cannot cd to $ROOT"

if [ ! -f "CMakeLists.txt" ]; then
    die "not in Human root (no CMakeLists.txt)"
fi

CLANG_FMT="clang-format"
if ! command -v "$CLANG_FMT" >/dev/null 2>&1; then
    for p in /opt/homebrew/opt/llvm/bin/clang-format /usr/local/opt/llvm/bin/clang-format; do
        if [ -x "$p" ]; then CLANG_FMT="$p"; break; fi
    done
fi

PASS=0
FAIL=0

# 1. clang-format (skippable via HU_SKIP_FORMAT=1 for cross-version tolerance)
info "Step 1/4: clang-format check..."
if [ "${HU_SKIP_FORMAT:-0}" = "1" ]; then
    info "  clang-format: skipped (HU_SKIP_FORMAT=1)"
    PASS=$((PASS + 1))
elif find src include \( -name '*.c' -o -name '*.h' \) 2>/dev/null | xargs "$CLANG_FMT" --dry-run -Werror 2>/dev/null; then
    info "  clang-format: pass"
    PASS=$((PASS + 1))
else
    warn "  clang-format: fail"
    FAIL=$((FAIL + 1))
fi

# 2 & 3. cmake configure and build
info "Step 2/4: cmake configure..."
BUILD_DIR="build-check"
mkdir -p "$BUILD_DIR"
CURL_FLAG=""
case "$(uname -s)" in
  Linux) CURL_FLAG="-DSC_ENABLE_CURL=ON" ;;
esac
(cd "$BUILD_DIR" && cmake .. -DCMAKE_BUILD_TYPE=Debug -DSC_ENABLE_ALL_CHANNELS=ON $CURL_FLAG) >/dev/null 2>&1
if [ $? -eq 0 ]; then
    info "  cmake configure: pass"
    PASS=$((PASS + 1))
else
    warn "  cmake configure: fail"
    FAIL=$((FAIL + 1))
fi

info "Step 3/4: cmake build..."
JOBS=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
(cd "$BUILD_DIR" && cmake --build . -j"$JOBS") >/dev/null 2>&1
if [ $? -eq 0 ]; then
    info "  cmake build: pass"
    PASS=$((PASS + 1))
else
    warn "  cmake build: fail"
    FAIL=$((FAIL + 1))
fi

# 4. run tests
info "Step 4/4: human_tests..."
if [ -x "$BUILD_DIR/human_tests" ]; then
    if "$BUILD_DIR/human_tests" >/dev/null 2>&1; then
        info "  human_tests: pass"
        PASS=$((PASS + 1))
    else
        warn "  human_tests: fail"
        FAIL=$((FAIL + 1))
    fi
else
    warn "  human_tests: binary not found"
    FAIL=$((FAIL + 1))
fi

info ""
if [ "$FAIL" -eq 0 ]; then
    info "Summary: all $PASS checks passed"
    exit 0
else
    info "Summary: $FAIL failed, $PASS passed"
    exit 1
fi
