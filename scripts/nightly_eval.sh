#!/usr/bin/env bash
#
# nightly_eval.sh — serial on-device eval runner.
#
# Runs the two on-device eval harnesses back-to-back, NEVER concurrently,
# so they don't contend for the resident 31B model / the :8741 server:
#
#   1. rung-3 sustained multi-turn coherence  (scripts/eval_multiturn_local.py)
#        - drives the local mlx-server on :8741 (model already resident)
#        - cloud judge via Vertex ADC (degrades to latency-only if ADC absent)
#   2. persona-fidelity SOTA gate             (scripts/eval_fidelity_nightly.py)
#        - generates THROUGH :8741 (--gen served): PRE arm = zero-delta twin
#          of the serving adapter via /v1/adapters/swap, POST = serving adapter
#        - loads a model in-process ONLY when :8741 is down and no trainer
#          holds the weights (never two loaders — 2026-09-03 outage)
#        - runs only AFTER the multi-turn pass has fully exited
#
# Why a single serial wrapper instead of two launchd agents:
#   - Two independent nightly jobs could overlap and load the 31B model
#     twice (OOM / swap) or pollute the multi-turn latency signal.
#   - One wrapper + one launchd agent makes the ordering and the
#     single-instance lock explicit and testable.
#
# Single-instance: an atomic mkdir lock (macOS has no flock). A second
# invocation while one is running exits 0 without stacking.
#
# Verdict retention: each harness's latest verdict is copied to a dated
# archive file and the archive is pruned to the most recent $RETAIN runs.
#
# Usage:
#   scripts/nightly_eval.sh                 # full nightly run (both harnesses)
#   scripts/nightly_eval.sh --dry-run       # validate env + print plan, run nothing
#   scripts/nightly_eval.sh --smoke         # fast: 1 scenario x 3 turns, skip fidelity
#
# Exit codes:
#   0  ran (individual harness verdicts are in the JSON; a harness FAIL or
#          a down server does NOT fail the wrapper — the wrapper's job is to
#          RUN the evals and archive verdicts, not to gate CI)
#   2  could not acquire lock is reported as 0 (benign); 2 is reserved for
#          wrapper-level setup failure (missing python, missing harness file)
#
set -euo pipefail

# ---- Fixed, absolute paths (per worktree-cwd-resets rule) -------------------
# HU_REPO override exists so a worktree copy can be dry-run against itself.
REPO="${HU_REPO:-/Users/sethford/Projects/h-uman}"
PY="/opt/homebrew/bin/python3"
LOG_DIR="/Users/sethford/.human/logs"
ARCHIVE_DIR="${LOG_DIR}/eval-archive"
LOCK_DIR="${LOG_DIR}/.nightly-eval.lock"
# Base model + adapter are resolved by eval_fidelity_nightly.py from the live
# mlx-server (this file used to pin the 4bit base while production served 8bit).
SERVER_URL="http://127.0.0.1:8741"
RETAIN=30

MULTITURN="${REPO}/scripts/eval_multiturn_local.py"
FIDELITY="${REPO}/scripts/eval_fidelity_nightly.py"
BLIND_AB="${REPO}/scripts/eval_blinded_ab.py"
EMOTION_REGISTER="${REPO}/scripts/eval_emotion_register.py"
BLIND_AB_GATE="${REPO}/docs/evaluation/blind_ab_gate.json"
GROUND_TRUTH="${REPO}/data/imessage/ground_truth.jsonl"

# Auto-commit refreshed blind_ab_gate.json after stage 3 (env-overridable).
# Default OFF since 2026-09-02: committing from a launchd job into the shared
# checkout's local `main` collides with interactive sessions (that branch is
# routinely dozens of commits behind origin, so the push silently fails and
# the commits strand — five did between 07-30 and 08-03). The gate JSON
# itself is still written; a human lands it. Set HU_NIGHTLY_AUTOPUSH=1 only
# from a dedicated clone that nothing else uses.
HU_NIGHTLY_AUTOPUSH=${HU_NIGHTLY_AUTOPUSH:-0}

