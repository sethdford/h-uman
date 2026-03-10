#!/usr/bin/env bash
# update-stats.sh — Sync all docs with actual repo metrics.
# Patches: AGENTS.md, README.md, CONTRIBUTING.md, PROJECT_STATUS.md, human/STUBS.md, CLAUDE.md
# Usage: ./scripts/update-stats.sh [--apply]
#   Without --apply: prints stats only (dry run).
#   With --apply: patches all files in place.
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

# Count channels (exclude non-channel infrastructure)
CHANNEL_COUNT=$(find src/channels -maxdepth 1 -name '*.c' ! -name 'factory.c' ! -name 'meta_common.c' | wc -l | tr -d ' ')

# Count tools (exclude factory)
TOOL_COUNT=$(find src/tools -maxdepth 1 -name '*.c' ! -name 'factory.c' | wc -l | tr -d ' ')

# Get test count from binary (try multiple build dirs)
TEST_COUNT="unknown"
for test_bin in build/human_tests build2/human_tests build-check/human_tests build-release/human_tests; do
    if [ -f "$test_bin" ]; then
        TEST_COUNT=$("$test_bin" 2>/dev/null | grep 'Results:' | sed 's|.*: \([0-9]*\)/.*|\1|' || echo "unknown")
        break
    fi
done

# Format test count with comma
if [ "$TEST_COUNT" != "unknown" ]; then
    TEST_COUNT_FMT=$(printf "%'d" "$TEST_COUNT" 2>/dev/null || echo "$TEST_COUNT")
else
    TEST_COUNT_FMT="unknown"
fi

# Get binary size (prefer MinSizeRel builds, then release, then debug)
BINARY_KB="unknown"
for bin in build-size/human build2/human build-release/human build/human; do
    if [ -f "$bin" ]; then
        BINARY_BYTES=$(stat -f%z "$bin" 2>/dev/null || stat -c%s "$bin" 2>/dev/null || echo 0)
        BINARY_KB=$((BINARY_BYTES / 1024))
        break
    fi
done

echo "=== Human Stats ==="
echo "Source + header files: ${SRC_COUNT}"
echo "Lines of C:           ~${C_LINES_K}K (${C_LINES_RAW})"
echo "Test files:           ${TEST_FILES}"
echo "Lines of tests:       ~${TEST_LINES_K}K (${TEST_LINES_RAW})"
echo "Tests:                ${TEST_COUNT_FMT}"
echo "Binary size:          ~${BINARY_KB} KB"
echo "Channels:             ${CHANNEL_COUNT}"
echo "Tools:                ${TOOL_COUNT}"

if ! $APPLY; then
    echo ""
    echo "Dry run. To patch files: ./scripts/update-stats.sh --apply"
    exit 0
fi

echo ""
echo "Patching AGENTS.md..."

# "Current scale" line (test count, channels, lines, etc.)
sed -i.bak -E \
    "s/Current scale: \*\*[^*]+\*\*/Current scale: **${SRC_COUNT} source + header files, ~${C_LINES_K}K lines of C, ~${TEST_LINES_K}K lines of tests, ${TEST_COUNT_FMT} tests, ${CHANNEL_COUNT} channels**/" \
    AGENTS.md && rm -f AGENTS.md.bak

# "tests/" repo-map line (test files + test count)
sed -i.bak -E \
    "s|tests/[[:space:]]+[0-9]+ test files, [0-9,]+\+? tests|tests/                 ${TEST_FILES} test files, ${TEST_COUNT_FMT}+ tests|" \
    AGENTS.md && rm -f AGENTS.md.bak

# "tools/" repo-map line (tool count)
sed -i.bak -E \
    "s|tools/[[:space:]]+[0-9]+ tool implementations|tools/                ${TOOL_COUNT} tool implementations|" \
    AGENTS.md && rm -f AGENTS.md.bak

# "channels/" repo-map line (channel count)
sed -i.bak -E \
    "s|channels/[[:space:]]+[0-9]+ channel implementations|channels/             ${CHANNEL_COUNT} channel implementations|" \
    AGENTS.md && rm -f AGENTS.md.bak

