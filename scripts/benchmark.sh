#!/bin/sh
# SeaClaw local benchmarking harness
# Usage: ./scripts/benchmark.sh [path/to/seaclaw] [--compare FILE]
# Measures: binary size, symbols, sections, startup time, memory, test metrics.

set -eu

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m'

die() { printf "${RED}error:${NC} %s\n" "$1" >&2; exit 1; }
info() { printf "${GREEN}==>${NC} ${BOLD}%s${NC}\n" "$1"; }
warn() { printf "${YELLOW}warning:${NC} %s\n" "$1"; }

case "${1:-}" in
    --help|-h)
        printf "Usage: %s [path/to/seaclaw] [--compare FILE]\n" "$0"
        printf "Measures: binary size, symbols, sections, startup time, memory, test metrics.\n"
        exit 0
        ;;
esac

# Default paths (release seaclaw, tests often in build-check with ASan)
SCRIPTS_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$SCRIPTS_DIR/.." && pwd)
DEFAULT_SEACLAW="${ROOT_DIR}/build/seaclaw"
DEFAULT_TESTS="${ROOT_DIR}/build-check/seaclaw_tests"

# If build-check doesn't exist, try build/ for tests (common single-dir setup)
if [ ! -x "$DEFAULT_TESTS" ] && [ -x "${ROOT_DIR}/build/seaclaw_tests" ]; then
    DEFAULT_TESTS="${ROOT_DIR}/build/seaclaw_tests"
fi

COMPARE_FILE=""
SEACLAW_BIN=""
TESTS_BIN=""

# Parse args
while [ $# -gt 0 ]; do
    case "$1" in
        --compare)
            COMPARE_FILE="${2:-}"
            shift 2
            ;;
        --compare=*)
            COMPARE_FILE="${1#--compare=}"
            shift
            ;;
        *)
            if [ -z "$SEACLAW_BIN" ]; then
                SEACLAW_BIN="$1"
            fi
            shift
            ;;
    esac
done

# Resolve paths
if [ -z "$SEACLAW_BIN" ]; then
    SEACLAW_BIN="$DEFAULT_SEACLAW"
fi

