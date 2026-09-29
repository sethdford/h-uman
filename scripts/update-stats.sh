#!/usr/bin/env bash
# update-stats.sh — Sync all docs with actual repo metrics.
# Patches: AGENTS.md, README.md, CONTRIBUTING.md, PROJECT_STATUS.md, human-skills/STUBS.md, CLAUDE.md
# Usage: ./scripts/update-stats.sh [--apply] [--test-count <N>]
#   Without --apply: prints stats only (dry run).
#   With --apply: patches all files in place.
#   --test-count <N>: trust this count (from a suite the caller just ran)
#       instead of executing a local test binary. N is the REGISTERED total,
#       passed + skipped from the Results: line (skips vary by machine; the
#       sum does not — see .githooks/pre-push). The pre-push hook passes
#       the N it parsed from its own Results: line; without it the script
#       re-ran whichever build*/human_tests sorted first — on 2026-09-03 a
#       two-day-old build/ binary — and stamped a count the hook never
#       verified (13,995 written, 14,125 measured). A count that was not
#       measured in this run is exactly what
#       .claude/rules/no-number-without-a-measurement.md forbids.
#
# Binary size, RSS and startup are NOT synced here. Each has exactly one writer,
# scripts/footprint.py, which renders them from a measurement with provenance
# (docs/perf/footprint.json). This script used to rewrite every "~N KB" with a
# regex from whatever release build it found, and stamped stale sizes three
# times (~23209 KB twice from Debug builds, ~2952 KB from a 5-week-old one).
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

USAGE="usage: update-stats.sh [--apply] [--test-count <N>]"
APPLY=false
TEST_COUNT_OVERRIDE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --apply) APPLY=true ;;
        --test-count)
            TEST_COUNT_OVERRIDE="${2:-}"
            case "$TEST_COUNT_OVERRIDE" in
                ''|*[!0-9]*) echo "error: --test-count requires a numeric value" >&2; exit 2 ;;
            esac
            shift ;;
        # Obsolete: binary size is owned by scripts/footprint.py. Accepted and
        # ignored, not rejected: every worktree runs .githooks from the SHARED
        # checkout (core.hooksPath), which can lag main by days, so an older
        # pre-push still passes --keep-binary-size to this newer script.
        --keep-binary-size) echo "note: --keep-binary-size is obsolete (binary size: scripts/footprint.py); ignoring" >&2 ;;
        --binary-size)
            echo "note: --binary-size is obsolete (binary size: scripts/footprint.py); ignoring" >&2
            shift ;;
        *) echo "error: unknown argument '$1' ($USAGE)" >&2; exit 2 ;;
    esac
    shift
done

# --- tracked-file counters --------------------------------------------------
# Every metric below reads git's INDEX, not the filesystem. src/data/data_*.c
# are generated embedded-data blobs, gitignored (.gitignore:152) but present in
# any checkout that has been built: counting them made the same commit measure
# 2105 files / 446,620 LOC built vs 2093 / 433,125 clean. The stamped numbers
# therefore depended on the machine, every push re-dirtied the docs, and
# "~445K lines of C" was counting embedded training data as source.
#
# grep exits 1 on no match and these scripts run under `set -e` + pipefail, so
# both helpers swallow that into a literal 0 / empty file list.
count_tracked() {  # count_tracked <ere> <pathspec...>
    _re="$1"; shift
    git ls-files -- "$@" | { grep -cE "$_re" || true; }
}
loc_tracked() {  # loc_tracked <ere> <pathspec...>  — total lines, xargs-batch safe
    _re="$1"; shift
    git ls-files -- "$@" | { grep -E "$_re" || true; } | xargs cat 2>/dev/null | wc -l | tr -d ' '
}

# Count source + header files
SRC_COUNT=$(count_tracked '\.(c|h)$' src include)

# Count lines of C (round to nearest K)
# MUST match scripts/repo-metrics.sh SRC_LOC (src/ only, no include/) — that is
# what the metrics-drift gate checks "[0-9]+K lines of C" claims against.
C_LINES_RAW=$(loc_tracked '\.(c|h)$' src)
C_LINES_K=$(( (C_LINES_RAW + 500) / 1000 ))

# Count test files
TEST_FILES=$(count_tracked '(^|/)test_[^/]*\.c$' tests)

