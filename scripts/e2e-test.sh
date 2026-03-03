#!/usr/bin/env bash
set -euo pipefail

# E2E test: validates that seaclaw builds, runs unit tests, and the binary
# starts correctly for key subcommands.
# Usage: ./scripts/e2e-test.sh [--help]

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
DIM='\033[2m'
RESET='\033[0m'

die() { printf "${RED}error:${RESET} %s\n" "$1" >&2; exit 1; }
info() { printf "${GREEN}==>${RESET} ${BOLD}%s${RESET}\n" "$1"; }
warn() { printf "${YELLOW}warning:${RESET} %s\n" "$1"; }

case "${1:-}" in
    --help|-h)
        printf "Usage: %s [--help]\n" "$0"
        printf "E2E test: validates that seaclaw builds, runs unit tests, and the binary starts correctly.\n"
        exit 0
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="$ROOT/build"
BIN="$BUILD/seaclaw"
TESTS="$BUILD/seaclaw_tests"

pass=0
fail=0

check() {
    local desc="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        echo -e "  ${GREEN}PASS${RESET}  $desc"
        ((pass++))
    else
        echo -e "  ${RED}FAIL${RESET}  $desc"
        ((fail++))
    fi
}

echo "=== seaclaw E2E test suite ==="
echo ""

# Build
echo -e "${DIM}Building...${RESET}"
mkdir -p "$BUILD"
(cd "$BUILD" && cmake "$ROOT" -DSC_ENABLE_ALL_CHANNELS=ON -DCMAKE_BUILD_TYPE=Debug >/dev/null 2>&1)
(cd "$BUILD" && cmake --build . -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null 2>&1)
echo ""

# Unit tests
echo "--- Unit tests ---"
check "all unit tests pass" "$TESTS"

# Binary basics
echo ""
echo "--- Binary checks ---"
check "binary exists" test -f "$BIN"
check "binary is executable" test -x "$BIN"
check "--version exits 0" "$BIN" --version
check "--help exits 0" "$BIN" --help
check "version output contains seaclaw" bash -c "$BIN --version 2>&1 | grep -qi seaclaw"

# Subcommand smoke tests
echo ""
echo "--- Subcommand smoke tests ---"
check "status exits 0" "$BIN" status
check "doctor exits 0" "$BIN" doctor
check "help exits 0" "$BIN" help
check "version exits 0" "$BIN" version
check "cron list exits 0" "$BIN" cron list

# Release build
echo ""
echo "--- Release build ---"
RELEASE_BUILD="$ROOT/build-e2e-release"
mkdir -p "$RELEASE_BUILD"
(cd "$RELEASE_BUILD" && cmake "$ROOT" -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON -DSC_ENABLE_ALL_CHANNELS=ON >/dev/null 2>&1)
(cd "$RELEASE_BUILD" && cmake --build . -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null 2>&1)

RELEASE_BIN="$RELEASE_BUILD/seaclaw"
RELEASE_SIZE=$(stat -f%z "$RELEASE_BIN" 2>/dev/null || stat -c%s "$RELEASE_BIN" 2>/dev/null)
check "release binary < 500 KB" test "$RELEASE_SIZE" -lt 512000
echo -e "  ${DIM}release binary size: ${RELEASE_SIZE} bytes${RESET}"

# Cleanup release build
rm -rf "$RELEASE_BUILD"

# Summary
echo ""
total=$((pass + fail))
if [ "$fail" -eq 0 ]; then
    echo -e "${GREEN}=== All $total checks passed ===${RESET}"
    exit 0
else
    echo -e "${RED}=== $fail/$total checks failed ===${RESET}"
    exit 1
fi