# Advisory Binoculars AI-tell metric after stage 3 (~12 min GPU, serial,
# results-JSON only — never feeds the gate). Disable with HU_NIGHTLY_BINOCULARS=0.
# See docs/research/2026-07-25-binoculars-discriminator.md
HU_NIGHTLY_BINOCULARS=${HU_NIGHTLY_BINOCULARS:-1}

# Binoculars -> DPO miner. OFF by default (feature-gate contract): every
# consumer reads dpo_pairs UNFILTERED, so mined rows enter training on the next
# run. 0=off, 1=SHADOW (candidates logged to the archive, no DB writes),
# 2=LIVE (inserts). Do not set 2 without a threshold recalibrated for the
# CURRENTLY-SERVED adapter.
HU_NIGHTLY_BINOCULARS_DPO=${HU_NIGHTLY_BINOCULARS_DPO:-0}
BINOC_DPO="${REPO}/scripts/blind_ab/binoculars_to_dpo.py"
BLIND_AB_RESULTS="${REPO}/data/eval_blinded_ab.json"

DRY_RUN=0
SMOKE=0
for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=1 ;;
    --smoke)   SMOKE=1 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

mkdir -p "$LOG_DIR" "$ARCHIVE_DIR"

ts() { date '+%Y-%m-%dT%H:%M:%S'; }
log() { echo "[$(ts)] $*" | tee -a "${LOG_DIR}/nightly-eval.log"; }

# ---- Serving-adapter resolution lives in eval_fidelity_nightly.py -----------
# History: a hardcoded .../seth-lora-v4-repair path skipped silently, then a
# v4-repair* glob here measured the WRONG adapter for a week while the server
# served v5-8bit. The harness now resolves the SERVING adapter itself (live
# mlx-server process, else config.json personalization.lora_adapter_path) and
# exits 3 with a FIDELITY_SKIP marker when it can't.

# ---- :8741 health: any HTTP response = up; connection refused / 000 = down --
server_up() {
  local code
  code=$(curl -s -m 5 -o /dev/null -w '%{http_code}' "${SERVER_URL}/health" 2>/dev/null || echo 000)
  [ "$code" != "000" ]
}

adc_present() {
  [ -f "/Users/sethford/.config/gcloud/application_default_credentials.json" ]
}

# The gate runs --gateway so it measures the PRODUCT (real agent turn, real
# persona) rather than a harness-authored prompt against raw MLX. That needs
# the daemon's gateway listening, on the CONFIGURED port — not the legacy
# hardcoded :3002. Without this check a dead gateway degrades every trial to
# "(gateway error: ...)" and the run aborts on consecutive failures.
gateway_url() {
  python3 - <<'PY' 2>/dev/null || echo "http://127.0.0.1:3002"
import json, os
try:
    port = json.load(open(os.path.expanduser("~/.human/config.json")))["gateway"]["port"]
except Exception:
    port = 3002
print(f"http://127.0.0.1:{port}")
PY
}

gateway_up() {
  local code
  code=$(curl -s -m 5 -o /dev/null -w '%{http_code}' "$(gateway_url)/api/status" 2>/dev/null || echo 000)
  [ "$code" = "200" ]
}

