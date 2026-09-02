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

# Capture the serving base BEFORE stopping the server — resolution is
# ps-based, so it returns nothing once the process is gone.
SERVING_BASE="$(python3 -c "
import sys; sys.path.insert(0, '$REPO/scripts')
import training_loop as t
print(t.serving_base_from_ps() or '')
" 2>/dev/null || true)"
log "serving base: ${SERVING_BASE:-<none detected>}"

# ── Stop serving, and guarantee it comes back ───────────────────────────────
# The trap fires on normal exit AND on error/interrupt: leaving the persona's
# serving path down because training crashed would be a far worse failure than
# a skipped retrain.
serving_stopped=0
restore_serving() {
    if [[ "$serving_stopped" == "1" ]]; then
        log "restarting mlx-server"
        # Booted OUT below (not just killed), so bootstrap it back; kickstart is
        # the fallback for a plist that is somehow still loaded.
        launchctl bootstrap "gui/$(id -u)" "$HOME/Library/LaunchAgents/ai.human.mlx-server.plist" 2>&1 | tee -a "$LOG" || \
            launchctl kickstart -k "$SERVER_LABEL" 2>&1 | tee -a "$LOG" || \
            log "WARNING: bootstrap and kickstart failed — bootstrap ~/Library/LaunchAgents/ai.human.mlx-server.plist by hand"
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
# 2026-09-02: `launchctl kill SIGTERM` is NOT a stop — KeepAlive={Crashed,
# SuccessfulExit} relaunched the server within seconds, the preflight then
# refused (two loaders), and training_loop wrote an empty adapter and exited 0.
# bootout UNLOADS the job so nothing relaunches it until restore_serving.
launchctl bootout "gui/$(id -u)/$SERVER_LABEL" 2>&1 | tee -a "$LOG" || true
serving_stopped=1

# Wait for the 56 GB to actually come back — the process closing its socket does
# NOT mean the kernel has reclaimed its Metal/mmap pages. Same lag that motivated
# the barrier in human-serve.sh.
for _ in $(seq 1 60); do
    if ! pgrep -f "mlx-server\.py .*--port ${PORT}" >/dev/null 2>&1 && \
       ! lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1; then break; fi
    sleep 2
done
if pgrep -f "mlx-server\.py .*--port ${PORT}" >/dev/null 2>&1; then
    log "FATAL: mlx-server still alive after bootout — refusing to train beside it"
    exit 1
fi
sleep 5
free_gb=$(vm_stat | awk '/Pages free/{gsub(/\./,"",$3); printf "%.0f", $3*16384/1073741824}')
log "serving stopped; ${free_gb} GB free"

# ── Train ──────────────────────────────────────────────────────────────────
# Preflight still runs (it is the authority, not this script): with serving down
# the co-residency check passes and the memory check sees real headroom. If it
# refuses anyway, that refusal is correct and we restart serving untouched.
if [[ -f "$SOURCE_JSONL" ]]; then
    log "training from $SOURCE_JSONL"
    train_stamp=$(mktemp "${TMPDIR:-/tmp}/hu-retrain-stamp.XXXXXX")
    python3 "$REPO/scripts/training_loop.py" --source-jsonl "$SOURCE_JSONL" 2>&1 | tee -a "$LOG"
    train_rc=${PIPESTATUS[0]}
    log "training exited rc=${train_rc}"
    # Belt and braces beside training_loop's own guard: never let a placeholder
    # or a zero-weight adapter sit in the staging dir looking like a success.
    # training_loop stages to a FIXED path; only judge it if this run wrote it
    # (newer than the stamp), so a stale-but-real adapter is never quarantined.
    staged="$HOME/.human/training-data/adapters/seth-lora"
    if [[ "$train_rc" == "0" && -d "$staged" && "$staged" -nt "$train_stamp" ]]; then
        if why=$(python3 "$REPO/scripts/adapter_is_real.py" "$staged" 2>&1); then
            log "  adapter real: $staged — $why"
        else
            log "  adapter FAILED the real-adapter guard: $why"
            log "  quarantining $staged -> $staged.rejected-$(date +%s)"
            mv "$staged" "$staged.rejected-$(date +%s)" 2>&1 | tee -a "$LOG" || true
        fi
    fi
else
    log "no source jsonl at $SOURCE_JSONL — skipping training"
fi

# ── Steering vectors for the serving base ───────────────────────────────────
# Persona steering has been running UNSTEERED since the 2026-07-26 GLM flip:
#   [steering] probe FAIL: formality shape (60, 5376) != [46, hidden] for
#   'GLM-4.5-Air-4bit' — these vectors were extracted from a DIFFERENT base
# Commit 41fad87 keys vectors by base model so a flip can no longer silently
# reuse the wrong ones; what remained was actually EXTRACTING them for GLM.
# Extraction loads the full base, so like training it can only run while
# serving is down — this window is the only safe place for it. Runs after
# training so the two never hold the weights at once, and only when the
# vectors are genuinely missing.
if [[ -n "$SERVING_BASE" ]]; then
    vec_dir="$HOME/.human/persona_vectors/$(basename "$SERVING_BASE")"
    extractor="$HOME/Documents/gemma-realtime-1/scripts/extract_persona_vectors.py"
    if [[ -d "$vec_dir" ]] && compgen -G "$vec_dir/*.npy" >/dev/null 2>&1; then
        log "steering vectors already present for $SERVING_BASE — skipping extraction"
    elif [[ "${HU_RETRAIN_SKIP_VECTORS:-0}" == "1" ]]; then
        log "steering extraction skipped (HU_RETRAIN_SKIP_VECTORS=1)"
    elif [[ ! -f "$extractor" ]]; then
        log "steering extractor not found at $extractor — skipping"
    else
        log "extracting steering vectors for $SERVING_BASE -> $vec_dir"
        mkdir -p "$vec_dir"
        python3 "$extractor" --model "$SERVING_BASE" --out-dir "$vec_dir" 2>&1 | tee -a "$LOG"
        log "steering extraction exited rc=${PIPESTATUS[0]}"
    fi
fi

# Task 11 (2026-09-01): classifier-tier gate beside the human blind A/B. Runs
# HERE because the scorer loads its own base+adapter pair and the serving
# model is already stopped in this window (never two loaders). Best-effort:
# a refusal (n<20, no adapter) is logged, never fabricated.
# Default to the n=37 blind-A/B run: the rated cycle dirs carry only 12 keyed
# trials each and the gate refuses n<20.
CYCLE_DIR="${HU_CLASSIFIER_CYCLE_DIR:-$HOME/blind_ab_run}"
if [ -n "$CYCLE_DIR" ] && [ "$serving_stopped" = 1 ]; then
    log "classifier gate on $CYCLE_DIR"
    python3 "$REPO/scripts/blind_ab/classifier_gate.py" --cycle-dir "$CYCLE_DIR" --in-window 2>&1 | tee -a "$LOG"
    log "classifier gate exited rc=${PIPESTATUS[0]}"
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
