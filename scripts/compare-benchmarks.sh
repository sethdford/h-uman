#!/bin/sh
# Compare two Human benchmark JSON result files.
# Usage: ./scripts/compare-benchmarks.sh before.json after.json

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
        printf "Usage: %s before.json after.json\n" "$0"
        printf "Compare two Human benchmark JSON result files.\n"
        exit 0
        ;;
esac

if [ "$#" -lt 2 ]; then
    die "Usage: $0 before.json after.json"
fi

BEFORE="$1"
AFTER="$2"

if [ ! -f "$BEFORE" ]; then
    die "$BEFORE not found"
fi
if [ ! -f "$AFTER" ]; then
    die "$AFTER not found"
fi

# Colors
red=''
green=''
reset=''
if [ -t 1 ]; then
    red='\033[0;31m'
    green='\033[0;32m'
    reset='\033[0m'
fi

# Get numeric values (grep/sed for POSIX portability, no jq)
b_size_before=$(grep "size_bytes" "$BEFORE" | head -1 | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')
b_size_after=$(grep "size_bytes" "$AFTER" | head -1 | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')
b_sym_before=$(grep "symbols_exported" "$BEFORE" | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')
b_sym_after=$(grep "symbols_exported" "$AFTER" | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')
b_text_before=$(grep "section_text_bytes" "$BEFORE" | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')
b_text_after=$(grep "section_text_bytes" "$AFTER" | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')

s_min_before=$(grep '"min"' "$BEFORE" | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')
s_min_after=$(grep '"min"' "$AFTER" | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')
s_avg_before=$(grep '"avg"' "$BEFORE" | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')
s_avg_after=$(grep '"avg"' "$AFTER" | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')
s_max_before=$(grep '"max"' "$BEFORE" | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')
s_max_after=$(grep '"max"' "$AFTER" | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')

# Memory and tests
# Use line-based extraction: each key appears once in our JSON shape
m_rss_tests_before=$(grep "peak_rss_tests_bytes" "$BEFORE" | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')
m_rss_tests_after=$(grep "peak_rss_tests_bytes" "$AFTER" | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')
m_rss_version_before=$(grep "peak_rss_version_bytes" "$BEFORE" | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')
m_rss_version_after=$(grep "peak_rss_version_bytes" "$AFTER" | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')

# Tests section - "count" and "duration_sec" and "rate_per_sec" appear once
t_count_before=$(grep '"count"' "$BEFORE" | head -1 | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')
t_count_after=$(grep '"count"' "$AFTER" | head -1 | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')
t_dur_before=$(grep "duration_sec" "$BEFORE" | sed 's/.*: *\([0-9.]*\).*/\1/' | tr -d ' ')
t_dur_after=$(grep "duration_sec" "$AFTER" | sed 's/.*: *\([0-9.]*\).*/\1/' | tr -d ' ')
t_rate_before=$(grep "rate_per_sec" "$BEFORE" | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')
t_rate_after=$(grep "rate_per_sec" "$AFTER" | sed 's/.*: *\([0-9]*\).*/\1/' | tr -d ' ')

# Clean empty values
: "${b_size_before:=0}"
: "${b_size_after:=0}"
: "${b_sym_before:=0}"
: "${b_sym_after:=0}"
: "${b_text_before:=0}"
: "${b_text_after:=0}"
: "${s_min_before:=0}"
: "${s_min_after:=0}"
: "${s_avg_before:=0}"
: "${s_avg_after:=0}"
: "${s_max_before:=0}"
: "${s_max_after:=0}"
: "${m_rss_tests_before:=0}"
: "${m_rss_tests_after:=0}"
: "${m_rss_version_before:=0}"
: "${m_rss_version_after:=0}"
: "${t_count_before:=0}"
: "${t_count_after:=0}"
: "${t_dur_before:=0}"
: "${t_dur_after:=0}"
: "${t_rate_before:=0}"
: "${t_rate_after:=0}"

print_row() {
    metric="$1"
    before="$2"
    after="$3"
    unit="$4"
    lower_better="$5"
    # Use awk for delta (handles integers and floats)
    delta=$(awk -v a="$after" -v b="$before" 'BEGIN { printf "%.2f", a - b }' 2>/dev/null || echo "0")
    if [ "$lower_better" = "1" ]; then
        if echo "$delta" | grep -q '^-' && [ "$delta" != "0.00" ]; then
            stat="improvement"
        elif echo "$delta" | grep -q '^[0-9]' && [ "$delta" != "0" ] && [ "$delta" != "0.00" ]; then
            stat="regression"
        else
            stat="same"
        fi
    else
        if echo "$delta" | grep -q '^[0-9]' && [ "$delta" != "0" ] && [ "$delta" != "0.00" ]; then
            stat="improvement"
        elif echo "$delta" | grep -q '^-'; then
            stat="regression"
        else
            stat="same"
        fi
    fi
    delta_str="$delta"
    case "$delta" in
        -*)
            ;;
        *)
            [ "$delta" != "0" ] && [ "$delta" != "0.00" ] && delta_str="+${delta}"
            ;;
    esac
    case "$stat" in
        improvement) col="$green" ;;
        regression)  col="$red" ;;
        *)           col="" ;;
    esac
    printf "  %-24s  %12s  %12s  %12s  ${col}%s${reset}\n" "$metric" "$before $unit" "$after $unit" "$delta_str" "$stat"
}

echo ""
echo "  Metric                    Before          After           Delta           Status"
echo "  ────────────────────────────────────────────────────────────────────────────────"

# Binary size (lower better)
print_row "Binary size (bytes)" "$b_size_before" "$b_size_after" "" "1"
# Symbols (lower often better for binary size)
print_row "Symbols exported" "$b_sym_before" "$b_sym_after" "" "1"
# Startup (lower better)
print_row "Startup min (ms)" "$s_min_before" "$s_min_after" "" "1"
print_row "Startup avg (ms)" "$s_avg_before" "$s_avg_after" "" "1"
print_row "Startup max (ms)" "$s_max_before" "$s_max_after" "" "1"
# Memory (lower better)
print_row "RSS tests (bytes)" "$m_rss_tests_before" "$m_rss_tests_after" "" "1"
print_row "RSS --version (bytes)" "$m_rss_version_before" "$m_rss_version_after" "" "1"
# Tests (higher count/rate better, duration lower better)
print_row "Test count" "$t_count_before" "$t_count_after" "" "0"
print_row "Test duration (s)" "$t_dur_before" "$t_dur_after" "" "1"
print_row "Test rate (1/s)" "$t_rate_before" "$t_rate_after" "" "0"

echo ""
