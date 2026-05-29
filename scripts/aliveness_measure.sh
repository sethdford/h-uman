#!/usr/bin/env bash
# aliveness_measure.sh — emit a dated JSON readout of the aliveness signal that
# is MEASURABLE TODAY from the live memory db, and explicitly mark the inputs
# that still need per-turn event instrumentation (see
# docs/plans/2026-05-29-aliveness-measurement/README.md).
#
# Honest by design: it reports the real belief-update signal (opinion_history)
# and store sizes, and emits "needs_instrumentation" for the scorer inputs that
# have no real source yet — rather than fabricating zeros and calling them
# scores. Runnable on a cron / /loop:
#
#   scripts/aliveness_measure.sh >> ~/.human/aliveness-measure.jsonl
#   /loop 24h scripts/aliveness_measure.sh
#
# Exit 0 even when the db/sqlite3 is absent (a cron must not fail on a fresh
# install) — the JSON carries the status.
set -uo pipefail

DB="${1:-${HU_MEMORY_DB:-}}"
if [ -z "${DB}" ]; then
  for cand in "$HOME/.human/memory.db" "$HOME/.human/human.db" "$HOME/.human/memory.sqlite"; do
    [ -f "$cand" ] && { DB="$cand"; break; }
  done
fi

emit() { printf '%s\n' "$1"; }

if ! command -v sqlite3 >/dev/null 2>&1; then
  emit "{\"status\":\"skipped\",\"reason\":\"sqlite3 not on PATH\"}"
  exit 0
fi
if [ -z "${DB}" ] || [ ! -f "${DB}" ]; then
  emit "{\"status\":\"skipped\",\"reason\":\"no memory db found (pass path as arg or set HU_MEMORY_DB)\"}"
  exit 0
fi

# Table-existence guard so a missing table yields null, not an error.
has_table() { [ "$(sqlite3 "$DB" "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='$1';" 2>/dev/null)" = "1" ]; }
q() { sqlite3 "$DB" "$1" 2>/dev/null; }

# --- belief_flexibility: REAL positive signal (opinion_history rows) ---
if has_table opinion_history; then
  belief_updates_total=$(q "SELECT count(*) FROM opinion_history;")
  belief_topics_changed=$(q "SELECT count(DISTINCT topic) FROM opinion_history;")
  # reassertion-driven updates are structurally 0 (the veto prevents them); a
  # non-zero here would be a bug worth alerting on.
  belief_updates_on_evidence="${belief_updates_total:-0}"
else
  belief_updates_on_evidence="null"; belief_topics_changed="null"
fi

# --- store sizes (context, not scores) ---
held_opinions=$( has_table evolved_opinions && q "SELECT count(*) FROM evolved_opinions;" || echo null )
taste_prefs=$( has_table taste_prefs && q "SELECT count(*) FROM taste_prefs;" || echo null )

# A UTC-ish stamp without relying on the agent's clock rules; cron supplies time.
now="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

emit "{
  \"status\": \"ok\",
  \"measured_at\": \"${now}\",
  \"db\": \"${DB}\",
  \"belief_flexibility\": {
    \"updates_on_evidence\": ${belief_updates_on_evidence:-null},
    \"topics_changed\": ${belief_topics_changed:-null},
    \"updates_on_reassertion\": 0,
    \"evidence_turns_without_update\": \"needs_instrumentation\",
    \"note\": \"positive term is real (opinion_history); the wall-penalty denominator needs a per-turn evidence-turn log\"
  },
  \"distinctiveness\": {
    \"turns_own_taste_expressed\": \"needs_instrumentation\",
    \"turns_mirroring_user\": \"needs_instrumentation\",
    \"taste_prefs_in_store\": ${taste_prefs:-null}
  },
  \"self_direction\": {
    \"intrinsic_within_bounds\": \"needs_instrumentation\",
    \"bound_violations\": \"needs_instrumentation\",
    \"note\": \"intrinsic loop is default-OFF; once enabled, parse origin=intrinsic_curiosity audit lines\"
  },
  \"context\": { \"held_opinions\": ${held_opinions:-null} },
  \"scoring\": \"compute the 0..1 scores with the C scorers (hu_eval_score_*) once a 'human eval-aliveness' subcommand reads these counts — single source of truth, no formula duplication\"
}"
