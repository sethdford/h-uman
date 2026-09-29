#!/usr/bin/env bash
# check-agent-flat-ratchet.sh
#
# Ratchet: the count of .c files directly in src/agent/ (not in a sub-package)
# may only shrink. DDD Phase E4 repackages agent/ into sub-packages; its target
# is <40 flat files (docs/plans/2026-05-29-ddd-bounded-contexts/phase-E4-repackaging.md).
#
# Why a gate: this was the one DDD counter with no ratchet, and the one that
# grew — 157 flat files on 2026-05-31, 162 on 2026-09-28 — while every
# ratcheted counter fell (docs/plans/2026-05-29-ddd-bounded-contexts/README.md).
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

# Measured 2026-09-28
AGENT_FLAT_BASELINE=162

cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

n=$(find src/agent -maxdepth 1 -name '*.c' | wc -l | tr -d ' ')
echo "flat src/agent/*.c: $n (ceiling $AGENT_FLAT_BASELINE)"
ratchet_autolock AGENT_FLAT_BASELINE "${n}" "scripts/check-agent-flat-ratchet.sh"
if [ "$n" -gt "$AGENT_FLAT_BASELINE" ]; then
  echo "FAIL: a new .c file landed directly in src/agent/. Put it in a sub-package" >&2
  echo "      (src/agent/<package>/) — see phase-E4-repackaging.md." >&2
  exit 1
elif [ "$n" -lt "$AGENT_FLAT_BASELINE" ]; then
  [ "${HU_RATCHET_LOCKED:-0}" = 1 ] || \
  echo "NOTE: flat agent/ count dropped to $n — lower AGENT_FLAT_BASELINE to lock the gain." >&2
fi