# ---- Stage 2 (fidelity) helpers ------------------------------------------------
# Never two loaders: the fidelity harness generates THROUGH :8741 when it is
# up. In-process generation (a second 56 GB copy) is only allowed when :8741 is
# DOWN and no trainer is holding the weights. 2026-09-03 04:31: an in-process
# load beside the live server reached 94.8 GB wired and killed production.
# 2026-09-04: nightly-retrain.sh now really stops :8741 at 03:07, so this 04:05
# run can meet the window mid-flight. The retrain script itself, its
# training_loop.py driver (which spawns `-m mlx_lm lora`), and the steering
# extractor all hold the base — and between those phases only the script is
# alive — so all three count as "a trainer". Pinned by
# scripts/test_nightly_eval_trainer_guard.sh.
trainer_running() {
  pgrep -f "mlx_lm.lora|mlx_lm_lora|train-glm-adapter|mlx_tune_train|dpo_mlx_train|kto_mlx_train|grpo_mlx_train|nightly-retrain\.sh|training_loop\.py|extract_persona_vectors" >/dev/null 2>&1
}
fidelity_mode() {  # served | inprocess | skip-trainer
  if server_up; then echo served
  elif trainer_running; then echo skip-trainer
  else echo inprocess
  fi
}
wired_gb() { vm_stat | awk '/Pages wired down/ {gsub("\\.","",$4); printf "%.1f", $4*16384/1073741824}'; }
loader_count() { pgrep -f "mlx-server.py|python.*-m mlx_lm|mlx_lm.lora|mlx_tune_train|dpo_mlx_train" | wc -l | tr -d ' '; }
# Background sampler: peak wired GB + max concurrent model loaders during a
# step, written to a file every 5 s. The artifact the acceptance reads
# (< 75 GB wired, exactly one loader) — not the harness's own report.
SAMPLER_PID=""; SAMPLER_FILE=""
start_resource_sampler() {
  SAMPLER_FILE=$(mktemp "${LOG_DIR}/.nightly-sampler.XXXXXX")
  (
    peak=0; loaders=0
    while :; do
      w=$(wired_gb); l=$(loader_count)
      if awk -v w="$w" -v p="$peak" 'BEGIN{exit !(w>p)}'; then peak=$w; fi
      [ "${l:-0}" -gt "$loaders" ] && loaders=$l
      printf 'peak_wired_gb=%s max_loaders=%s\n' "$peak" "$loaders" > "$SAMPLER_FILE"
      sleep 5
    done
  ) &
  SAMPLER_PID=$!
}
stop_resource_sampler() {  # prints the sampler's last line
  [ -n "$SAMPLER_PID" ] && { kill "$SAMPLER_PID" 2>/dev/null || true; wait "$SAMPLER_PID" 2>/dev/null || true; }
  [ -n "$SAMPLER_FILE" ] && { cat "$SAMPLER_FILE" 2>/dev/null; rm -f "$SAMPLER_FILE"; }
  SAMPLER_PID=""; SAMPLER_FILE=""
}
# Safety net: if the harness could not put the serving adapter back after the
# PRE arm, production is on the zero adapter. One more swap + verify from here.
restore_serving_adapter() {  # adapter_path
  local want="$1" got
  [ -n "$want" ] || return 1
  curl -s -m 600 -X POST -H 'Content-Type: application/json' \
    -d "{\"adapter_path\": \"$want\"}" "${SERVER_URL}/v1/adapters/swap" >/dev/null 2>&1 || true
  got=$(curl -s -m 15 "${SERVER_URL}/v1/adapters/current" 2>/dev/null \
        | python3 -c 'import json,sys; print(json.load(sys.stdin).get("adapter_path") or "")' 2>/dev/null || true)
  [ -n "$got" ] && [ "$(cd "$got" 2>/dev/null && pwd -P)" = "$(cd "$want" 2>/dev/null && pwd -P)" ]
}

# ---- Stage 3 precondition checks ------------------------------------------------
# Sufficiency, not existence: the gate stamps ENFORCING only at >= 30 real pairs
# (ENFORCE_MIN_PAIRS in blind_ab_gate.py); below that a run wastes GPU on an
# ADVISORY verdict the freshness gate ignores. 2026-07-11: file had rotted to 2
# pairs while "exists and non-empty" passed. Refresh: scripts/extract_imessage_pairs.py
GROUND_TRUTH_MIN_PAIRS=30
ground_truth_exists() {
  [ -f "$GROUND_TRUTH" ] || return 1
  [ "$(wc -l < "$GROUND_TRUTH" | tr -d ' ')" -ge "$GROUND_TRUTH_MIN_PAIRS" ]
}

judge_creds_present() {
  # Judge uses ADC if GEMINI_API_KEY is not set
  [ -n "${GEMINI_API_KEY:-}" ] || adc_present
}

