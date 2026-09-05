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
#   HU_RETRAIN_FORCE_TRAIN=1 bash scripts/nightly-retrain.sh
#                       # retrain even when the source corpus is unchanged
#                       # (see the source-unchanged skip below). Distinct from
#                       # HU_RETRAIN_FORCE, which only overrides the window.
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
ADAPTERS_DIR="${HU_RETRAIN_ADAPTERS_DIR:-$HOME/.human/training-data/adapters}"
SKIP_BASE_TRAINING=0

mkdir -p "$(dirname "$LOG")"
log() { printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" | tee -a "$LOG"; }

# sha256 of a file, bare digest on stdout. Non-zero (and silent) when the file
# is missing or no digest tool exists, so every caller must treat "" as
# "unknown" rather than as a value — an empty digest must never compare equal
# to a stamp and skip a real retrain.
hu_sha256() {
    [[ -f "$1" ]] || return 1
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" 2>/dev/null | awk 'NR==1{print $1}'
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" 2>/dev/null | awk 'NR==1{print $1}'
    else
        return 1
    fi
}

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
    # train-glm-adapter.sh appends its OWN "-<TAG>-<its own STAMP>" naming
    # (ADAPTER=.../seth-glm-air-${TAG}-${STAMP}), so the real output directory
    # is this prefix PLUS a suffix we do not know in advance. Resolve the
    # actual directory via glob AFTER training completes (below) — do not
    # assume this prefix IS the directory.
    local candidate_dir_prefix="$HOME/.human/training-data/adapters/seth-glm-air-${mlxtune_tag}"
    local data_dir="${HU_RETRAIN_MLXTUNE_DATA_DIR:-$HOME/.human/training-data/glm-v61-pref}"
    local config="${HU_RETRAIN_MLXTUNE_CONFIG:-$HOME/.human/training-data/glm-v61-orpo-config.yaml}"
    local mlxtune_py="$HOME/.human/venvs/mlxtune312/bin/python"
    local eval_py="$HOME/.human/venvs/eval312/bin/python"

    log "mlx-tune candidate stage: config=$config data=$data_dir out=${candidate_dir_prefix}-* cap=${max_min}min"

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
                --candidate "${candidate_dir_prefix}-placeholder" 2>&1 | tee -a "$LOG"
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

    log "mlx-tune candidate stage: training -> ${candidate_dir_prefix}-* (timeboxed ${max_min} min)"
    # HU_TRAIN_SERVING_MANAGED_BY_CALLER=1: WE already stopped :8741 above (or
    # this function would have refused at the serving_stopped check). Tell
    # train-glm-adapter.sh not to bootout again and NOT to restore when it
    # finishes — restoring is OUR job, exactly once, via the EXIT trap below,
    # AFTER the offline scoring step further down. This is the fix for the
    # bug where every real run silently skipped scoring: train-glm-adapter.sh
    # used to restore prod itself before returning, so by the time we reached
    # the scoring step :8741 was already back up and we refused to load a
    # second model.
    #
    # No `timeout` binary on macOS: background the job, race a watchdog
    # subshell against it, and kill on overrun. restore_serving (the caller's
    # trap) still runs afterward regardless of how this exits — including if
    # the watchdog SIGKILLs train-glm-adapter.sh mid-run: a killed process
    # cannot run its OWN traps either way, so serving stays down until OUR
    # trap restores it, same as any other failure path here.
    # HU_TRAIN_REBALANCE_CASING=1: pull the corpus's chosen-side
    # lowercase-start/terminal-punct habit back toward Seth's measured style
    # card before training (scripts/rebalance_preference_corpus.py) — see the
    # 2026-09-04 finding that an 86% lowercase-start production habit traced
    # back to a corpus that was 77.5% lowercase by construction, an axis LUAR
    # never measures. The nightly candidate path always wants this; a
    # standalone `bash scripts/train-glm-adapter.sh` invocation does not
    # unless the caller opts in (default 0 — see that script).
    ( HU_TRAIN_SERVING_MANAGED_BY_CALLER=1 HU_TRAIN_REBALANCE_CASING=1 bash "$REPO/scripts/train-glm-adapter.sh" \
        --config "$config" --trainer mlx_tune --train-mode simpo \
        --tag "$mlxtune_tag" --est-minutes "$max_min" ) >>"$LOG" 2>&1 &
    local job_pid=$!
    # Explicit >/dev/null redirect is load-bearing, not cosmetic: without it
    # this subshell inherits whatever fd stdout/stderr currently are. When the
    # CALLER captured this function's output via command substitution
    # ($(...)) -- as the test harness does -- that fd is a pipe, and bash's
    # $(...) blocks until EVERY process holding the pipe's write end exits.
    # `kill "$watchdog_pid"` below only reaps the SUBSHELL; if it were still
    # asleep, its `sleep N` CHILD would be orphaned holding that pipe open for
    # the remaining duration (up to max_min minutes) even though the function
    # has already returned -- a caller capturing output via $(...) would hang
    # for the full timebox on every single invocation. log() still reaches
    # $LOG unaffected -- it opens $LOG explicitly via `tee -a`, independent of
    # this subshell's own stdout target.
    ( sleep $(( max_min * 60 ))
      if kill -0 "$job_pid" 2>/dev/null; then
          log "mlx-tune candidate stage: exceeded ${max_min} min cap — terminating (pid $job_pid)"
          kill -TERM "$job_pid" 2>/dev/null
          sleep 5
          kill -KILL "$job_pid" 2>/dev/null
      fi
    ) >/dev/null 2>&1 &
    local watchdog_pid=$!
    wait "$job_pid"
    local train_rc=$?
    kill "$watchdog_pid" 2>/dev/null; wait "$watchdog_pid" 2>/dev/null || true
    log "mlx-tune candidate stage: train-glm-adapter.sh exited rc=$train_rc"

    # Resolve the REAL output directory now that training has run — see the
    # comment on candidate_dir_prefix above. Newest match wins if more than
    # one somehow exists (there should be exactly zero or one).
    local candidate_dir
    candidate_dir=$(ls -d "${candidate_dir_prefix}-"* 2>/dev/null | sort | tail -1)

    if [[ "$train_rc" != "0" ]]; then
        log "mlx-tune candidate stage: training FAILED rc=$train_rc — see $LOG"
        return 0
    fi
    if [[ -z "$candidate_dir" || ! -d "$candidate_dir" ]]; then
        log "mlx-tune candidate stage: WARNING rc=0 but no adapter dir matching ${candidate_dir_prefix}-* — treating as failure"
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

    # Same TAG-STAMP suffix train-glm-adapter.sh used for both the adapter dir
    # and its own log file — derive the log path from the adapter dir's
    # basename rather than guessing it (see the STAMP-mismatch note above).
    local mlxtune_train_log="$HOME/.human/logs/train-glm-${candidate_dir##*/seth-glm-air-}.log"

    # :8741 should still be DOWN here — train-glm-adapter.sh was told
    # (HU_TRAIN_SERVING_MANAGED_BY_CALLER=1) not to restore it, so restoring
    # is exclusively OUR job via the EXIT trap, after this scoring step. The
    # check below is now a defensive fallback, not the routine case it used
    # to be: if something ELSE brought serving back up (a stray launchd
    # restart, another session), refuse the load rather than risk a second
    # 56 GB model resident — never fabricate a score off a load we shouldn't
    # have made.
    if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1; then
        log "mlx-tune candidate stage: :$PORT is unexpectedly back up already (not by train-glm-adapter.sh, which was told to leave it stopped) — skipping offline authorship scoring to avoid a second loader"
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

# ── Stop serving (a function so scripts/test_nightly_retrain_stop_serving.sh
#    can drive it hermetically with fake launchctl/pgrep/lsof/sleep on PATH) ──
#
# Sets serving_stopped=1 once the job is booted out (so the EXIT trap in the
# main flow puts it back) and returns 0 when :PORT is free. Returns 1 — refuse
# to train — when the server is still alive after the wait; serving_stopped
# then reflects only whether WE booted it out, so a server this script never
# stopped is left alone rather than kickstarted by the trap.
#
# 2026-09-02: `launchctl kill SIGTERM` is NOT a stop — KeepAlive={Crashed,
# SuccessfulExit} relaunched the server within seconds, the preflight then
# refused (two loaders), and training_loop wrote an empty adapter and exited 0.
# bootout UNLOADS the job so nothing relaunches it until restore_serving.
#
# 2026-09-04: the bootout passed "gui/$(id -u)/$SERVER_LABEL" — the domain
# prefixed TWICE, since SERVER_LABEL already carries it — so launchctl answered
# "No such process", the server never stopped, and every nightly run since
# aa2a1a79b was refused by the still-alive check. The test pins the label.
stop_serving() {
    log "stopping mlx-server to free the base weights"
    local out rc
    out=$(launchctl bootout "$SERVER_LABEL" 2>&1); rc=$?
    [[ -n "$out" ]] && log "$out"
    if [[ "$rc" -eq 0 ]]; then
        serving_stopped=1
    else
        log "WARNING: launchctl bootout $SERVER_LABEL failed (rc=$rc) — job not loaded?"
    fi

    # Wait for the 56 GB to actually come back — the process closing its socket
    # does NOT mean the kernel has reclaimed its Metal/mmap pages. Same lag that
    # motivated the barrier in human-serve.sh. 3f12aca97: a 54 GB server can
    # take well over 120 s to exit, so wait up to HU_RETRAIN_STOP_WAIT_SECS
    # (default 900) polling every 5 s, and log pid/stat/rss once a minute so a
    # truly stuck server is distinguishable from a slow exit.
    local stop_wait_secs="${HU_RETRAIN_STOP_WAIT_SECS:-900}" waited=0
    while :; do
        if ! pgrep -f "mlx-server\.py .*--port ${PORT}" >/dev/null 2>&1 && \
           ! lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1; then break; fi
        if (( waited >= stop_wait_secs )); then break; fi
        if (( waited % 60 == 0 )); then
            log "  waiting for mlx-server to exit (${waited}s): $(ps -o pid=,stat=,rss= -p "$(pgrep -f "mlx-server\.py .*--port ${PORT}" | head -1)" 2>/dev/null | tr -s ' ')"
        fi
        sleep 5; waited=$((waited + 5))
    done
    if pgrep -f "mlx-server\.py .*--port ${PORT}" >/dev/null 2>&1; then
        log "FATAL: mlx-server still alive after bootout — refusing to train beside it"
        return 1
    fi
    # Belt and braces (PR #391): the shared guard answers "is it safe to load a
    # model in-process right now?" — no server answering :8741/health, no trainer
    # process, wired memory under 70 GB. Refuse rather than become the second loader.
    if [[ -f "$REPO/scripts/check-no-resident-model.sh" ]]; then
        bash "$REPO/scripts/check-no-resident-model.sh" >>"$LOG" 2>&1
        local guard_rc=$?
        if [[ "$guard_rc" -ne 0 ]]; then
            log "FATAL: check-no-resident-model.sh refused (rc=$guard_rc: 1=server answering, 2=trainer running, 3=wired over limit) — not training"
            return 1
        fi
        log "check-no-resident-model.sh: clear to train"
    fi
    # Nothing is serving and we are about to train: whatever stopped it, the
    # trap must bring serving back afterwards.
    serving_stopped=1
    sleep 5
    local free_gb
    free_gb=$(vm_stat | awk '/Pages free/{gsub(/\./,"",$3); printf "%.0f", $3*16384/1073741824}')
    log "serving stopped; ${free_gb} GB free"
    return 0
}

# Testability hook: `HU_RETRAIN_STAGE_TEST=1 bash -c 'source scripts/nightly-retrain.sh; run_mlxtune_candidate_stage'`
# (or the equivalent from a test harness) defines log()/run_mlxtune_candidate_stage()/stop_serving()
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

# ── Refresh the corpus BEFORE digesting it ─────────────────────────────────
# The export stage loads no model (it reads the daemon's ring, else
# production_outcomes) so it is safe here, beside the still-resident server
# and above stop_serving. Until 2026-09-05 its only trigger was the weekly
# Sunday ai.human.m3-loop job, which last fired 2026-08-02 (it is skipped
# whenever the Mac is asleep at Sun 04:00) — so this nightly retrained a
# frozen 313-row file. Idempotent: a repeat append is a no-op.
if [[ "${HU_RETRAIN_SKIP_EXPORT:-0}" != "1" && -f "$REPO/scripts/m3_outcome_driver.py" ]]; then
    log "refreshing $SOURCE_JSONL (m3_outcome_driver --export-only)"
    python3 "$REPO/scripts/m3_outcome_driver.py" --export-only >>"$LOG" 2>&1 \
        || log "WARNING: outcome export returned non-zero — training from the corpus as-is"
fi

# ── Source-unchanged skip — MUST stay above every serving-stopping step ─────
#
# Until 2026-09-05 this window retrained unconditionally from $SOURCE_JSONL,
# a file untouched since 2026-08-02, and produced two 556 MB adapters from the
# identical corpus (seth-m3-outcomes-20260904-212919-glm and
# -20260905-030710-glm). Each run cost ~5 minutes of persona downtime to learn
# nothing. Every accepted adapter now records the digest it trained from in
# <adapter_dir>/source.sha256 (written in the "adapter real:" branch below);
# if the newest non-rejected stamped adapter carries the digest the source has
# right now, we exit 0 here — before the launchctl bootout, before
# check-no-resident-model.sh, before the steering and classifier stages — so
# mlx-server is never touched.
#
# The `log` above already wrote today's dated line, which is the artifact
# nightly-watchdog.sh reads as "retrain ran", so a skip does not re-trigger it.
#
# Bypass with HU_RETRAIN_FORCE_TRAIN=1 (retrain the same corpus anyway).
# HU_RETRAIN_FORCE=1 is the *window* override and deliberately does not imply
# this one. `.rejected-` dirs are excluded exactly as check-learning-loops.sh
# excludes them: mtime survives the quarantine rename, so an unfiltered `ls -t`
# would let a rejected no-op adapter authorize skipping a real retrain.
SOURCE_SHA="$(hu_sha256 "$SOURCE_JSONL" 2>/dev/null || true)"
if [[ "${HU_RETRAIN_FORCE_TRAIN:-0}" == "1" ]]; then
    log "HU_RETRAIN_FORCE_TRAIN=1 — training even if the source is unchanged"
elif [[ -n "$SOURCE_SHA" ]]; then
    stamp_dir=""; stamp_sha=""
    # mtime ORDER is the point below, and a glob cannot sort by it; adapter dir
    # names are [A-Za-z0-9.-] only. Same idiom and same filter as
    # check-learning-loops.sh:46 — deliberately kept identical.
    # shellcheck disable=SC2010
    while IFS= read -r d; do
        [[ -n "$d" && -f "$d/source.sha256" ]] || continue
        stamp_dir="${d%/}"
        stamp_sha="$(awk 'NR==1{print $1}' "$d/source.sha256" 2>/dev/null)"
        break
    done < <(ls -dt "$ADAPTERS_DIR"/*/ 2>/dev/null | grep -v '\.rejected-')
    if [[ -n "$stamp_sha" && "$stamp_sha" == "$SOURCE_SHA" ]]; then
        if [[ "${HU_RETRAIN_MLXTUNE:-0}" == "1" ]]; then
            # The candidate stage has its own corpus and needs the serving-down
            # window regardless of the outcome corpus; skip only the base training.
            SKIP_BASE_TRAINING=1
            log "source unchanged since $(basename "$stamp_dir") (sha ${SOURCE_SHA:0:12}) — base training skipped; continuing for the mlx-tune candidate stage"
        else
            log "source unchanged since $(basename "$stamp_dir") (sha ${SOURCE_SHA:0:12}) — skipping training, serving untouched"
            exit 0
        fi
    fi
    if [[ -n "$stamp_sha" ]]; then
        log "source changed since $(basename "$stamp_dir") (stamp ${stamp_sha:0:12} != source ${SOURCE_SHA:0:12}) — training"
    else
        log "no source.sha256 stamp on any staged adapter — training"
    fi
fi

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

# The EXIT trap above only restores what stop_serving reports as stopped.
stop_serving || exit 1

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
if [[ "${SKIP_BASE_TRAINING:-0}" == "1" ]]; then
    log "base training skipped (outcome corpus unchanged); candidate stage follows"
elif [[ -f "$SOURCE_JSONL" ]]; then
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
        # Record WHAT this adapter was trained from, inside the adapter dir, so
        # a later run can tell "nothing new to learn" from "never trained" —
        # see the source-unchanged skip above. Written only here, on the
        # accepted branch: a quarantined adapter must never stamp a digest, or
        # it would authorize skipping the retrain it failed to produce.
        if [[ -n "$SOURCE_SHA" ]]; then
            printf '%s  %s\n' "$SOURCE_SHA" "$SOURCE_JSONL" > "$staged/source.sha256"
            log "  recorded source digest ${SOURCE_SHA:0:12} -> $staged/source.sha256"
        else
            log "  WARNING: no source digest available — next run cannot skip an unchanged corpus"
        fi
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
    # Score the adapter this run just ACCEPTED (source.sha256 is written only in
    # the accepted branch), not the served one: without --adapter the gate falls
    # back to config.json's personalization.lora_adapter_path, so the 2026-09-04/05
    # runs scored the served v6 and no staged adapter ever got a measurement.
    gate_adapter_args=()
    if [[ -n "${staged:-}" && -f "${staged:-/nonexistent}/source.sha256" ]]; then
        gate_adapter_args=(--adapter "$staged")
        log "classifier gate scores tonight's accepted adapter $(basename "$staged")"
    fi
    python3 "$REPO/scripts/blind_ab/classifier_gate.py" --cycle-dir "$CYCLE_DIR" --in-window \
        ${gate_adapter_args[@]+"${gate_adapter_args[@]}"} 2>&1 | tee -a "$LOG"
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
