#!/usr/bin/env bash
# W7 Phase 1 — list **v1 graph API call sites** under agent / persona / feeds.
# Counts only `hu_graph_<api>(` symbols from graph.h (open/upsert/neighbors/…),
# not typedef spellings like `hu_graph_entity_t` / `hu_graph_relation_t`.
# For docs/plans/2026-05-10-w7-phase1-bypass-inventory.md and trend tracking.
# Requires ripgrep (`rg`). Exit 0 (informational).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
if ! command -v rg >/dev/null 2>&1; then
  printf '%s\n' "w7-phase1-graph-bypass-inventory: rg not found; install ripgrep" >&2
  exit 0
fi
# Keep in sync with public graph entry points in include/human/memory/graph.h
GRAPH_API='hu_graph_(open|close|upsert_entity|find_entity|upsert_relation|upsert_relation_ex|upsert_relation_with_belief|relations_in_window|set_entity_community|neighbors|build_context|build_contact_context|build_communities|query_temporal|query_causal|list_entities|list_relations|list_relations_verifier_scan|set_relation_confidence|set_relation_belief|get_relation_belief|record_recall|reconsolidate|leiden_communities|add_temporal_event|add_causal_link)\('
printf '# hu_graph_<api>(...) call-site matches (per file, descending by count)\n\n'
set +e
rg "$GRAPH_API" src/agent src/persona src/feeds -g '*.c' -g '*.h' --count-matches 2>/dev/null \
    | sort -t: -k2 -nr \
    | awk -F: '{printf "%5d  %s\n", $2, $1; t+=$2} END {printf "\nTOTAL  %d\n", t+0}'
rg_exit=${PIPESTATUS[0]}
set -e
if [ "$rg_exit" -ne 0 ] && [ "$rg_exit" -ne 1 ]; then
  exit "$rg_exit"
fi
