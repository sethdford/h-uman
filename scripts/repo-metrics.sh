#!/usr/bin/env bash
# Compute authoritative repo metrics from the actual codebase.
# Used by check-metrics-drift.sh to verify doc claims stay current.
# Output: KEY=VALUE pairs, one per line (machine-readable).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

MODE="${1:-}"

test_files=$(find tests -name '*.c' 2>/dev/null | wc -l | tr -d ' ')
test_cases=$(grep -r 'HU_RUN_TEST' tests/ --include='*.c' 2>/dev/null | wc -l | tr -d ' ')

src_c_files=$(find src -name '*.c' 2>/dev/null | wc -l | tr -d ' ')
src_h_files=$(find src -name '*.h' 2>/dev/null | wc -l | tr -d ' ')
include_h_files=$(find include -name '*.h' 2>/dev/null | wc -l | tr -d ' ')
total_source_header=$((src_c_files + src_h_files + include_h_files))

# SRC_LOC measures src/ only (no include/) — MUST match update-stats.sh
# C_LINES_RAW, the writer that stamps "~NNNK lines of C" into the docs. When the
# two scopes diverged (writer src/+include/ = 484K vs checker src/ = 421K) the
# drift gate rejected the writer's own output. Same writer≠checker convergence
# contract as CHANNEL_ENUM in update-stats.sh.
src_loc=$(find src -name '*.[ch]' 2>/dev/null | xargs wc -l 2>/dev/null | tail -1 | awk '{print $1}')
test_loc=$(find tests -name '*.c' 2>/dev/null | xargs wc -l 2>/dev/null | tail -1 | awk '{print $1}')

channel_c_files=$(find src/channels -maxdepth 1 -name '*.c' 2>/dev/null | wc -l | tr -d ' ')
channel_enum=$(grep -cE '^\s+HU_CHANNEL_[A-Z_]+,' include/human/channel_catalog.h 2>/dev/null | tr -d ' ')

tool_c_files=$(find src/tools -name '*.c' 2>/dev/null | wc -l | tr -d ' ')

provider_c_files=$(find src/providers -maxdepth 1 -name '*.c' 2>/dev/null | wc -l | tr -d ' ')

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
