#!/usr/bin/env bash
# Phase G4 (2026-05-18) — autonomous M3 loop cycle.
#
# Designed to be invoked by launchd weekly (see ai.human.m3-loop.plist):
#   1. Poll outcomes from the daemon's /v1/m3/outcomes endpoint
#      (m3_outcome_driver.py).
#   2. Summarize REWRITE preference pairs and export Alpaca-DPO JSONL
#      (m3_dpo_from_rewrites.py).
#   3. If ≥ threshold pairs accumulated, run real DPO training via
#      `human ml dpo-train --pairs` (the C-side trainer, which can
#      target the served gemma model when --backend mlx is set).
#   4. Run A/B eval (m3_eval_adapter.py --judge metadata) against the
#      previous adapter.
#   5. If verdict == pass, promote (m3_promote.py).
#   6. Always run drift detector and log status.
#
# Idempotent + safe to run repeatedly. No-ops when nothing has
# accumulated. Soft-fails on every external dependency (MLX server,
# binary availability) so a missed weekly run doesn't snowball.
#
# All output to ~/.human/logs/m3-loop-<date>.log for forensic review.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HUMAN_HOME="$HOME/.human"
LOG_DIR="$HUMAN_HOME/logs"
LOG="$LOG_DIR/m3-loop-$(date +%Y-%m-%d).log"
mkdir -p "$LOG_DIR"

# Configurable thresholds via environment.
DPO_THRESHOLD="${M3_DPO_THRESHOLD:-32}"      # min REWRITE pairs to retrain
AUTO_PROMOTE="${M3_AUTO_PROMOTE:-0}"          # 1 = auto-promote on PASS verdict
MLX_URL="${HUMAN_MLX_URL:-http://127.0.0.1:8741}"

ts() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }
log() { echo "[$(ts)] $*" | tee -a "$LOG"; }

log "═══ m3-loop-cycle start ═══"
log "  threshold=$DPO_THRESHOLD auto_promote=$AUTO_PROMOTE mlx=$MLX_URL"

# Step 1: poll outcomes (drives JSONL append + adapter swap if --run-loop)
log "--- step 1: poll outcomes ---"
python3 "$REPO_ROOT/scripts/m3_outcome_driver.py" 2>&1 | tee -a "$LOG"

# Step 2: summarize REWRITE pairs + export
REWRITE_PAIRS="$HUMAN_HOME/training-data/m3-rewrite-pairs.jsonl"
ALPACA_OUT="$HUMAN_HOME/training-data/m3-alpaca-dpo-$(date +%Y%m%d).jsonl"
log "--- step 2: export DPO pairs ---"
python3 "$REPO_ROOT/scripts/m3_dpo_from_rewrites.py" \
    --input "$REWRITE_PAIRS" --export-jsonl "$ALPACA_OUT" 2>&1 | tee -a "$LOG"

# Count pairs (each line is one pair).
PAIRS_COUNT=$(wc -l < "$ALPACA_OUT" 2>/dev/null | tr -d ' ' || echo 0)
log "  accumulated $PAIRS_COUNT DPO pairs"

# Step 3: train if threshold met
if [ "$PAIRS_COUNT" -ge "$DPO_THRESHOLD" ]; then
    ADAPTER_OUT="$HUMAN_HOME/training-data/adapters/dpo-$(date +%Y%m%d-%H%M%S)"
    log "--- step 3: real DPO training (threshold $DPO_THRESHOLD met) ---"
    python3 "$REPO_ROOT/scripts/m3_dpo_from_rewrites.py" \
        --input "$REWRITE_PAIRS" \
        --export-jsonl "$ALPACA_OUT" \
        --train --adapter-out "$ADAPTER_OUT" \
        --iters 100 --beta 0.1 2>&1 | tee -a "$LOG"

    # Step 4: A/B eval against the prior adapter (find via lineage)
    PRIOR_ADAPTER=$(python3 -c "
import json
from pathlib import Path
p = Path.home() / '.human/training-data/adapter_lineage.jsonl'
if not p.exists():
    print('')
else:
    last_promote = None
    for line in p.read_text().splitlines():
        if not line.strip(): continue
        try:
            r = json.loads(line)
            if r.get('action') == 'promote' and r.get('ok'):
                last_promote = r
        except Exception: pass
    print(last_promote.get('to_adapter', '') if last_promote else '')
" 2>/dev/null || echo "")

    if [ -n "$PRIOR_ADAPTER" ] && [ -f "$ADAPTER_OUT/adapters.safetensors" ]; then
        log "--- step 4: A/B eval ---"
        python3 "$REPO_ROOT/scripts/m3_eval_adapter.py" \
            --baseline "$PRIOR_ADAPTER" \
            --candidate "$ADAPTER_OUT/adapters.safetensors" \
            --judge metadata \
            --json-out "$LOG_DIR/m3-verdict-$(date +%Y%m%d-%H%M%S).json" \
            2>&1 | tee -a "$LOG"
        VERDICT=$(jq -r '.verdict' "$LOG_DIR/m3-verdict-"*.json 2>/dev/null | tail -1)
        log "  verdict: $VERDICT"

        # Step 5: auto-promote (only if explicitly enabled)
        if [ "$AUTO_PROMOTE" = "1" ] && [ "$VERDICT" = "pass" ]; then
            log "--- step 5: auto-promote ---"
            python3 "$REPO_ROOT/scripts/m3_promote.py" promote \
                --adapter "$ADAPTER_OUT" --yes --no-prod-check 2>&1 | tee -a "$LOG"
        else
            log "  skipping promote (auto_promote=$AUTO_PROMOTE verdict=$VERDICT)"
            log "  to promote manually: scripts/m3_promote.py promote --adapter $ADAPTER_OUT --yes"
        fi
    else
        log "  skipping eval — no prior adapter or training did not produce one"
    fi
else
    log "  skipping train: $PAIRS_COUNT < $DPO_THRESHOLD pairs"
fi

# Step 6: drift detector (always runs)
log "--- step 6: drift detection ---"
python3 "$REPO_ROOT/scripts/m3_drift_detector.py" 2>&1 | tee -a "$LOG"

log "═══ m3-loop-cycle complete ═══"
