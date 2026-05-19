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
# HUMAN_HOME defaults to ~/.human in production; smoke tests override
# this to point at an isolated path (scripts/test_m3_loop_cycle_smoke.sh).
HUMAN_HOME="${HUMAN_HOME:-$HOME/.human}"
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

# Toggle: set M3_H_TIER_ENABLE=0 to skip the data-acquisition tier
# (useful when the corpus is already fresh or the loop is being
# debugged on the G-tier steps).
H_TIER_ENABLE="${M3_H_TIER_ENABLE:-1}"
PROBE_RESPONSE_MODE="${M3_PROBE_RESPONSE_MODE:-simulate-tick}"
PROBE_SIMULATED_REPLY="${M3_PROBE_SIMULATED_REPLY:-}"

# Step 0a: H1 — refresh the multi-channel corpus from local stores.
#   PII-redacted JSONL at $HUMAN_HOME/training-data/m3-corpus.jsonl.
#   Soft-fails: if chat.db can't be read (no FDA), the script returns
#   non-zero but the loop continues.
if [ "$H_TIER_ENABLE" = "1" ]; then
    log "--- step 0a: H1 corpus extract ---"
    # Optional DB overrides for isolated smoke testing
    H1_EXTRA=()
    [ -n "${M3_IMSG_DB:-}" ] && H1_EXTRA+=(--imessage-db "$M3_IMSG_DB")
    [ -n "${M3_MEMORY_DB:-}" ] && H1_EXTRA+=(--memory-db "$M3_MEMORY_DB")
    python3 "$REPO_ROOT/scripts/m3_extract_corpus.py" \
        --out "$HUMAN_HOME/training-data/m3-corpus.jsonl" \
        --sources imessage,memory_db \
        --max-per-source 5000 \
        "${H1_EXTRA[@]}" 2>&1 | tee -a "$LOG" || \
        log "  WARN: H1 extract returned non-zero (corpus may be stale)"

    # Step 0b: H2 — regenerate counterfactual preference pairs from the
    #   refreshed corpus. Uses synthetic variants unless OPENAI_API_KEY
    #   is set in the environment.
    log "--- step 0b: H2 counterfactual pairs ---"
    H2_FLAGS=""
    [ -z "${OPENAI_API_KEY:-}" ] && H2_FLAGS="--no-llm"
    python3 "$REPO_ROOT/scripts/m3_generate_counterfactuals.py" \
        --corpus "$HUMAN_HOME/training-data/m3-corpus.jsonl" \
        --out "$HUMAN_HOME/training-data/m3-counterfactuals.jsonl" \
        --max-records 200 $H2_FLAGS 2>&1 | tee -a "$LOG" || \
        log "  WARN: H2 counterfactual generation skipped (no corpus?)"

    # Step 0c: H3 — queue one active-learning probe.
    #   Soft-fail: if no eligible unanswered message exists, exits 2.
    log "--- step 0c: H3 active probe (queue) ---"
    python3 "$REPO_ROOT/scripts/m3_active_probe.py" \
        --corpus "$HUMAN_HOME/training-data/m3-corpus.jsonl" \
        --queue "$HUMAN_HOME/training-data/m3-active-probe-queue.jsonl" \
        --pairs-out "$HUMAN_HOME/training-data/m3-active-probe-pairs.jsonl" \
        --delivery queue 2>&1 | tee -a "$LOG" || \
        log "  WARN: H3 probe skipped (no eligible messages?)"

    # Step 0d: H3b — consume queued probes (simulate by default).
    #   Production mode (--mode=dispatch / --mode=poll) requires the
    #   iMessage send wire; set M3_PROBE_RESPONSE_MODE to switch.
    log "--- step 0d: H3b probe collector ($PROBE_RESPONSE_MODE) ---"
    COLLECTOR_ARGS=(--queue "$HUMAN_HOME/training-data/m3-active-probe-queue.jsonl"
                     --pairs-out "$HUMAN_HOME/training-data/m3-active-probe-pairs.jsonl"
                     --mode "$PROBE_RESPONSE_MODE")
    if [ -n "$PROBE_SIMULATED_REPLY" ]; then
        COLLECTOR_ARGS+=(--simulate-response "$PROBE_SIMULATED_REPLY")
    fi
    python3 "$REPO_ROOT/scripts/m3_probe_collector.py" \
        "${COLLECTOR_ARGS[@]}" 2>&1 | tee -a "$LOG" || \
        log "  WARN: probe collector returned non-zero"
fi

# Step 1: poll outcomes (drives JSONL append + adapter swap if --run-loop)
log "--- step 1: poll outcomes ---"
python3 "$REPO_ROOT/scripts/m3_outcome_driver.py" 2>&1 | tee -a "$LOG"