# ---- Verdict archive + prune ------------------------------------------------
archive_verdict() {
  local harness="$1" latest_json="$2"
  [ -f "$latest_json" ] || { log "  (no verdict json at $latest_json — nothing to archive)"; return 0; }
  # 2026-09-04: smoke runs get their own slot. Two --smoke invocations had
  # overwritten the day's real multiturn archive with a 1-scenario result, and
  # the doctor's eval_freshness check ignores "-smoke-" files by name.
  local tag=""
  [ "$SMOKE" -eq 1 ] && tag="smoke-"
  local dated="${ARCHIVE_DIR}/eval-${harness}-${tag}$(date '+%Y-%m-%d').json"
  cp -f "$latest_json" "$dated"
  log "  archived → $dated"
  # Prune: keep the $RETAIN most recent dated files for this harness+slot.
  local stale
  stale=$(ls -t "${ARCHIVE_DIR}/eval-${harness}-${tag}"[0-9]*.json 2>/dev/null | tail -n +$((RETAIN + 1)) || true)
  if [ -n "$stale" ]; then
    echo "$stale" | xargs rm -f
    log "  pruned $(echo "$stale" | wc -l | tr -d ' ') old ${harness} verdict(s)"
  fi
}

# ---- Preflight --------------------------------------------------------------
[ -x "$PY" ]            || { log "FATAL: python3 not found at $PY"; exit 2; }
[ -f "$MULTITURN" ]     || { log "FATAL: missing $MULTITURN"; exit 2; }
[ -f "$FIDELITY" ]      || { log "FATAL: missing $FIDELITY"; exit 2; }

ADAPTER=$("$PY" "$FIDELITY" --resolve-only 2>/dev/null || true)

if [ "$DRY_RUN" -eq 1 ]; then
  log "=== DRY RUN (validate only, run nothing) ==="
  log "  python3:       $PY ($($PY --version 2>&1))"
  log "  multiturn:     $MULTITURN"
  log "  fidelity:      $FIDELITY"
  log "  blind_ab:      $BLIND_AB"
  log "  :8741 server:  $(server_up && echo UP || echo DOWN)"
  log "  judge ADC:     $(adc_present && echo PRESENT || echo ABSENT)"
  log "  ground truth:  $(ground_truth_exists && echo FOUND || echo MISSING)"
  log "  judge creds:   $(judge_creds_present && echo AVAILABLE || echo UNAVAILABLE)"
  log "  adapter:       ${ADAPTER:-'(unresolved — fidelity will SKIP loudly)'}"
  log "  archive dir:   $ARCHIVE_DIR (retain last $RETAIN per harness)"
  log "  lock dir:      $LOCK_DIR ($([ -d "$LOCK_DIR" ] && echo HELD || echo free))"
  log "  autopush:      HU_NIGHTLY_AUTOPUSH=$HU_NIGHTLY_AUTOPUSH"
  log "  binoculars:    HU_NIGHTLY_BINOCULARS=$HU_NIGHTLY_BINOCULARS (advisory AI-tell after stage 3)"
  log "  binoc-dpo:     HU_NIGHTLY_BINOCULARS_DPO=$HU_NIGHTLY_BINOCULARS_DPO (0=off, 1=shadow, 2=live)"
  log "  fidelity mode: $(fidelity_mode)"
  log "  plan: [1/4] multiturn (needs :8741), [2/4] fidelity (served via :8741; in-process only if DOWN), [3/4] blind-ab gate refresh (REAL measurement), [4/4] emotion register (local judge on :8741), serial"
  exit 0
fi

# ---- Acquire single-instance lock (atomic mkdir) ----------------------------
# A lock whose pid is gone is stale (the 2026-09-03 04:31 crash + reboot left
# one behind, and every later run would have exited here forever). Reclaim it.
if ! mkdir "$LOCK_DIR" 2>/dev/null; then
  lock_pid=$(cat "${LOCK_DIR}/pid" 2>/dev/null || true)
  if [ -n "$lock_pid" ] && kill -0 "$lock_pid" 2>/dev/null; then
    log "another nightly_eval run holds the lock ($LOCK_DIR, pid $lock_pid) — exiting without stacking"
    exit 0
  fi
  log "reclaiming stale lock $LOCK_DIR (pid ${lock_pid:-unknown} is gone)"
  rm -rf "$LOCK_DIR"
  mkdir "$LOCK_DIR" 2>/dev/null || { log "could not reclaim lock — exiting"; exit 0; }
