#!/usr/bin/env bash
# W7 Phase 1 — list `hu_graph_*` touchpoints under agent / persona / feeds.
# For docs/plans/2026-05-10-w7-phase1-bypass-inventory.md and trend tracking.
# Requires ripgrep (`rg`). Exit 0 (informational).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
if ! command -v rg >/dev/null 2>&1; then
    printf '%s\n' "w7-phase1-graph-bypass-inventory: rg not found; install ripgrep" >&2
    exit 0
fi
printf '# hu_graph_* matches (per file, descending by count)\n\n'
set +e
rg 'hu_graph_' src/agent src/persona src/feeds -g '*.c' -g '*.h' --count-matches 2>/dev/null \
    | sort -t: -k2 -nr \
    | awk -F: '{printf "%5d  %s\n", $2, $1; t+=$2} END {printf "\nTOTAL  %d\n", t+0}'
rg_exit=${PIPESTATUS[0]}
set -e
if [ "$rg_exit" -ne 0 ] && [ "$rg_exit" -ne 1 ]; then
    exit "$rg_exit"
fi
