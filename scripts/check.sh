#!/bin/sh
# SeaClaw local validation script — single source of truth for pre-commit/pre-push style.
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
  4. run seaclaw_tests

Exit 0 if all pass, 1 otherwise.
EOF
}

case "${1:-}" in
    -h|--help) show_help; exit 0 ;;
esac

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || die "cannot cd to $ROOT"

if [ ! -f "CMakeLists.txt" ]; then
    die "not in SeaClaw root (no CMakeLists.txt)"
fi

PASS=0
FAIL=0

# 1. clang-format
info "Step 1/4: clang-format check..."
if find src include \( -name '*.c' -o -name '*.h' \) 2>/dev/null | xargs clang-format --dry-run -Werror 2>/dev/null; then
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
(cd "$BUILD_DIR" && cmake .. -DCMAKE_BUILD_TYPE=Debug -DSC_ENABLE_ALL_CHANNELS=ON) >/dev/null 2>&1
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
info "Step 4/4: seaclaw_tests..."
if [ -x "$BUILD_DIR/seaclaw_tests" ]; then
    if "$BUILD_DIR/seaclaw_tests" >/dev/null 2>&1; then
        info "  seaclaw_tests: pass"
        PASS=$((PASS + 1))
    else
        warn "  seaclaw_tests: fail"
        FAIL=$((FAIL + 1))
    fi
else
    warn "  seaclaw_tests: binary not found"
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