fi
echo "$$" > "${LOCK_DIR}/pid"
cleanup() { rm -rf "$LOCK_DIR"; }
trap cleanup EXIT INT TERM

log "=== nightly_eval start (smoke=$SMOKE) ==="

# ---- 1) rung-3 multi-turn coherence (uses :8741) ----------------------------
MT_OUT="${LOG_DIR}/eval-multiturn-local.json"
if server_up; then
  log "[1/4] multi-turn: :8741 UP — running"
  MT_ARGS=(--server-url "$SERVER_URL" --output-json "$MT_OUT")
  if [ "$SMOKE" -eq 1 ]; then
    MT_ARGS+=(--limit-scenarios 1 --max-turns 3)
  fi
  # exit 0=PASS 1=FAIL 2=server-unreachable 3=judge-SKIPPED — all are "ran".
  set +e
  "$PY" "$MULTITURN" "${MT_ARGS[@]}"; mt_rc=$?
  set -e
  log "[1/4] multi-turn exit=$mt_rc ($(case $mt_rc in 0)echo PASS;;1)echo FAIL;;2)echo DEFERRED-server-down;;3)echo SKIPPED-judge;;*)echo "rc=$mt_rc";;esac))"
  archive_verdict multiturn "$MT_OUT"
else
  log "[1/4] multi-turn: :8741 DOWN — deferring (mlx-server not running)"
fi

# ---- 2) persona-fidelity SOTA gate (served through :8741; serial after #1) --
# No --adapter-path: the harness resolves the SERVING adapter itself and
# exits 3 + FIDELITY_SKIP when it can't (grep the log for FIDELITY_SKIP).
# --gen served: generation goes through the production server; the PRE arm
# swaps a zero-delta twin of the serving adapter in (the serving build's
# /v1/adapters/swap has no unload), the adapter is restored + verified, then
# POST runs on the serving adapter. The harness itself DEFERs rather than load
# in-process beside a live server; this wrapper adds the trainer check for the
# server-DOWN case, a resource sampler, and a restore safety net.
if [ "$SMOKE" -eq 1 ]; then
  log "[2/4] fidelity: skipped (--smoke)"