# Count test lines (round to nearest K)
TEST_LINES_RAW=$(loc_tracked '\.(c|h)$' tests)
TEST_LINES_K=$(( (TEST_LINES_RAW + 500) / 1000 ))

# Channel .c file count — for the "N channel implementations" repo-map line only.
CHANNEL_COUNT=$(git ls-files -- src/channels | { grep -E '^src/channels/[^/]+\.c$' || true; } | { grep -vcE '/(factory|meta_common)\.c$' || true; })
# Canonical channel count = HU_CHANNEL_* enum entries in channel_catalog.h.
# MUST match scripts/repo-metrics.sh — the source of truth the docs metrics-drift
# gate (scripts/check-metrics-drift.sh) checks. Use this for every "N channels"
# phrasing so the writer and the gate agree; otherwise pre-push rewrites the count
# to the .c-file tally and re-breaks the docs gate on the next push.
CHANNEL_ENUM=$(grep -cE '^[[:space:]]+HU_CHANNEL_[A-Z_]+,' include/human/channel_catalog.h 2>/dev/null | tr -d ' ')

# Count tools (exclude factory)
TOOL_COUNT=$(git ls-files -- src/tools | { grep -E '^src/tools/[^/]+\.c$' || true; } | { grep -vcE '/factory\.c$' || true; })

# Get test count: the caller's measurement if given, else re-run a binary.
# build-check/ comes first — it is what the pre-push hook built from THIS tree
# moments ago; build/ is a developer's dev build of whatever tree state it was
# last configured against. Say which one ran, and how old it is, so a stale
# count is at least visible in the hook output.
TEST_COUNT="unknown"
if [ -n "$TEST_COUNT_OVERRIDE" ]; then
    TEST_COUNT="$TEST_COUNT_OVERRIDE"
    echo "Test count: ${TEST_COUNT} (from --test-count; no binary executed)"
else
    for test_bin in build-check/human_tests build/human_tests build2/human_tests build-release/human_tests; do
        if [ -f "$test_bin" ]; then
            bin_mtime=$(stat -f '%Sm' -t '%Y-%m-%d %H:%M' "$test_bin" 2>/dev/null \
                || stat -c '%y' "$test_bin" 2>/dev/null | cut -c1-16 || echo "?")
            echo "Test count: running ${test_bin} (mtime ${bin_mtime}) — pass --test-count to use a count you already measured"
            # Registered total = passed + skipped, same parse as .githooks/pre-push.
            results_line=$("$test_bin" 2>/dev/null | grep '^--- Results: ' | head -1 || true)
            passed=$(printf '%s\n' "$results_line" | sed -n 's|^--- Results: \([0-9][0-9]*\)/.*|\1|p')
            skipped=$(printf '%s\n' "$results_line" | sed -n 's|.*, \([0-9][0-9]*\) skipped.*|\1|p')
            TEST_COUNT=$([ -n "$passed" ] && echo $((passed + ${skipped:-0})) || echo "unknown")
            break
        fi
    done
fi

# Treat an empty extraction (binary ran but no Results: line) as unknown too
[ -n "$TEST_COUNT" ] || TEST_COUNT="unknown"

# Format the README stats-block counts with a thousands separator: that block
# reads "Source files: 1,093", and the comma-less pattern this script used
# never matched it, so the line sat at 1,093 while the tree grew past 2,000.
SRC_COUNT_FMT=$(printf "%'d" "$SRC_COUNT" 2>/dev/null || echo "$SRC_COUNT")

# Format test count with comma
if [ "$TEST_COUNT" != "unknown" ]; then
    TEST_COUNT_FMT=$(printf "%'d" "$TEST_COUNT" 2>/dev/null || echo "$TEST_COUNT")
else
    TEST_COUNT_FMT="unknown"
    echo "WARN: no test binary found in build*/ — leaving test-count claims untouched" >&2
fi

echo "=== Human Stats ==="
echo "Source + header files: ${SRC_COUNT}"
echo "Lines of C:           ~${C_LINES_K}K (${C_LINES_RAW})"
echo "Test files:           ${TEST_FILES}"
echo "Lines of tests:       ~${TEST_LINES_K}K (${TEST_LINES_RAW})"
echo "Tests:                ${TEST_COUNT_FMT}"
echo "Channels (enum):      ${CHANNEL_ENUM}"
echo "Channel .c files:     ${CHANNEL_COUNT}"
echo "Tools:                ${TOOL_COUNT}"