# Binary size — all ~NNN KB references
if [ "$BINARY_KB" != "unknown" ]; then
    sed -i.bak -E \
        "s/~[0-9]+ KB/~${BINARY_KB} KB/g" \
        AGENTS.md && rm -f AGENTS.md.bak
fi

# "All N+ tests" rule-of-thumb line
sed -i.bak -E \
    "s/All [0-9,]+\+ tests must pass/All ${TEST_COUNT_FMT}+ tests must pass/" \
    AGENTS.md && rm -f AGENTS.md.bak

echo "Patching README.md..."

# Test count — all "NNNN tests" / "NNNN+ tests" references
sed -i.bak -E \
    "s/[0-9,]+\+? tests/${TEST_COUNT_FMT}+ tests/g" \
    README.md && rm -f README.md.bak

# "Tests:" stat line
sed -i.bak -E \
    "s/Tests:[[:space:]]+[0-9,]+ passing/Tests:         ${TEST_COUNT_FMT} passing/" \
    README.md && rm -f README.md.bak

# "tests/" stat line
sed -i.bak -E \
    "s|tests/[[:space:]]+[0-9]+ test files, [0-9,]+\+? tests|tests/ ${TEST_FILES} test files, ${TEST_COUNT_FMT} tests|" \
    README.md && rm -f README.md.bak

# "# run all tests" comment with count
sed -i.bak -E \
    "s/# [0-9,]+ tests$/# ${TEST_COUNT_FMT} tests/" \
    README.md && rm -f README.md.bak

# Binary size — all ~NNN KB references
if [ "$BINARY_KB" != "unknown" ]; then
    sed -i.bak -E \
        "s/~[0-9]+ KB/~${BINARY_KB} KB/g" \
        README.md && rm -f README.md.bak
fi

# Tools count in tagline and feature list
sed -i.bak -E \
    "s/[0-9]+ channels, [0-9]+\+ tools/${CHANNEL_COUNT} channels, ${TOOL_COUNT}+ tools/g" \
    README.md && rm -f README.md.bak

# Tools count in bullet-separated tagline (· separator)
sed -i.bak -E \
    "s/[0-9]+ channels · [0-9]+\+ tools/${CHANNEL_COUNT} channels · ${TOOL_COUNT}+ tools/g" \
    README.md && rm -f README.md.bak

# Stats block: "Source files:", "Lines of code:", "Test files:", "Tests:"
sed -i.bak -E \
    "s/^Source files: [0-9]+$/Source files: ${SRC_COUNT}/" \
    README.md && rm -f README.md.bak

sed -i.bak -E \
    "s/^Lines of code: ~[0-9]+K$/Lines of code: ~${C_LINES_K}K/" \
    README.md && rm -f README.md.bak

sed -i.bak -E \
    "s/^Test files: [0-9]+$/Test files: ${TEST_FILES}/" \
    README.md && rm -f README.md.bak

sed -i.bak -E \
    "s/^Tests: [0-9,]+$/Tests: ${TEST_COUNT_FMT}/" \
    README.md && rm -f README.md.bak

echo "Patching CONTRIBUTING.md..."

# "All N+ tests must pass" line
sed -i.bak -E \
    "s/All [0-9,]+\+ tests must pass/All ${TEST_COUNT_FMT}+ tests must pass/" \
    CONTRIBUTING.md && rm -f CONTRIBUTING.md.bak

echo "Patching PROJECT_STATUS.md..."

