#!/usr/bin/env bash
# nightly-retrain.sh — run LoRA retraining in a window where serving is stopped.
#
# WHY THIS EXISTS
#
# On 2026-07-26 this machine rebooted four times (04:01, 05:02, 06:45, 14:38).
# Cause: the daemon retrained GLM-4.5-Air-4bit (56 GB) while the mlx-server held
# the SAME base resident (56 GB) on a 128 GB box. Eleven runs fired that day, six
# inside 28 minutes, driving the machine to 154 MB free / 28 GB compressed /
# 13.2 GB swap. The trainer exiting recovered 53 GB instantly.
#
# training_loop.py now REFUSES to train while the production server serves the
# same base (training_preflight_decision). That stops the crashes but also means
# training can never run while serving is up — correct, and useless on its own.
# This script is the other half: inside a window, it stops serving, trains, and
# restarts serving, so the two never co-reside.
#
# The persona is offline for the training window. Cloud providers still answer
# via the model_fallback chain, so this degrades rather than goes dark.
#
# USAGE
#   bash scripts/nightly-retrain.sh              # honors HU_TRAIN_WINDOW
#   HU_TRAIN_WINDOW=02:00-05:00 bash scripts/nightly-retrain.sh
#   HU_RETRAIN_FORCE=1 bash scripts/nightly-retrain.sh   # ignore the window
#
# Install as a nightly job (03:07 daily — off the :00 mark on purpose):
#   see docs at the bottom of this file.

set -uo pipefail   # NOT -e: a training failure must still restart serving.

REPO="${HU_REPO_DIR:-$HOME/Projects/h-uman}"
LOG="$HOME/.human/logs/nightly-retrain.log"
SERVER_LABEL="gui/$(id -u)/ai.human.mlx-server"
WINDOW="${HU_TRAIN_WINDOW:-02:00-05:00}"
SOURCE_JSONL="${HU_RETRAIN_SOURCE:-$HOME/.human/training-data/m3-outcomes.jsonl}"
PORT="${HU_RETRAIN_PORT:-8741}"

mkdir -p "$(dirname "$LOG")"
log() { printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" | tee -a "$LOG"; }

# ── Window check ────────────────────────────────────────────────────────────
# Delegated to training_loop.py so the window semantics (inclusive start,
# exclusive end, midnight-crossing) have exactly ONE implementation, pinned by
# scripts/test_training_loop_preflight.py. A second parser here would drift.
if [[ "${HU_RETRAIN_FORCE:-0}" != "1" ]]; then
    if ! python3 -c "
import sys; sys.path.insert(0, '$REPO/scripts')
from datetime import datetime
import training_loop as t
w = t.parse_train_window('$WINDOW')
if w is None:
    sys.exit(0)   # unset/invalid window imposes no restriction
n = datetime.now()
ok, _ = t.training_preflight_decision(0, 1 << 60, n.hour * 60 + n.minute, w)
sys.exit(0 if ok else 1)
" 2>/dev/null; then
        log "outside training window $WINDOW — nothing to do"
        exit 0
    fi
fi

log "=== nightly retrain starting (window=$WINDOW) ==="

# ── Stop serving, and guarantee it comes back ───────────────────────────────
# The trap fires on normal exit AND on error/interrupt: leaving the persona's
# serving path down because training crashed would be a far worse failure than
# a skipped retrain.
serving_stopped=0
restore_serving() {
    if [[ "$serving_stopped" == "1" ]]; then
        log "restarting mlx-server"
        launchctl kickstart -k "$SERVER_LABEL" 2>&1 | tee -a "$LOG" || \
            log "WARNING: kickstart failed — run 'launchctl kickstart -k $SERVER_LABEL' by hand"
        for _ in $(seq 1 40); do
            code=$(curl -s --max-time 3 -o /dev/null -w '%{http_code}' \
                   "http://127.0.0.1:$PORT/health" 2>/dev/null || true)
            [[ "$code" == "200" ]] && { log "mlx-server healthy again"; return; }
            sleep 3
        done
        log "WARNING: mlx-server did not report healthy within ~120s"
    fi
}
trap restore_serving EXIT INT TERM

log "stopping mlx-server to free the base weights"
launchctl kill SIGTERM "$SERVER_LABEL" 2>&1 | tee -a "$LOG" || true
serving_stopped=1

# Wait for the 56 GB to actually come back — the process closing its socket does
# NOT mean the kernel has reclaimed its Metal/mmap pages. Same lag that motivated
# the barrier in human-serve.sh.
for _ in $(seq 1 60); do
    pgrep -f "mlx-server\.py .*--port ${PORT}" >/dev/null 2>&1 || break
    sleep 2
done
sleep 5
free_gb=$(vm_stat | awk '/Pages free/{gsub(/\./,"",$3); printf "%.0f", $3*16384/1073741824}')
log "serving stopped; ${free_gb} GB free"

# ── Train ──────────────────────────────────────────────────────────────────
# Preflight still runs (it is the authority, not this script): with serving down
# the co-residency check passes and the memory check sees real headroom. If it
# refuses anyway, that refusal is correct and we restart serving untouched.
if [[ -f "$SOURCE_JSONL" ]]; then
    log "training from $SOURCE_JSONL"
    python3 "$REPO/scripts/training_loop.py" --source-jsonl "$SOURCE_JSONL" 2>&1 | tee -a "$LOG"
    log "training exited rc=${PIPESTATUS[0]}"
else
    log "no source jsonl at $SOURCE_JSONL — skipping training"
fi

log "=== nightly retrain done ==="
# restore_serving runs via the EXIT trap.

# ── Installing as a launchd job ─────────────────────────────────────────────
#
# Write ~/Library/LaunchAgents/ai.human.nightly-retrain.plist with
# StartCalendarInterval Hour=3 Minute=7 (off the :00 mark so it does not collide
# with every other scheduled job), ProgramArguments = /bin/bash <this script>,
# then:  launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/ai.human.nightly-retrain.plist
#
# Re-enable the daemon-side trigger ONLY if you want opportunistic retraining on
# top of this: learning.m3_frontier_auto_training was set false on 2026-07-26 to
# stop the crash loop. The preflight now makes it safe (it refuses rather than
# thrashes), but with serving up it will simply never fire — which is why this
# scheduled window is the path that actually retrains.
