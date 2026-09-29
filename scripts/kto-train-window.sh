#!/bin/bash
# kto-train-window.sh — maintenance-window KTO trainer.
#
# Closes the reaction→train loop: hu_lora_nightly_run exports single-sided
# KTO signals when there are no DPO pairs and writes <data>.pending; this
# job (launchd ai.human.kto-train-window, 04:40 daily) consumes the marker.
# Training and the live MLX server cannot co-run (31B co-train OOMs the box,
# verified 2026-06-06), so we stop the server for the window and ALWAYS
# restart it via trap. The produced adapter is STAGED ONLY — promotion stays
# behind the blind-A/B verdict gate (~/.human/blind_ab_gate.json).
set -uo pipefail

HUMAN_BIN="${HUMAN_BIN:-$HOME/.local/bin/human-daemon}"
PENDING="$HOME/.human/lora-pairs.jsonl.kto.jsonl.pending"
MLX_LABEL="ai.human.mlx-server"
MLX_PLIST="$HOME/Library/LaunchAgents/$MLX_LABEL.plist"
MLX_URL="http://127.0.0.1:8741/v1/models"
LOG="$HOME/.human/logs/kto-train-window.log"
TRAIN_TIMEOUT_SECS=2700   # 45 min hard cap on the window

mkdir -p "$(dirname "$LOG")"
log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG"; }

# Nothing to do is the common case, but it must be LOGGED: 20 silent days in
# July 2026 made "no work" indistinguishable from "dead job" (the caretaker's
# loop-liveness check flags a stale log — this heartbeat keeps it honest).
[ -f "$PENDING" ] || { log "no pending KTO marker — nothing to do"; exit 0; }

DATA=$(python3 -c "import json,sys;print(json.load(open('$PENDING'))['data'])" 2>/dev/null)
SIGNALS=$(python3 -c "import json,sys;print(json.load(open('$PENDING'))['signals'])" 2>/dev/null)

# Phase 1: If data file is missing, try to generate it.
# This closes the feedback→train loop: export single-sided signals to the path
# expected by the marker. If generation fails or is below threshold, skip training.
if [ -z "$DATA" ] || [ ! -f "$DATA" ]; then
    if [ -z "$DATA" ] || [ "$DATA" = "null" ]; then
        DATA="$HOME/.human/lora-pairs.jsonl.kto.jsonl"
    fi

    log "Data file missing ($DATA) — attempting to generate via kto_export..."
    # Repo path, NOT a worktree: the original ~/.human-worktrees/close-the-loops
    # path died with that worktree's cleanup (found dead 2026-07-25).
    python3 "$HOME/Projects/h-uman/scripts/kto_export.py" \
        --db "$HOME/.human/memory.db" \
        --output "$DATA" \
        --min-threshold 50 \
        >> "$LOG" 2>&1
    KTO_EXPORT_RC=$?

    if [ $KTO_EXPORT_RC -eq 2 ]; then
        log "KTO data below threshold (rc=2) — skipping training window"
        mv "$PENDING" "$PENDING.stale-$(date +%s)"
        exit 0
    elif [ $KTO_EXPORT_RC -ne 0 ]; then
        log "KTO export failed (rc=$KTO_EXPORT_RC) — clearing marker"
        mv "$PENDING" "$PENDING.stale-$(date +%s)"
        exit 0
    fi

    # Verify the file was created
    if [ ! -f "$DATA" ]; then
        log "KTO export succeeded but data file still missing ($DATA) — clearing marker"
        mv "$PENDING" "$PENDING.stale-$(date +%s)"
        exit 0
    fi
fi

OUT_DIR="$HOME/.human/training-data/adapters/kto-nightly-$(date +%Y%m%d-%H%M%S)"
log "window start: $SIGNALS signals from $DATA -> $OUT_DIR"

restart_server() {
    launchctl bootstrap "gui/$(id -u)" "$MLX_PLIST" 2>>"$LOG"
    for _ in $(seq 1 30); do
        sleep 5
        if curl -s -o /dev/null -w '%{http_code}' "$MLX_URL" | grep -q 200; then
            log "mlx server back up"
            return 0
        fi
    done
    log "WARNING: mlx server did not come back within 150s — check manually"
    return 1
}
trap restart_server EXIT

launchctl bootout "gui/$(id -u)/$MLX_LABEL" 2>>"$LOG" || true
sleep 5

# perl alarm = portable timeout (macOS has no coreutils timeout by default)
perl -e 'alarm shift; exec @ARGV' "$TRAIN_TIMEOUT_SECS" \
    "$HUMAN_BIN" ml kto-train --pairs "$DATA" --backend mlx --adapter-out "$OUT_DIR" \
    >> "$LOG" 2>&1
TRAIN_RC=$?

if [ $TRAIN_RC -eq 0 ] && ls "$OUT_DIR"/*.safetensors >/dev/null 2>&1; then
    log "train OK — adapter staged at $OUT_DIR (promotion gated on blind-A/B verdict)"
    mv "$PENDING" "$PENDING.done-$(date +%s)"
else
    log "train FAILED (rc=$TRAIN_RC) — marker kept for retry tomorrow"
fi
# trap restarts the server on exit