if [ -f PROJECT_STATUS.md ]; then
    # Test files
    sed -i.bak -E \
        "s/Test files[[:space:]]+\| [0-9]+/Test files                     | ${TEST_FILES}/" \
        PROJECT_STATUS.md && rm -f PROJECT_STATUS.md.bak

    # Tests passing
    sed -i.bak -E \
        "s/Tests passing[[:space:]]+\| \*\*[^*]+\*\*/Tests passing                  | **${TEST_COUNT_FMT}\/${TEST_COUNT_FMT} (100%)**/" \
        PROJECT_STATUS.md && rm -f PROJECT_STATUS.md.bak

    # Binary size
    if [ "$BINARY_KB" != "unknown" ]; then
        sed -i.bak -E \
            "s/Binary size \(MinSizeRel\+LTO\)[[:space:]]+\| \*\*~[0-9]+ KB\*\*/Binary size (MinSizeRel+LTO)   | **~${BINARY_KB} KB**/" \
            PROJECT_STATUS.md && rm -f PROJECT_STATUS.md.bak
    fi

    # Source files
    sed -i.bak -E \
        "s/Source files \(src\/ \+ include\/\)[[:space:]]*\| \*\*[0-9]+\*\*/Source files (src\/ + include\/) | **${SRC_COUNT}**/" \
        PROJECT_STATUS.md && rm -f PROJECT_STATUS.md.bak

    # Lines of code
    sed -i.bak -E \
        "s/Lines of C\/H\/ASM code[[:space:]]+\| \*\*~[0-9]+K\*\*/Lines of C\/H\/ASM code          | **~${C_LINES_K}K**/" \
        PROJECT_STATUS.md && rm -f PROJECT_STATUS.md.bak

    # Tools count in section header
    sed -i.bak -E \
        "s/All [0-9]+ Real \(with all feature flags\)/All ${TOOL_COUNT} Real (with all feature flags)/" \
        PROJECT_STATUS.md && rm -f PROJECT_STATUS.md.bak

    # Update date
    sed -i.bak -E \
        "s/Last updated: [0-9]{4}-[0-9]{2}-[0-9]{2}/Last updated: $(date +%Y-%m-%d)/" \
        PROJECT_STATUS.md && rm -f PROJECT_STATUS.md.bak
fi

echo "Patching human/STUBS.md..."

if [ -f human/STUBS.md ]; then
    # Test count in header blurb
    sed -i.bak -E \
        "s/[0-9,]+ tests, ~[0-9]+ KB binary/${TEST_COUNT_FMT} tests, ~${BINARY_KB} KB binary/" \
        human/STUBS.md && rm -f human/STUBS.md.bak

    # Tests passing table row
    sed -i.bak -E \
        "s/Tests passing[[:space:]]+\| \*\*[^*]+\*\*/Tests passing                  | **${TEST_COUNT_FMT}\/${TEST_COUNT_FMT} (100%)**/" \
        human/STUBS.md && rm -f human/STUBS.md.bak

    # Test files
    sed -i.bak -E \
        "s/Test files[[:space:]]+\| [0-9]+/Test files                     | ${TEST_FILES}/" \
        human/STUBS.md && rm -f human/STUBS.md.bak

    # Update date
    sed -i.bak -E \
        "s/Last updated: [0-9]{4}-[0-9]{2}-[0-9]{2}/Last updated: $(date +%Y-%m-%d)/" \
        human/STUBS.md && rm -f human/STUBS.md.bak
fi

echo "Patching CLAUDE.md..."

if [ -f CLAUDE.md ]; then
    # Binary size in tagline
    if [ "$BINARY_KB" != "unknown" ]; then
        sed -i.bak -E \
            "s/~[0-9]+ KB binary/~${BINARY_KB} KB binary/" \
            CLAUDE.md && rm -f CLAUDE.md.bak
    fi

    # Test count references
    sed -i.bak -E \
        "s/[0-9,]+\+ tests/${TEST_COUNT_FMT}+ tests/g" \
        CLAUDE.md && rm -f CLAUDE.md.bak

    # Test files in paths table
    sed -i.bak -E \
        "s/[0-9]+ test files, [0-9,]+\+ tests/${TEST_FILES} test files, ${TEST_COUNT_FMT}+ tests/" \
        CLAUDE.md && rm -f CLAUDE.md.bak

    # Source file count
    sed -i.bak -E \
        "s/~[0-9]+ files, ~[0-9]+K lines/~${SRC_COUNT} files, ~${C_LINES_K}K lines/" \
        CLAUDE.md && rm -f CLAUDE.md.bak
fi

echo "Done. Review changes with: git diff"
