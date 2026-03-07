#!/usr/bin/env bash
# update-stats.sh — Sync AGENTS.md and README.md with actual repo metrics.
# Usage: ./scripts/update-stats.sh [--apply]
#   Without --apply: prints stats only (dry run).
#   With --apply: patches AGENTS.md and README.md in place.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

APPLY=false
[ "${1:-}" = "--apply" ] && APPLY=true

# Count source + header files
SRC_COUNT=$(find src include \( -name '*.c' -o -name '*.h' \) | wc -l | tr -d ' ')

# Count lines of C (round to nearest K)
C_LINES_RAW=$(find src include \( -name '*.c' -o -name '*.h' \) -exec cat {} + | wc -l | tr -d ' ')
C_LINES_K=$(( (C_LINES_RAW + 500) / 1000 ))

# Count test files
TEST_FILES=$(find tests -name 'test_*.c' | wc -l | tr -d ' ')

# Count test lines (round to nearest K)
TEST_LINES_RAW=$(find tests \( -name '*.c' -o -name '*.h' \) -exec cat {} + | wc -l | tr -d ' ')
TEST_LINES_K=$(( (TEST_LINES_RAW + 500) / 1000 ))

# Count channels
CHANNEL_COUNT=$(find src/channels -maxdepth 1 -name '*.c' ! -name 'factory.c' ! -name 'meta_common.c' | wc -l | tr -d ' ')

# Count tools
TOOL_COUNT=$(find src/tools -maxdepth 1 -name '*.c' ! -name 'factory.c' | wc -l | tr -d ' ')

# Get test count from binary
if [ -f build/seaclaw_tests ]; then
    TEST_COUNT=$(build/seaclaw_tests 2>/dev/null | grep 'Results:' | sed 's|.*: \([0-9]*\)/.*|\1|' || echo "unknown")
else
    TEST_COUNT="unknown"
fi

# Format test count with comma
if [ "$TEST_COUNT" != "unknown" ]; then
    TEST_COUNT_FMT=$(printf "%'d" "$TEST_COUNT" 2>/dev/null || echo "$TEST_COUNT")
else
    TEST_COUNT_FMT="unknown"
fi

echo "=== SeaClaw Stats ==="
echo "Source + header files: ${SRC_COUNT}"
echo "Lines of C:           ~${C_LINES_K}K (${C_LINES_RAW})"
echo "Test files:           ${TEST_FILES}"
echo "Lines of tests:       ~${TEST_LINES_K}K (${TEST_LINES_RAW})"
echo "Tests:                ${TEST_COUNT_FMT}"
echo "Channels:             ${CHANNEL_COUNT}"
echo "Tools:                ${TOOL_COUNT}"

if ! $APPLY; then
    echo ""
    echo "Dry run. To patch files: ./scripts/update-stats.sh --apply"
    exit 0
fi

echo ""
echo "Patching AGENTS.md..."

# Patch AGENTS.md "Current scale" line
sed -i.bak -E \
    "s/Current scale: \*\*[^*]+\*\*/Current scale: **${SRC_COUNT} source + header files, ~${C_LINES_K}K lines of C, ~${TEST_LINES_K}K lines of tests, ${TEST_COUNT_FMT} tests, ${CHANNEL_COUNT} channels**/" \
    AGENTS.md && rm -f AGENTS.md.bak

echo "Patching README.md..."

# Patch README.md tagline (line with tests · providers · channels · tools)
sed -i.bak -E \
    "s/[0-9,]+ tests/${TEST_COUNT_FMT} tests/g" \
    README.md && rm -f README.md.bak

# Patch README.md "Tests:" lines
sed -i.bak -E \
    "s/Tests:[[:space:]]+[0-9,]+ passing/Tests:         ${TEST_COUNT_FMT} passing/" \
    README.md && rm -f README.md.bak

# Patch README.md "tests/" stat line
sed -i.bak -E \
    "s|tests/[[:space:]]+[0-9]+ test files, [0-9,]+ tests|tests/ ${TEST_FILES} test files, ${TEST_COUNT_FMT} tests|" \
    README.md && rm -f README.md.bak

echo "Done. Review changes with: git diff AGENTS.md README.md"
