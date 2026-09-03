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

# ── mlx-tune candidate-training stage (gated, OFF by default) ───────────────
#
# Trains a SimPO candidate via scripts/train-glm-adapter.sh's mlx-tune path
# (contract C6), guards it with adapter_is_real.py, and — if serving has not
# already come back up — scores it offline against the SERVING adapter with
# the LUAR authorship tier (contract C4). Never promotes: the candidate is
# staged only; promotion stays scripts/register_v6_adapter.py + a human call.
#
# Defined as its own function (rather than inlined below) so it can be
# sourced and invoked directly by a hermetic test without running the rest
# of this script (window check, launchctl bootout, real training). Sourcing
# with HU_RETRAIN_STAGE_TEST=1 defines this function (and log() above) then
# returns before anything else executes — see the guard right after this
# function definition.
run_mlxtune_candidate_stage() {
    if [[ "${HU_RETRAIN_MLXTUNE:-0}" != "1" ]]; then
        log "mlx-tune candidate stage disabled (HU_RETRAIN_MLXTUNE=0)"
        return 0
    fi

    log "mlx-tune candidate stage: starting (HU_RETRAIN_MLXTUNE=1)"

    # serving_stopped is the caller's global (set once mlx-server is actually
    # bootout'd, below). Refuse rather than risk a second loader if somehow
    # invoked while serving is still up.
    if [[ "${serving_stopped:-0}" != "1" ]]; then
        log "mlx-tune candidate stage: serving is not stopped — refusing (never two loaders)"
        return 0
    fi

    local max_min="${HU_RETRAIN_MLXTUNE_MAX_MIN:-90}"
    local stamp; stamp="$(date +%Y%m%d-%H%M)"
    local mlxtune_tag="mlxtune-simpo-${stamp}"
    local candidate_dir="$HOME/.human/training-data/adapters/seth-glm-air-${mlxtune_tag}"
    local mlxtune_train_log="$HOME/.human/logs/train-glm-${mlxtune_tag}.log"
    local data_dir="${HU_RETRAIN_MLXTUNE_DATA_DIR:-$HOME/.human/training-data/glm-v61-pref}"
    local config="${HU_RETRAIN_MLXTUNE_CONFIG:-$HOME/.human/training-data/glm-v61-orpo-config.yaml}"
    local mlxtune_py="$HOME/.human/venvs/mlxtune312/bin/python"
    local eval_py="$HOME/.human/venvs/eval312/bin/python"

    log "mlx-tune candidate stage: config=$config data=$data_dir out=$candidate_dir cap=${max_min}min"

    if [[ "${HU_RETRAIN_DRY_RUN:-0}" == "1" ]]; then
        log "mlx-tune candidate stage: DRY RUN (HU_RETRAIN_DRY_RUN=1) — loading nothing"
        if [[ -x "$mlxtune_py" ]]; then
            "$mlxtune_py" "$REPO/scripts/mlx_tune_train.py" --dry-run --config "$config" --train-mode simpo 2>&1 | tee -a "$LOG"
            log "mlx-tune candidate stage: dry-run train check exited rc=${PIPESTATUS[0]}"
        else
            log "mlx-tune candidate stage: mlxtune venv missing ($mlxtune_py) — skipping dry-run train check"
        fi
        if [[ -x "$eval_py" ]]; then
            "$eval_py" "$REPO/scripts/blind_ab/score_candidate_offline.py" --dry-run \
                --candidate "$candidate_dir" 2>&1 | tee -a "$LOG"
            log "mlx-tune candidate stage: dry-run score check exited rc=${PIPESTATUS[0]}"
        else
            log "mlx-tune candidate stage: eval venv missing ($eval_py) — skipping dry-run score check"
        fi
        log "mlx-tune candidate stage: DRY RUN done"
        return 0
    fi

    if [[ ! -s "$data_dir/train.jsonl" ]]; then
        log "mlx-tune candidate stage: no preference corpus at $data_dir/train.jsonl — skipping (not failing the window)"
        return 0
    fi
    local pair_count; pair_count=$(wc -l < "$data_dir/train.jsonl" | tr -d ' ')
    if [[ "$pair_count" -lt 200 ]]; then
        log "mlx-tune candidate stage: $pair_count pairs < floor 200 — skipping (see build_v6_preference_corpus.py --floor)"
        return 0
    fi
    log "mlx-tune candidate stage: $pair_count preference pairs at $data_dir — proceeding"

    log "mlx-tune candidate stage: training -> $candidate_dir (timeboxed ${max_min} min)"
    # No `timeout` binary on macOS: background the job, race a watchdog
    # subshell against it, and kill on overrun. restore_serving (the caller's
    # trap) still runs afterward regardless of how this exits.
    ( bash "$REPO/scripts/train-glm-adapter.sh" \
        --config "$config" --trainer mlx_tune --train-mode simpo \
        --tag "$mlxtune_tag" --est-minutes "$max_min" ) >>"$LOG" 2>&1 &
    local job_pid=$!
    ( sleep $(( max_min * 60 ))
      if kill -0 "$job_pid" 2>/dev/null; then
          log "mlx-tune candidate stage: exceeded ${max_min} min cap — terminating (pid $job_pid)"
          kill -TERM "$job_pid" 2>/dev/null
          sleep 5
          kill -KILL "$job_pid" 2>/dev/null
      fi
    ) &
    local watchdog_pid=$!
    wait "$job_pid"
    local train_rc=$?
    kill "$watchdog_pid" 2>/dev/null; wait "$watchdog_pid" 2>/dev/null || true
    log "mlx-tune candidate stage: train-glm-adapter.sh exited rc=$train_rc"

    if [[ "$train_rc" != "0" ]]; then
        log "mlx-tune candidate stage: training FAILED rc=$train_rc — see $mlxtune_train_log"
        return 0
    fi
    if [[ ! -d "$candidate_dir" ]]; then
        log "mlx-tune candidate stage: WARNING rc=0 but no adapter at $candidate_dir — treating as failure"
        return 0
    fi

    local why
    if why=$(python3 "$REPO/scripts/adapter_is_real.py" "$candidate_dir" 2>&1); then
        log "mlx-tune candidate stage: adapter real: $candidate_dir — $why"
    else
        log "mlx-tune candidate stage: adapter FAILED the real-adapter guard: $why"
        log "mlx-tune candidate stage: quarantining $candidate_dir -> $candidate_dir.rejected-$(date +%s)"
        mv "$candidate_dir" "$candidate_dir.rejected-$(date +%s)" 2>&1 | tee -a "$LOG" || true
        return 0
    fi

    # train-glm-adapter.sh restores production itself at the end of ITS OWN
    # run (it manages its own stop/restart around the training call). If
    # :8741 is already back up by the time we get here, a direct mlx_lm load
    # for offline scoring would be a second loader beside the resident base —
    # skip rather than risk it. This is a real, expected race with the
    # nested script, not a bug: refuse loudly instead of half-running.
    if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1; then
        log "mlx-tune candidate stage: :8741 already back up (train-glm-adapter.sh restored it) — skipping offline authorship scoring to avoid a second loader"
    elif [[ ! -x "$eval_py" ]]; then
        log "mlx-tune candidate stage: eval venv missing ($eval_py) — refusing to score, nothing written"
    else
        local score_out="$HOME/.human/logs/candidate-authorship-$(date +%Y-%m-%d).json"
        log "mlx-tune candidate stage: scoring candidate vs serving (offline LUAR) -> $score_out"
        "$eval_py" "$REPO/scripts/blind_ab/score_candidate_offline.py" \
            --candidate "$candidate_dir" --out "$score_out" 2>&1 | tee -a "$LOG"
        log "mlx-tune candidate stage: score_candidate_offline.py exited rc=${PIPESTATUS[0]}"
    fi

    log "mlx-tune candidate stage: candidate staged at $candidate_dir (NOT promoted)"
    log "mlx-tune candidate stage: to promote after human review: python3 $REPO/scripts/register_v6_adapter.py --adapter $candidate_dir --log $mlxtune_train_log"
}

