#!/usr/bin/env bash
# check-no-new-root-files.sh
#
# Ratchet: the count of loose src/*.c (no bounded-context dir) may only shrink.
# E1 relocations lower ROOT_BASELINE; floor is 0, then flip to a hard "no new root .c" gate.
set -euo pipefail

# Auto-lock any gain so it can never be spent again (scripts/ratchet-config.tsv).
# Sourced defensively: this gate must keep working — and keep BLOCKING growth —
# even in a tree where the helper is absent, so a missing helper degrades to
# "no auto-lock" rather than to "commit refused".
_hu_root="$(git rev-parse --show-toplevel 2>/dev/null || echo .)"
if [ -r "$_hu_root/scripts/lib/ratchet.sh" ]; then
    . "$_hu_root/scripts/lib/ratchet.sh"
else
    ratchet_autolock() { :; }
fi

# Measured 2026-05-31
ROOT_BASELINE=4

cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

n=$(find src -maxdepth 1 -name '*.c' | wc -l | tr -d ' ')
echo "loose src/*.c at root: $n (ceiling $ROOT_BASELINE)"
ratchet_autolock ROOT_BASELINE "${n}" "scripts/check-no-new-root-files.sh"
if [ "$n" -gt "$ROOT_BASELINE" ]; then
  echo "FAIL: a new file landed loose at src/ root. Put it in a bounded context" >&2
  echo "      (src/<context>/), not src/. See docs/standards/engineering/bounded-contexts.md." >&2
  find src -maxdepth 1 -name '*.c' | sed 's/^/  /' >&2
  exit 1
elif [ "$n" -lt "$ROOT_BASELINE" ]; then
  [ "${HU_RATCHET_LOCKED:-0}" = 1 ] || \
  echo "NOTE: root count dropped to $n — lower ROOT_BASELINE to lock the gain." >&2
fi
