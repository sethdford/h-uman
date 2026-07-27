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

# Rebase resolution 2026-07-27: keep main's 14082 (it carved daemon.c further
# than the 14132 this branch was written against). This branch contributed the
# auto-lock wiring above, not a baseline change — the two edits collided only
# because they touch adjacent lines.
MAX_BASELINE=14082   # src/daemon.c, 2026-07-27 post-sched-send-outcome carve (was 14090)
                     # took daemon.c to 14085; 5 lines of headroom are reserved for the SIGHUP
                     # config-reload call site the carve was made to pay for. Net: -42.
                     # Lower as god-files are carved.

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
