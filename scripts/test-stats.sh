#!/usr/bin/env bash
# Print authoritative test counts for CLAUDE.md and CI.
#
# Three numbers, three definitions:
#   1. Test files      — count of tests/*.c
#   2. Test functions  — count of `void test_*(void)` and `static void test_*(void)` defs
#   3. Cases passed    — the framework's own count from a full run (most authoritative)
#
# The third number is what CLAUDE.md should quote; the framework expands
# parameterized tests, so this number is always >= the function count.
#
# Usage:
#   scripts/test-stats.sh                # static counts only (fast)
#   scripts/test-stats.sh --run          # also build + run to get case count
#
# Exit code is always 0 unless something is structurally wrong.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

run_full=0
if [[ "${1:-}" == "--run" ]]; then
  run_full=1
fi

test_files=$(find tests -maxdepth 1 -name 'test_*.c' -type f 2>/dev/null | wc -l | tr -d ' ')
test_funcs=$(grep -rE '^(static[[:space:]]+)?void[[:space:]]+test_[a-zA-Z0-9_]+\(' tests/ 2>/dev/null | wc -l | tr -d ' ')

echo "test_files=${test_files}"
echo "test_functions=${test_funcs}"

if [[ "$run_full" -eq 1 ]]; then
  if [[ ! -x build/human_tests ]]; then
    cmake --preset dev >/dev/null
    cmake --build --preset dev -j >/dev/null
  fi
  # The framework prints "--- Results: N/N passed ---" at the end.
  cases=$(./build/human_tests 2>&1 | awk '/^--- Results: [0-9]+\/[0-9]+ passed/ {print $3}' | head -1)
  echo "cases_passed=${cases}"
fi