if ! $APPLY; then
    echo ""
    echo "Dry run. To patch files: ./scripts/update-stats.sh --apply"
    exit 0
fi

echo ""
echo "Patching AGENTS.md..."

# "Current scale" line (test count, channels, lines, etc.)
if [ "$TEST_COUNT" != "unknown" ]; then
    sed -i.bak -E \
        "s/Current scale: \*\*[^*]+\*\*/Current scale: **${SRC_COUNT} source + header files, ~${C_LINES_K}K lines of C, ~${TEST_LINES_K}K lines of tests, ${TEST_COUNT_FMT} tests, ${CHANNEL_ENUM} channels**/" \
        AGENTS.md && rm -f AGENTS.md.bak
fi

# "tests/" repo-map line (test files + test count)
if [ "$TEST_COUNT" != "unknown" ]; then
    sed -i.bak -E \
        "s|tests/[[:space:]]+[0-9]+ test files, [0-9,]+\+? tests|tests/                 ${TEST_FILES} test files, ${TEST_COUNT_FMT}+ tests|" \
        AGENTS.md && rm -f AGENTS.md.bak
fi

# "tools/" repo-map line (tool count)
sed -i.bak -E \
    "s|tools/[[:space:]]+[0-9]+ tool implementations|tools/                ${TOOL_COUNT} tool implementations|" \
    AGENTS.md && rm -f AGENTS.md.bak

# "channels/" repo-map line (channel count)
sed -i.bak -E \
    "s|channels/[[:space:]]+[0-9]+ channel implementations|channels/             ${CHANNEL_COUNT} channel implementations|" \
    AGENTS.md && rm -f AGENTS.md.bak

# "All N+ tests" rule-of-thumb line
if [ "$TEST_COUNT" != "unknown" ]; then
    sed -i.bak -E \
        "s/All [0-9,]+\+ tests must pass/All ${TEST_COUNT_FMT}+ tests must pass/" \
        AGENTS.md && rm -f AGENTS.md.bak
fi

echo "Patching README.md..."

# Test count — all "NNNN tests" / "NNNN+ tests" references
if [ "$TEST_COUNT" != "unknown" ]; then
    sed -i.bak -E \
        "s/[0-9,]+\+? tests/${TEST_COUNT_FMT}+ tests/g" \
        README.md && rm -f README.md.bak
fi

# "Tests:" stat line. Both this and the "^Tests: N" block pattern below accept
# a trailing "+": the committed lines read "11,924+ passing" and "6374+", so
# the strict patterns never matched and the two lines rotted from 2026-07 on.
if [ "$TEST_COUNT" != "unknown" ]; then
    sed -i.bak -E \
        "s/Tests:[[:space:]]+[0-9,]+\+? passing/Tests:         ${TEST_COUNT_FMT} passing/" \
        README.md && rm -f README.md.bak
fi

# "tests/" stat line
if [ "$TEST_COUNT" != "unknown" ]; then
    sed -i.bak -E \
        "s|tests/[[:space:]]+[0-9]+ test files, [0-9,]+\+? tests|tests/ ${TEST_FILES} test files, ${TEST_COUNT_FMT} tests|" \
        README.md && rm -f README.md.bak
fi

# "# run all tests" comment with count
if [ "$TEST_COUNT" != "unknown" ]; then
    sed -i.bak -E \
        "s/# [0-9,]+ tests$/# ${TEST_COUNT_FMT} tests/" \
        README.md && rm -f README.md.bak
fi

# Tools count in tagline and feature list
sed -i.bak -E \
    "s/[0-9]+ channels, [0-9]+\+ tools/${CHANNEL_ENUM} channels, ${TOOL_COUNT}+ tools/g" \
    README.md && rm -f README.md.bak

# Tools count in bullet-separated tagline (· separator)
sed -i.bak -E \
    "s/[0-9]+ channels · [0-9]+\+ tools/${CHANNEL_ENUM} channels · ${TOOL_COUNT}+ tools/g" \
    README.md && rm -f README.md.bak

# Stats block: "Source files:", "Lines of code:", "Test files:", "Tests:"
sed -i.bak -E \
    "s/^Source files: [0-9,]+$/Source files: ${SRC_COUNT_FMT}/" \
    README.md && rm -f README.md.bak

