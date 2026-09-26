#!/usr/bin/env bash
# check-file-size-ceiling.sh — no src/*.c may exceed the current max-LOC ratchet.
# Ratchet: lower MAX_BASELINE whenever the largest file shrinks (E2 drives daemon.c down).
# Aspirational target documented in .claude/rules/file-size-ceiling.md: 800 LOC.
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

MAX_BASELINE=10511   # measured 2026-09-26 on the origin/main merge into PR #440.
                     # main's #438 carved src/daemon.c to 10256 (10264 here with this
                     # branch's changes), so the largest file is src/agent/agent_turn.c,
                     # which the dead-code sweep had taken to 10511 against main's 10512.
                     # Both conflicting sides (branch 10522, main 10512) sat above the
                     # merged tree; this is its own measurement, so the merge tightens.
                     # (was 10574 before the 09-20 carve, 14058 before the reactive
                     # context/prompt carves.) Lower as god-files are carved.

cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

worst=$(find src -name '*.c' 2>/dev/null | xargs wc -l 2>/dev/null | awk '$2!="total"' | sort -rn | sed -n '1p' || true)
worst_loc=$(echo "$worst" | awk '{print $1}')
worst_file=$(echo "$worst" | awk '{print $2}')

echo "largest src/*.c: $worst_file = $worst_loc LOC (ceiling $MAX_BASELINE)"
ratchet_autolock MAX_BASELINE "${worst_loc}" "scripts/check-file-size-ceiling.sh"

fail=0

if [ -n "$worst_loc" ] && [ "$worst_loc" -gt "$MAX_BASELINE" ]; then
  echo "FAIL: a file grew past the size ratchet. Split it, or it cannot land." >&2
  find src -name '*.c' 2>/dev/null | xargs wc -l 2>/dev/null | awk -v b="$MAX_BASELINE" '$2!="total" && $1>b {print "  "$0}' >&2 || true
  fail=1
elif [ -n "$worst_loc" ] && [ "$worst_loc" -lt "$MAX_BASELINE" ]; then
  [ "${HU_RATCHET_LOCKED:-0}" = 1 ] || \
  echo "NOTE: largest file shrank to $worst_loc — lower MAX_BASELINE to lock the gain." >&2
fi

exit $fail