# Testability hook: `HU_RETRAIN_STAGE_TEST=1 bash -c 'source scripts/nightly-retrain.sh; run_mlxtune_candidate_stage'`
# (or the equivalent from a test harness) defines log()/run_mlxtune_candidate_stage()
# above and stops here — the window check, mlx-server bootout, and real
# training below never execute. `return` works when sourced; `|| exit 0`
# covers the (unsupported, but harmless) case of executing this file
# directly with the flag set.
if [[ "${HU_RETRAIN_STAGE_TEST:-0}" == "1" ]]; then
    return 0 2>/dev/null || exit 0
fi

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

# MLX training must use the PINNED 3.12 venv, the same interpreter human-serve.sh
# picks for the server. Bare `python3` is /opt/homebrew/bin/python3 = 3.14, which
# the project has ruled out for MLX (loky/semaphore crash). mlx imports under
# 3.14, so this fails late and oddly rather than loudly — pin it.
TRAIN_PY="$HOME/Documents/gemma-realtime-1/.venv312/bin/python3.12"
[ -x "$TRAIN_PY" ] || TRAIN_PY="python3"

# ── Train ──────────────────────────────────────────────────────────────────
# Preflight still runs (it is the authority, not this script): with serving down
# the co-residency check passes and the memory check sees real headroom. If it
# refuses anyway, that refusal is correct and we restart serving untouched.
if [[ -f "$SOURCE_JSONL" ]]; then
    # --adapter-out is REQUIRED by training_loop.py's C3 fast path; without it the
    # script prints "ERROR: --adapter-out is required when --source-jsonl is set"
    # and exits 2 in under a second. Every scheduled run through 2026-09-03 hit
    # either that or the KeepAlive relaunch above; this window had never trained.
    # The adapter is STAGED only; promotion stays a separate, gated decision.
    ADAPTER_OUT="${HU_RETRAIN_ADAPTER_OUT:-$HOME/.human/training-data/adapters/seth-m3-outcomes-$(date +%Y%m%d-%H%M%S)}"
    log "training from $SOURCE_JSONL -> $ADAPTER_OUT (staged, not promoted)"
    "$TRAIN_PY" "$REPO/scripts/training_loop.py" \
        --source-jsonl "$SOURCE_JSONL" \
        --adapter-out "$ADAPTER_OUT" 2>&1 | tee -a "$LOG"
    train_rc=${PIPESTATUS[0]}
    log "training exited rc=${train_rc}"
    # Belt and braces beside training_loop's own guard: never let a placeholder
    # or a zero-weight adapter sit in the staging dir looking like a success.
    # training_loop may rename the dir with a base tag (…-glm); judge whichever
    # exists. A run that exits 0 without a real adapter is the bug this guards.
    staged="$ADAPTER_OUT"
    [[ -d "$staged" ]] || staged=$(ls -d "${ADAPTER_OUT}"-* 2>/dev/null | head -1)
    if [[ "$train_rc" != "0" ]]; then
        log "  training FAILED rc=$train_rc — see the lines above; NO adapter was produced tonight"
    elif [[ -z "$staged" || ! -d "$staged" ]]; then
        log "  WARNING: rc=0 but no adapter dir at $ADAPTER_OUT — treating as failure"
        train_rc=3
    elif why=$(python3 "$REPO/scripts/adapter_is_real.py" "$staged" 2>&1); then
        log "  adapter real: $staged — $why"
    else
        log "  adapter FAILED the real-adapter guard: $why"
        log "  quarantining $staged -> $staged.rejected-$(date +%s)"
        mv "$staged" "$staged.rejected-$(date +%s)" 2>&1 | tee -a "$LOG" || true
        train_rc=3
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

# mlx-tune candidate-training stage — gated OFF by default (HU_RETRAIN_MLXTUNE=1
# to enable). Runs AFTER training_loop above and BEFORE restore_serving (the
# EXIT trap below), i.e. still inside the dark window. See the function
# definition near the top of this file for the full contract.
run_mlxtune_candidate_stage

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