else
  FID_OUT="${LOG_DIR}/eval-fidelity-nightly-latest.json"
  fid_mode=$(fidelity_mode)
  if [ "$fid_mode" = "skip-trainer" ]; then
    fid_rc=3
    log "[2/4] fidelity: SKIPPED FIDELITY_SKIP — :8741 is down but a trainer holds the model (never two loaders)"
  else
    fid_base_url=""
    [ "$fid_mode" = "served" ] && fid_base_url="$SERVER_URL"
    log "[2/4] fidelity: mode=$fid_mode serving-adapter=${ADAPTER:-'(unresolved)'} — running"
    start_resource_sampler
    set +e
    HU_MLX_BASE_URL="$fid_base_url" "$PY" "$FIDELITY" \
      --gen "$fid_mode" \
      --output-json "$FID_OUT" \
      --log-dir "$LOG_DIR"; fid_rc=$?
    set -e
    fid_resources=$(stop_resource_sampler)
    log "[2/4] fidelity exit=$fid_rc ($(case $fid_rc in 0)echo PASS;;1)echo FAIL;;2)echo DEFERRED;;3)echo SKIP-see-FIDELITY_SKIP-marker;;*)echo "rc=$fid_rc";;esac))"
    log "[2/4] fidelity resources: ${fid_resources:-'(no samples)'}"
    peak=${fid_resources#peak_wired_gb=}; peak=${peak%% *}
    loaders=${fid_resources##*max_loaders=}
    if [ -n "$fid_resources" ] && { awk -v p="${peak:-0}" 'BEGIN{exit !(p>=75)}' || [ "${loaders:-0}" -gt 1 ]; }; then
      log "[2/4] fidelity: RESOURCE_BREACH peak_wired_gb=$peak max_loaders=$loaders (limit: <75 GB, 1 loader)"
    fi
    # Restore safety net — read the ARTIFACT (verdict JSON), not the log.
    if [ "$fid_mode" = "served" ] && [ -f "$FID_OUT" ] && \
       python3 -c 'import json,sys; g=json.load(open(sys.argv[1])).get("generation") or {}; sys.exit(0 if g.get("adapter_restored") is False else 1)' "$FID_OUT" 2>/dev/null; then
      log "[2/4] fidelity: FIDELITY_RESTORE_FAILED reported by the harness — retrying restore of $ADAPTER from the wrapper"
      if restore_serving_adapter "$ADAPTER"; then
        log "[2/4] fidelity: serving adapter restored + verified by wrapper"
      else
        log "[2/4] fidelity: RESTORE STILL FAILED — :8741 may be serving the zero adapter; restore by hand: curl -X POST ${SERVER_URL}/v1/adapters/swap -d '{\"adapter_path\": \"$ADAPTER\"}'"
      fi
    fi
    archive_verdict fidelity "$FID_OUT"
  fi
fi

# ---- 3) REAL blind-A/B gate refresh (product via daemon gateway + Gemini judge) ---
# This stage runs the REAL proxy blind-A/B, not a dry-run, so the measurement
# refreshes the committed verdict artifact. Only skipped on --smoke; precondition
# failures log loudly but do NOT fail the wrapper (contract: run evals, don't gate).
#
# --gateway, NOT --mlx (changed 2026-07-27). --mlx posted a hand-written
# SETH_SYSTEM_PROMPT straight to :8741, so the persona pipeline was never
# exercised and every style claim in that prompt became an AI tell the moment it
# was wrong — twice measured ("Lowercase." ~10x off, "Abbreviate (gonna, tbh,
# idk, hru)" drove tbh ~200x over Seth's real rate). --gateway runs the real
# agent turn through the daemon, which builds the persona itself. Generation
# still lands on :8741 underneath, so server_up remains a precondition.
if [ "$SMOKE" -eq 1 ]; then
  log "[3/4] blind-ab: skipped (--smoke)"
else
  # Pre-flight: ground truth pairs, :8741 health, judge credentials
  stage3_skip=0
  if ! ground_truth_exists; then
    log "[3/4] blind-ab: SKIPPED — ground truth missing or < ${GROUND_TRUTH_MIN_PAIRS} pairs (run scripts/extract_imessage_pairs.py)"
    log "           path: $GROUND_TRUTH"
    log "           generate with: python3 $REPO/scripts/extract_imessage_pairs.py"
    stage3_skip=1
  fi
  if [ "$stage3_skip" -eq 0 ] && ! server_up; then
    log "[3/4] blind-ab: SKIPPED — :8741 server not responding"
    stage3_skip=1
  fi
  if [ "$stage3_skip" -eq 0 ] && ! gateway_up; then
    log "[3/4] blind-ab: SKIPPED — daemon gateway not responding at $(gateway_url)"
    log "           the gate measures the product via --gateway; start the daemon"
    log "           with --with-gateway, or set HU_GATEWAY_URL."
    stage3_skip=1
  fi
  if [ "$stage3_skip" -eq 0 ] && ! judge_creds_present; then
    log "[3/4] blind-ab: SKIPPED — judge credentials unavailable (no GEMINI_API_KEY, no ADC)"
    stage3_skip=1
  fi

  if [ "$stage3_skip" -eq 0 ]; then
    log "[3/4] blind-ab: all preconditions met — running REAL gate measurement"
    # Capture the gate file state BEFORE the run
    gate_before=""
    [ -f "$BLIND_AB_GATE" ] && gate_before=$(cat "$BLIND_AB_GATE")

    binoc_flag=""
    [ "$HU_NIGHTLY_BINOCULARS" -eq 1 ] && binoc_flag="--binoculars"

    set +e
    # shellcheck disable=SC2086  # binoc_flag is intentionally word-split (empty or one flag)
    "$PY" "$BLIND_AB" --gate --gateway $binoc_flag; ab_rc=$?
    set -e

    log "[3/4] blind-ab exit=$ab_rc ($(case $ab_rc in 0)echo PASS;;1)echo FAIL;;*)echo "rc=$ab_rc";;esac))"

    # ---- Binoculars -> DPO miner (advisory; OFF by default) -----------------
    # Reads the per-trial scores eval_blinded_ab.py --binoculars merged into the
    # results file — no extra GPU pass. Never fails the wrapper.
    if [ "$HU_NIGHTLY_BINOCULARS_DPO" -ne 0 ] && [ -f "$BLIND_AB_RESULTS" ]; then
      dpo_mode_args="--shadow-out ${ARCHIVE_DIR}/binoc-dpo-candidates.jsonl"
      dpo_mode_name="SHADOW"
      if [ "$HU_NIGHTLY_BINOCULARS_DPO" -eq 2 ]; then
        dpo_mode_args="--live"
        dpo_mode_name="LIVE"
      fi
      log "[3/4] binoc-dpo: mining in $dpo_mode_name mode"
      set +e
      # shellcheck disable=SC2086  # dpo_mode_args is intentionally word-split
      "$PY" "$BINOC_DPO" --pairs "$BLIND_AB_RESULTS" $dpo_mode_args 2>&1 \
        | while IFS= read -r line; do log "[3/4] binoc-dpo: $line"; done
      set -e
    elif [ "$HU_NIGHTLY_BINOCULARS_DPO" -eq 0 ]; then
      log "[3/4] binoc-dpo: OFF (HU_NIGHTLY_BINOCULARS_DPO=0)"
    fi

    # Auto-commit if the gate file changed
    if [ "$HU_NIGHTLY_AUTOPUSH" -eq 1 ] && [ -f "$BLIND_AB_GATE" ]; then
      gate_after=$(cat "$BLIND_AB_GATE")
      if [ "$gate_before" != "$gate_after" ]; then
        log "[3/4] blind-ab: gate artifact changed — staging auto-commit"
        if git -C "$REPO" add "$BLIND_AB_GATE" 2>/dev/null; then
          if git -C "$REPO" commit -m "chore(eval): nightly blind-ab gate refresh" 2>/dev/null; then
            if git -C "$REPO" push origin main 2>/dev/null; then
              log "[3/4] blind-ab: committed + pushed $BLIND_AB_GATE"
            else
              log "[3/4] blind-ab: committed but push failed — may need rebase"
            fi
          else
            log "[3/4] blind-ab: staging succeeded but commit failed (dirty tree?)"
          fi
        else
          log "[3/4] blind-ab: could not stage $BLIND_AB_GATE"
        fi
      else
        log "[3/4] blind-ab: gate artifact unchanged, no commit needed"
      fi
    fi
  fi
fi

# ---- 4) emotional register vs the persona's emotion card (local judge) -------
# Labels the twin's SENT iMessage replies (memory.db production_outcomes) with
# the same :8741 judge that built ~/.human/personas/<persona>.emotion-card.json
# and writes the Jensen-Shannon divergence + axis deltas. Measurement only —
# it feeds the HU_EMOTION_REGISTER activation decision, never a gate. HTTP to
# :8741 only (no second loader). exit 0=MEASURED 2=DEFERRED 3=REFUSED (no
# card / judge mismatch / n < min) — all are "ran"; 2 and 3 write nothing.
if [ "$SMOKE" -eq 1 ]; then
  log "[4/4] emotion-register: skipped (--smoke)"
elif ! [ -f "$EMOTION_REGISTER" ]; then
  log "[4/4] emotion-register: SKIPPED — missing $EMOTION_REGISTER"
elif server_up; then
  ER_OUT="${LOG_DIR}/eval-emotion-register-latest.json"
  log "[4/4] emotion-register: :8741 UP — running"
  set +e
  "$PY" "$EMOTION_REGISTER" --output-json "$ER_OUT"; er_rc=$?
  set -e
  log "[4/4] emotion-register exit=$er_rc ($(case $er_rc in 0)echo MEASURED;;2)echo DEFERRED-judge-down;;3)echo REFUSED-see-stderr;;*)echo "rc=$er_rc";;esac))"
  [ "$er_rc" -eq 0 ] && archive_verdict emotion-register "$ER_OUT"
else
  log "[4/4] emotion-register: :8741 DOWN — deferring"
fi

log "=== nightly_eval done ==="
exit 0
