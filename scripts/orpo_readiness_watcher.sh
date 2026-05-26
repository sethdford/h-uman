#!/usr/bin/env bash
# Layer 1 / Month 2 — ORPO readiness watcher.
#
# Polls dpo_pairs nightly. When sources of REAL preference signal
# (imessage_tapback, slack_reactji, outbound_edit, user_feedback)
# accumulate to a threshold, fires the ORPO training pipeline.
#
# Designed to be invoked from launchd (~/Library/LaunchAgents/) on a
# daily schedule, OR from cron, OR ad-hoc by the operator.
#
# Per docs/plans/2026-05-18-adapter-routing-decision.md: target the
# imessage-tapback-v1 channel-scoped adapter; threshold ≥500 rows
# per Cui et al. 2025 (https://arxiv.org/abs/2507.04889) data floor
# for noticeable voice transfer.
#
# Usage:
#   scripts/orpo_readiness_watcher.sh                # check + report
#   scripts/orpo_readiness_watcher.sh --fire-if-ready  # also trigger ORPO
#   scripts/orpo_readiness_watcher.sh --threshold 250  # override
set -euo pipefail

DB=~/.human/memory.db
THRESHOLD=500
FIRE=false

while [ $# -gt 0 ]; do
    case "$1" in
        --fire-if-ready) FIRE=true; shift ;;
        --threshold)     THRESHOLD="$2"; shift 2 ;;
        --db)            DB="$2"; shift 2 ;;
        *)               echo "Usage: $0 [--fire-if-ready] [--threshold N] [--db PATH]"; exit 2 ;;
    esac
done

if [ ! -f "$DB" ]; then
    echo "[orpo-watcher] $DB not found"; exit 1
fi

# Count by source — focus on real-world preference signal
REAL_SIGNAL=$(sqlite3 "$DB" \
    "SELECT IFNULL(SUM(c), 0) FROM (
        SELECT COUNT(*) AS c FROM dpo_pairs
        WHERE source IN ('imessage_tapback','slack_reactji','outbound_edit','user_feedback')
    );")
TAPBACK=$(sqlite3 "$DB" "SELECT COUNT(*) FROM dpo_pairs WHERE source='imessage_tapback';")
ALL_PAIRS=$(sqlite3 "$DB" "SELECT COUNT(*) FROM dpo_pairs;")

echo "[orpo-watcher] dpo_pairs:"
echo "  total rows:                    $ALL_PAIRS"
echo "  real-world preference signal:  $REAL_SIGNAL  (threshold: $THRESHOLD)"
echo "  imessage_tapback specifically: $TAPBACK"
echo ""

# Per-source breakdown
sqlite3 -header -column "$DB" \
    "SELECT source, COUNT(*) AS rows FROM dpo_pairs GROUP BY source ORDER BY 2 DESC;"

if [ "$REAL_SIGNAL" -lt "$THRESHOLD" ]; then
    DEFICIT=$((THRESHOLD - REAL_SIGNAL))
    echo ""
    echo "[orpo-watcher] NOT READY — need $DEFICIT more real-signal rows."
    echo "  (passive wait — accumulates as you text)"
    exit 0
fi

echo ""
echo "[orpo-watcher] READY — $REAL_SIGNAL rows >= $THRESHOLD threshold."

if [ "$FIRE" != "true" ]; then
    echo "[orpo-watcher] --fire-if-ready not set; not triggering. Re-run with the flag."
    exit 0
fi

# Trigger the ORPO training pipeline. Uses the existing
# human ml dpo-train command per docs/plans/2026-05-11-rl-loop-phase-2-dpo-reactions.md.
# G16 (2026-05-26): try canonical dev path first, fall back to Documents
# (historic clone), then PATH lookup.
HU_BIN=~/Projects/h-uman/build/human
if [ ! -x "$HU_BIN" ]; then
    HU_BIN=~/Documents/h-uman/build/human
fi
if [ ! -x "$HU_BIN" ]; then
    HU_BIN=$(command -v human || true)
fi
if [ -z "$HU_BIN" ] || [ ! -x "$HU_BIN" ]; then
    echo "[orpo-watcher] ERROR: cannot find executable 'human' binary"
    exit 1
fi

OUT=~/.human/training-data/adapters/imessage-tapback-v1
mkdir -p "$OUT"

echo "[orpo-watcher] firing: $HU_BIN ml dpo-train --backend auto --output $OUT"
exec "$HU_BIN" ml dpo-train --backend auto --output "$OUT"
