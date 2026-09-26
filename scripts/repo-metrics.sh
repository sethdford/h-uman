#!/usr/bin/env bash
# Compute authoritative repo metrics from the actual codebase.
# Used by check-metrics-drift.sh to verify doc claims stay current.
# Output: KEY=VALUE pairs, one per line (machine-readable).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

MODE="${1:-}"

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

test_files=$(count_tracked '\.c$' tests)
test_cases=$(grep -r 'HU_RUN_TEST' tests/ --include='*.c' 2>/dev/null | wc -l | tr -d ' ')

src_c_files=$(count_tracked '\.c$' src)
src_h_files=$(count_tracked '\.h$' src)
include_h_files=$(count_tracked '\.h$' include)
total_source_header=$((src_c_files + src_h_files + include_h_files))

# SRC_LOC measures src/ only (no include/) — MUST match update-stats.sh
# C_LINES_RAW, the writer that stamps "~NNNK lines of C" into the docs. When the
# two scopes diverged (writer src/+include/ = 484K vs checker src/ = 421K) the
# drift gate rejected the writer's own output. Same writer≠checker convergence
# contract as CHANNEL_ENUM in update-stats.sh.
src_loc=$(loc_tracked '\.(c|h)$' src)
test_loc=$(loc_tracked '\.c$' tests)

channel_c_files=$(count_tracked '^src/channels/[^/]+\.c$' src/channels)
channel_enum=$(grep -cE '^\s+HU_CHANNEL_[A-Z_]+,' include/human/channel_catalog.h 2>/dev/null | tr -d ' ')

tool_c_files=$(count_tracked '\.c$' src/tools)

provider_c_files=$(count_tracked '^src/providers/[^/]+\.c$' src/providers)

fuzz_harnesses=$(find fuzz -name 'fuzz_*.c' 2>/dev/null | wc -l | tr -d ' ')

md_files=$(find . -name '*.md' -not -path '*/node_modules/*' -not -path '*/build*/*' -not -path '*/.git/*' 2>/dev/null | wc -l | tr -d ' ')

if [ "$MODE" = "--human" ]; then
  echo "=== Repo Metrics ==="
  echo "Test files:            $test_files"
  echo "Test cases:            $test_cases"
  echo "Source .c files:       $src_c_files"
  echo "Source+header files:   $total_source_header"
  echo "Lines of C (src/):     $src_loc"
  echo "Lines of test code:    $test_loc"
  echo "Channel .c files:      $channel_c_files"
  echo "Channels (enum):       $channel_enum"
  echo "Tool .c files:         $tool_c_files"
  echo "Provider .c files:     $provider_c_files"
  echo "Fuzz harnesses:        $fuzz_harnesses"
  echo "Markdown files:        $md_files"
else
  echo "TEST_FILES=$test_files"
  echo "TEST_CASES=$test_cases"
  echo "SRC_C_FILES=$src_c_files"
  echo "SRC_H_FILES=$src_h_files"
  echo "INCLUDE_H_FILES=$include_h_files"
  echo "TOTAL_SOURCE_HEADER=$total_source_header"
  echo "SRC_LOC=$src_loc"
  echo "TEST_LOC=$test_loc"
  echo "CHANNEL_C_FILES=$channel_c_files"
  echo "CHANNEL_ENUM=$channel_enum"
  echo "TOOL_C_FILES=$tool_c_files"
  echo "PROVIDER_C_FILES=$provider_c_files"
  echo "FUZZ_HARNESSES=$fuzz_harnesses"
  echo "MD_FILES=$md_files"
fi
