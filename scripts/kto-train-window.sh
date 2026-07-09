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

[ -f "$PENDING" ] || exit 0   # nothing to do — the common case

DATA=$(python3 -c "import json,sys;print(json.load(open('$PENDING'))['data'])" 2>/dev/null)
SIGNALS=$(python3 -c "import json,sys;print(json.load(open('$PENDING'))['signals'])" 2>/dev/null)
if [ -z "$DATA" ] || [ ! -f "$DATA" ]; then
    log "pending marker present but data file missing ($DATA) — clearing marker"
    mv "$PENDING" "$PENDING.stale-$(date +%s)"
    exit 0
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