# Resolve relative paths to absolute
case "$SEACLAW_BIN" in
    /*) ;;
    *) SEACLAW_BIN="${ROOT_DIR}/${SEACLAW_BIN}"
esac

# Tests: same dir as seaclaw first, else default
BIN_DIR=$(dirname "$SEACLAW_BIN")
if [ -x "${BIN_DIR}/seaclaw_tests" ]; then
    TESTS_BIN="${BIN_DIR}/seaclaw_tests"
else
    TESTS_BIN="$DEFAULT_TESTS"
fi

if [ ! -f "$SEACLAW_BIN" ]; then
    die "seaclaw binary not found at $SEACLAW_BIN. Build with: mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel && cmake --build ."
fi

# Detect OS for time/memory
case "$(uname -s)" in
    Darwin)
        TIME_CMD="/usr/bin/time"
        TIME_ARGS="-l"
        TIME_RSS_UNIT="bytes"
        ;;
    Linux|*)
        TIME_CMD="/usr/bin/time"
        TIME_ARGS="-v"
        TIME_RSS_UNIT="kbytes"
        ;;
esac

# Nanosecond timestamp: date (Linux), gdate (macOS), perl fallback
get_nanos() {
    if command -v gdate >/dev/null 2>&1; then
        gdate +%s%N 2>/dev/null || true
    elif date +%s%N 2>/dev/null | grep -q '^[0-9][0-9]*$'; then
        date +%s%N
    elif command -v perl >/dev/null 2>&1; then
        perl -MTime::HiRes -e 'print int(Time::HiRes::time() * 1e9)' 2>/dev/null
    else
        date +%s
    fi
}

# Parse RSS from time output
# macOS: "  1234567  maximum resident set size" (bytes)
# Linux: "Maximum resident set size (kbytes): 1234"
parse_rss() {
    out="$1"
    rss=0
    if [ "$TIME_RSS_UNIT" = "bytes" ]; then
        rss=$(echo "$out" | awk '/maximum resident set size/ {gsub(/,/,""); print $1+0; exit}')
    else
        rss=$(echo "$out" | awk '/Maximum resident set size.*kbytes/ {for(i=1;i<=NF;i++) if($i ~ /^[0-9]+$/) {print $i*1024; exit}}')
    fi
    echo "${rss:-0}"
}

# Human-readable bytes
fmt_bytes() {
    local n="$1"
    if [ "$n" -ge 1048576 ]; then
        echo "${n} bytes ($((n / 1048576)) MB)"
    elif [ "$n" -ge 1024 ]; then
        echo "${n} bytes ($((n / 1024)) KB)"
    else
        echo "${n} bytes"
    fi
}

# Colored output (optional)
red=''
green=''
reset=''
if [ -t 1 ]; then
    red='\033[0;31m'
    green='\033[0;32m'
    reset='\033[0m'
fi

# --- Collect metrics ---

# Binary size
BINARY_SIZE=$(wc -c < "$SEACLAW_BIN" | tr -d ' ')
BINARY_SIZE_HR=$(printf "%'d" "$BINARY_SIZE")
if [ "$BINARY_SIZE" -ge 1024 ]; then
    BINARY_SIZE_KB=$((BINARY_SIZE / 1024))
    BINARY_HR="${BINARY_SIZE_KB} KB (${BINARY_SIZE_HR} bytes)"
else
    BINARY_HR="${BINARY_SIZE_HR} bytes"
fi

# Exported symbols
SYMBOL_COUNT=0
if command -v nm >/dev/null 2>&1; then
    SYMBOL_COUNT=$(nm -g "$SEACLAW_BIN" 2>/dev/null | wc -l | tr -d ' ')
fi
SYMBOL_HR=$(printf "%'d" "$SYMBOL_COUNT")

# Section sizes (text, data, bss)
TEXT_SIZE=0
DATA_SIZE=0
BSS_SIZE=0
if command -v size >/dev/null 2>&1; then
    size_out=$(size "$SEACLAW_BIN" 2>/dev/null)
    TEXT_SIZE=$(echo "$size_out" | tail -1 | awk '{print $1}')
    DATA_SIZE=$(echo "$size_out" | tail -1 | awk '{print $2}')
    BSS_SIZE=$(echo "$size_out" | tail -1 | awk '{print $3}')
fi
TEXT_HR=""
[ "$TEXT_SIZE" -gt 0 ] && TEXT_HR="$((TEXT_SIZE / 1024)) KB"

# Startup time (10 runs)
STARTUP_MIN=999999
STARTUP_MAX=0
STARTUP_SUM=0
STARTUP_RUNS=10
i=0
while [ "$i" -lt "$STARTUP_RUNS" ]; do
    t0=$(get_nanos)
    "$SEACLAW_BIN" --version >/dev/null 2>&1
    t1=$(get_nanos)
    # Handle both nanoseconds and seconds (perl fallback)
    if [ "${#t0}" -ge 10 ]; then
        elaps=$((t1 - t0))
    else
        elaps=$(((t1 - t0) * 1000000000))
    fi
    elaps_ms=$((elaps / 1000000))
    [ "$elaps_ms" -lt "$STARTUP_MIN" ] && STARTUP_MIN=$elaps_ms
    [ "$elaps_ms" -gt "$STARTUP_MAX" ] && STARTUP_MAX=$elaps_ms
    STARTUP_SUM=$((STARTUP_SUM + elaps_ms))
    i=$((i + 1))
done
STARTUP_AVG=$((STARTUP_SUM / STARTUP_RUNS))

# Memory: peak RSS during tests
TESTS_RSS=0
TESTS_DURATION=0
if [ -x "$TESTS_BIN" ]; then
    tests_dir=$(dirname "$TESTS_BIN")
    time_out=$("$TIME_CMD" $TIME_ARGS "$TESTS_BIN" 2>&1 || true)
    TESTS_RSS=$(parse_rss "$time_out")
    # Duration from time "real" line (last occurrence, time appends at end)
    real_line=$(echo "$time_out" | grep ' real ' | tail -1)
    if [ -n "$real_line" ]; then
        real_sec=$(echo "$real_line" | awk '{for(i=1;i<=NF;i++) if($i~/^[0-9]+\.?[0-9]*$/) {print $i; exit}}')
        if [ -n "$real_sec" ]; then
            if command -v bc >/dev/null 2>&1; then
                TESTS_DURATION=$(echo "scale=2; $real_sec" | bc)
            else
                TESTS_DURATION="$real_sec"
            fi
        fi
    fi
else
    TESTS_RSS=0
fi

# Memory: peak RSS during --version
VERSION_RSS=0
time_out=$("$TIME_CMD" $TIME_ARGS "$SEACLAW_BIN" --version 2>&1 || true)
VERSION_RSS=$(parse_rss "$time_out")

# Test count and duration (if we ran tests and got output)
TEST_COUNT=0
if [ -x "$TESTS_BIN" ]; then
    test_out=$("$TESTS_BIN" 2>&1 || true)
    # Parse "Results: 2002/2002 passed" or "Results: 1998/2002 passed, 4 FAILED"
    match=$(echo "$test_out" | grep -o 'Results: [0-9]*/[0-9]* passed' | tail -1)
    if [ -n "$match" ]; then
        TEST_COUNT=$(echo "$match" | sed 's/.*\/\([0-9]*\) passed/\1/')
    fi