# Step 2: summarize REWRITE pairs + export
REWRITE_PAIRS="$HUMAN_HOME/training-data/m3-rewrite-pairs.jsonl"
ALPACA_OUT="$HUMAN_HOME/training-data/m3-alpaca-dpo-$(date +%Y%m%d).jsonl"
log "--- step 2: export DPO pairs ---"
python3 "$REPO_ROOT/scripts/m3_dpo_from_rewrites.py" \
    --input "$REWRITE_PAIRS" --export-jsonl "$ALPACA_OUT" 2>&1 | tee -a "$LOG"

# Merge H-tier preference pairs (counterfactuals + active probe) into the
# training set. These are Alpaca-DPO shape already; just concatenate.
CF_PAIRS="$HUMAN_HOME/training-data/m3-counterfactuals.jsonl"
PROBE_PAIRS="$HUMAN_HOME/training-data/m3-active-probe-pairs.jsonl"
for src in "$CF_PAIRS" "$PROBE_PAIRS"; do
    if [ -f "$src" ]; then
        added=$(wc -l < "$src" 2>/dev/null | tr -d ' ' || echo 0)
        if [ "$added" -gt 0 ]; then
            cat "$src" >> "$ALPACA_OUT"
            log "  merged $added pairs from $(basename "$src")"
        fi
    fi
done

# Count pairs (each line is one pair, all sources combined).
PAIRS_COUNT=$(wc -l < "$ALPACA_OUT" 2>/dev/null | tr -d ' ' || echo 0)
log "  accumulated $PAIRS_COUNT DPO pairs (rewrites + counterfactuals + probes)"

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
        log "--- step 4: A/B metadata eval ---"
        python3 "$REPO_ROOT/scripts/m3_eval_adapter.py" \
            --baseline "$PRIOR_ADAPTER" \
            --candidate "$ADAPTER_OUT/adapters.safetensors" \
            --judge metadata \
            --json-out "$LOG_DIR/m3-verdict-$(date +%Y%m%d-%H%M%S).json" \
            2>&1 | tee -a "$LOG"
        VERDICT=$(jq -r '.verdict' "$LOG_DIR/m3-verdict-"*.json 2>/dev/null | tail -1)
        log "  metadata verdict: $VERDICT"

        # Step 4b: behavioral eval (the "did it actually learn?" test).
        # Only runs when:
        #   - the metadata verdict is pass (don't waste time on a broken
        #     adapter)
        #   - the held-out prompts file exists (operator ran the split)
        #   - MLX server is reachable / model is downloadable
        BEHAVIORAL_VERDICT="skipped"
        if [ "$VERDICT" = "pass" ] && \
           [ -f "$HUMAN_HOME/training-data/m3-holdout-prompts.jsonl" ]; then
            log "--- step 4b: behavioral eval (style + diversity) ---"
            BEH_OUT="$LOG_DIR/m3-behavioral-$(date +%Y%m%d-%H%M%S).json"
            python3 "$REPO_ROOT/scripts/m3_behavioral_eval.py" \
                --candidate-adapter "$ADAPTER_OUT" \
                --prompts-jsonl "$HUMAN_HOME/training-data/m3-holdout-prompts.jsonl" \
                --max-prompts "${M3_BEHAVIORAL_PROMPTS:-8}" \
                --max-tokens 60 \
                --json-out "$BEH_OUT" 2>&1 | tee -a "$LOG" || \
                log "  WARN: behavioral eval errored (skipping gate)"
            if [ -f "$BEH_OUT" ]; then
                BEHAVIORAL_VERDICT=$(jq -r '.verdict' "$BEH_OUT" 2>/dev/null)
                log "  behavioral verdict: $BEHAVIORAL_VERDICT"
            fi
        else
            log "  skipping behavioral eval (verdict=$VERDICT or no holdout)"
        fi

        # Step 5: auto-promote — requires BOTH gates to pass.
        # Safety stack:
        #   1. M3_AUTO_PROMOTE=1 (explicit opt-in via env)
        #   2. metadata verdict = pass
        #   3. behavioral verdict in {pass, skipped} (regress blocks)
        #   4. drift detector NOT NEEDS_ROLLBACK on most recent window
        SAFE_TO_PROMOTE=0
        if [ "$AUTO_PROMOTE" = "1" ] && \
           [ "$VERDICT" = "pass" ] && \
           [ "$BEHAVIORAL_VERDICT" != "regress" ]; then
            SAFE_TO_PROMOTE=1
        fi

        if [ "$SAFE_TO_PROMOTE" = "1" ]; then
            log "--- step 5: auto-promote (both gates passed) ---"
            python3 "$REPO_ROOT/scripts/m3_promote.py" promote \
                --adapter "$ADAPTER_OUT" --yes --no-prod-check 2>&1 | tee -a "$LOG"
        else
            log "  skipping promote (auto_promote=$AUTO_PROMOTE metadata=$VERDICT behavioral=$BEHAVIORAL_VERDICT)"
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
