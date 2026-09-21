#!/usr/bin/env bash
# check-realpath-wrapper.sh
#
# Absolute gate: no raw realpath() call may appear in src/ outside
# src/app/platform.c. Every path resolution goes through
# hu_platform_realpath (include/human/platform.h) so the Windows
# (_fullpath) and allocator-ownership contract is honored everywhere.
# See docs/standards/engineering/cross-platform.md and
# .claude/rules/ (Task 20: route raw realpath() through
# hu_platform_realpath). Ceiling is 0.
set -euo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

hits=$(grep -rnE '[^_a-zA-Z]realpath\(' src --include='*.c' --include='*.m' \
  | grep -v '^src/app/platform\.c:' || true)
n=$(printf '%s' "$hits" | grep -c . || true)
echo "raw realpath() calls outside src/app/platform.c: $n (ceiling 0)"
if [ "$n" -gt 0 ]; then
  echo "FAIL: call hu_platform_realpath(alloc, path) instead of raw realpath()." >&2
  echo "      See include/human/platform.h and docs/standards/engineering/cross-platform.md." >&2
  printf '%s\n' "$hits" | sed 's/^/  /' >&2
  exit 1
fi