fi

# Re-run tests for duration if we need it (we already have it from time)
if [ -x "$TESTS_BIN" ] && [ "$TESTS_DURATION" = "0" ]; then
    t0=$(get_nanos)
    "$TESTS_BIN" >/dev/null 2>&1 || true
    t1=$(get_nanos)
    if [ "${#t0}" -ge 10 ]; then
        elaps=$((t1 - t0))
    else
        elaps=$(((t1 - t0) * 1000000000))
    fi
    if command -v bc >/dev/null 2>&1; then
        TESTS_DURATION=$(echo "scale=2; $elaps / 1000000000" | bc)
    else
        TESTS_DURATION=$((elaps / 1000000000))
    fi
fi

# Tests rate
RATE=0
if [ "$TEST_COUNT" -gt 0 ] && [ "$TESTS_DURATION" != "0" ] && [ "${TESTS_DURATION}" != "0.00" ]; then
    if command -v bc >/dev/null 2>&1; then
        RATE=$(echo "scale=0; $TEST_COUNT / $TESTS_DURATION" | bc)
    else
        denom="${TESTS_DURATION%.*}"; [ -z "$denom" ] && denom=1
        RATE=$((TEST_COUNT / denom))
    fi
fi

TEST_COUNT_HR=$(printf "%'d" "$TEST_COUNT")
TESTS_RSS_HR=$(fmt_bytes "$TESTS_RSS")
VERSION_RSS_HR=$(fmt_bytes "$VERSION_RSS")

# --- Output JSON ---
JSON_FILE="${ROOT_DIR}/benchmark-results.json"
cat > "$JSON_FILE" << EOF
{
  "binary": {
    "path": "$SEACLAW_BIN",
    "size_bytes": $BINARY_SIZE,
    "size_human": "$BINARY_HR",
    "symbols_exported": $SYMBOL_COUNT,
    "section_text_bytes": $TEXT_SIZE
  },
  "startup_ms": {
    "min": $STARTUP_MIN,
    "avg": $STARTUP_AVG,
    "max": $STARTUP_MAX,
    "runs": $STARTUP_RUNS
  },
  "memory": {
    "peak_rss_tests_bytes": $TESTS_RSS,
    "peak_rss_version_bytes": $VERSION_RSS
  },
  "tests": {
    "count": $TEST_COUNT,
    "duration_sec": $TESTS_DURATION,
    "rate_per_sec": $RATE
  }
}
EOF

# --- Print table ---
print_row() {
    printf "  %-20s  %s\n" "$1" "$2"
}

print_section() {
    echo ""
    echo "  $1"
    echo "  ─────────────────"
}

echo ""
echo "═══════════════════════════════════════════════"
echo "  SeaClaw Benchmark Report"
echo "═══════════════════════════════════════════════"

print_section "Binary"
print_row "Size:" "$BINARY_HR"
print_row "Symbols:" "${SYMBOL_HR} exported"
[ -n "$TEXT_HR" ] && print_row "Text section:" "$TEXT_HR"

print_section "Startup (10 runs)"
print_row "Min:" "${STARTUP_MIN} ms"
print_row "Avg:" "${STARTUP_AVG} ms"
print_row "Max:" "${STARTUP_MAX} ms"

print_section "Memory"
print_row "Peak RSS (tests):" "$TESTS_RSS_HR"
print_row "Peak RSS (--version):" "$VERSION_RSS_HR"

print_section "Tests"
print_row "Count:" "$TEST_COUNT_HR"
print_row "Duration:" "${TESTS_DURATION}s"
print_row "Rate:" "$(printf "%'d" "$RATE") tests/sec"

echo ""
echo "═══════════════════════════════════════════════"
echo "Results saved to: $JSON_FILE"
echo ""

# --- Compare with previous ---
if [ -n "$COMPARE_FILE" ] && [ -f "$COMPARE_FILE" ]; then
    echo "Comparing with $COMPARE_FILE:"
    "${SCRIPTS_DIR}/compare-benchmarks.sh" "$COMPARE_FILE" "$JSON_FILE" 2>/dev/null || true
fi
