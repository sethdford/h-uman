#!/usr/bin/env bash
# check-no-new-root-files.sh
#
# Ratchet: the count of loose src/*.c (no bounded-context dir) may only shrink.
# E1 relocations lower ROOT_BASELINE; floor is 0, then flip to a hard "no new root .c" gate.
set -euo pipefail

# Measured 2026-05-31
ROOT_BASELINE=4

cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

n=$(find src -maxdepth 1 -name '*.c' | wc -l | tr -d ' ')
echo "loose src/*.c at root: $n (ceiling $ROOT_BASELINE)"
if [ "$n" -gt "$ROOT_BASELINE" ]; then
  echo "FAIL: a new file landed loose at src/ root. Put it in a bounded context" >&2
  echo "      (src/<context>/), not src/. See docs/standards/engineering/bounded-contexts.md." >&2
  find src -maxdepth 1 -name '*.c' | sed 's/^/  /' >&2
  exit 1
elif [ "$n" -lt "$ROOT_BASELINE" ]; then
  echo "NOTE: root count dropped to $n — lower ROOT_BASELINE to lock the gain." >&2
fi