sed -i.bak -E \
    "s/^Lines of code: ~[0-9]+K$/Lines of code: ~${C_LINES_K}K/" \
    README.md && rm -f README.md.bak

sed -i.bak -E \
    "s/^Test files: [0-9]+$/Test files: ${TEST_FILES}/" \
    README.md && rm -f README.md.bak

if [ "$TEST_COUNT" != "unknown" ]; then
    sed -i.bak -E \
        "s/^Tests: [0-9,]+\+?$/Tests: ${TEST_COUNT_FMT}/" \
        README.md && rm -f README.md.bak
fi

echo "Patching CONTRIBUTING.md..."

# "All N+ tests must pass" line
if [ "$TEST_COUNT" != "unknown" ]; then
    sed -i.bak -E \
        "s/All [0-9,]+\+ tests must pass/All ${TEST_COUNT_FMT}+ tests must pass/" \
        CONTRIBUTING.md && rm -f CONTRIBUTING.md.bak
fi

echo "Patching PROJECT_STATUS.md..."

if [ -f PROJECT_STATUS.md ]; then
    ps_before=$(git hash-object PROJECT_STATUS.md)
    # Test files
    sed -i.bak -E \
        "s/Test files[[:space:]]+\| [0-9]+/Test files                     | ${TEST_FILES}/" \
        PROJECT_STATUS.md && rm -f PROJECT_STATUS.md.bak

    # Tests passing
    if [ "$TEST_COUNT" != "unknown" ]; then
        sed -i.bak -E \
            "s/Tests passing[[:space:]]+\| \*\*[^*]+\*\*/Tests passing                  | **${TEST_COUNT_FMT}\/${TEST_COUNT_FMT} (100%)**/" \
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

    # Update date — only when a metric above actually moved. An unconditional
    # stamp dirtied PROJECT_STATUS.md on every push that crossed a midnight
    # even when no number changed, so the hook's output never converged.
    if [ "$(git hash-object PROJECT_STATUS.md)" != "$ps_before" ]; then
        sed -i.bak -E \
            "s/Last updated: [0-9]{4}-[0-9]{2}-[0-9]{2}/Last updated: $(date +%Y-%m-%d)/" \
            PROJECT_STATUS.md && rm -f PROJECT_STATUS.md.bak
    fi
fi

echo "Patching human/STUBS.md..."

if [ -f human/STUBS.md ]; then
    # Test count in header blurb; skipped when the count itself is unknown —
    # never write "unknown" into docs
    if [ "$TEST_COUNT" != "unknown" ]; then
        sed -i.bak -E \
            "s/[0-9,]+ tests, ~([0-9]+) KB binary/${TEST_COUNT_FMT} tests, ~\1 KB binary/" \
            human/STUBS.md && rm -f human/STUBS.md.bak
    fi

    # Tests passing table row
    if [ "$TEST_COUNT" != "unknown" ]; then
        sed -i.bak -E \
            "s/Tests passing[[:space:]]+\| \*\*[^*]+\*\*/Tests passing                  | **${TEST_COUNT_FMT}\/${TEST_COUNT_FMT} (100%)**/" \
            human/STUBS.md && rm -f human/STUBS.md.bak
    fi

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
    # Test count references
    if [ "$TEST_COUNT" != "unknown" ]; then
        sed -i.bak -E \
            "s/[0-9,]+\+ tests/${TEST_COUNT_FMT}+ tests/g" \
            CLAUDE.md && rm -f CLAUDE.md.bak
    fi

    # Test files in paths table
    if [ "$TEST_COUNT" != "unknown" ]; then
        sed -i.bak -E \
            "s/[0-9]+ test files, [0-9,]+\+ tests/${TEST_FILES} test files, ${TEST_COUNT_FMT}+ tests/" \
            CLAUDE.md && rm -f CLAUDE.md.bak
    fi

    # Lines-of-C claim in the paths table. (The old pattern
    # '~[0-9]+ files, ~[0-9]+K lines' stopped matching when the row's phrasing
    # changed to "~1,050 `.c` files, ~NNNK lines of C", so CLAUDE.md silently
    # rotted while other docs got refreshed. Patch just the LOC figure, which is
    # what the drift gate checks.)
    sed -i.bak -E \
        "s/~[0-9]+K lines of C/~${C_LINES_K}K lines of C/g" \
        CLAUDE.md && rm -f CLAUDE.md.bak
fi

echo "Done. Review changes with: git diff"
