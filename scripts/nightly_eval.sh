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
#        - spins up its OWN mlx_lm.generate subprocess (loads the model)
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
REPO="/Users/sethford/Projects/h-uman"
PY="/opt/homebrew/bin/python3"
LOG_DIR="/Users/sethford/.human/logs"
ARCHIVE_DIR="${LOG_DIR}/eval-archive"
LOCK_DIR="${LOG_DIR}/.nightly-eval.lock"
ADAPTER_GLOB="/Users/sethford/.human/training-data/adapters/seth-lora-v4-repair*"
MODEL_ID="mlx-community/gemma-4-31b-it-4bit"
SERVER_URL="http://127.0.0.1:8741"
RETAIN=30

MULTITURN="${REPO}/scripts/eval_multiturn_local.py"
FIDELITY="${REPO}/scripts/eval_fidelity_nightly.py"

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

# ---- Resolve the newest v4-repair adapter dir (survives re-training) --------
# The launchd plist used to hardcode .../seth-lora-v4-repair (no timestamp
# suffix), which no longer exists, so every nightly fidelity run SKIPped.
# Glob + newest-mtime makes the schedule self-healing across re-trains.
resolve_adapter() {
  local newest
  # shellcheck disable=SC2086
  newest=$(ls -dt $ADAPTER_GLOB 2>/dev/null | head -1 || true)
  printf '%s' "$newest"
}

# ---- :8741 health: any HTTP response = up; connection refused / 000 = down --
server_up() {
  local code
  code=$(curl -s -m 5 -o /dev/null -w '%{http_code}' "${SERVER_URL}/health" 2>/dev/null || echo 000)
  [ "$code" != "000" ]
}

adc_present() {
  [ -f "/Users/sethford/.config/gcloud/application_default_credentials.json" ]
}

# ---- Verdict archive + prune ------------------------------------------------
archive_verdict() {
  local harness="$1" latest_json="$2"
  [ -f "$latest_json" ] || { log "  (no verdict json at $latest_json — nothing to archive)"; return 0; }
  local dated="${ARCHIVE_DIR}/eval-${harness}-$(date '+%Y-%m-%d').json"
  cp -f "$latest_json" "$dated"
  log "  archived → $dated"
  # Prune: keep the $RETAIN most recent dated files for this harness.
  local stale
  stale=$(ls -t "${ARCHIVE_DIR}/eval-${harness}-"*.json 2>/dev/null | tail -n +$((RETAIN + 1)) || true)
  if [ -n "$stale" ]; then
    echo "$stale" | xargs rm -f
    log "  pruned $(echo "$stale" | wc -l | tr -d ' ') old ${harness} verdict(s)"
  fi
}

# ---- Preflight --------------------------------------------------------------
[ -x "$PY" ]            || { log "FATAL: python3 not found at $PY"; exit 2; }
[ -f "$MULTITURN" ]     || { log "FATAL: missing $MULTITURN"; exit 2; }
[ -f "$FIDELITY" ]      || { log "FATAL: missing $FIDELITY"; exit 2; }

ADAPTER=$(resolve_adapter)

if [ "$DRY_RUN" -eq 1 ]; then
  log "=== DRY RUN (validate only, run nothing) ==="
  log "  python3:       $PY ($($PY --version 2>&1))"
  log "  multiturn:     $MULTITURN"
  log "  fidelity:      $FIDELITY"
  log "  :8741 server:  $(server_up && echo UP || echo DOWN)"
  log "  judge ADC:     $(adc_present && echo PRESENT || echo ABSENT)"
  log "  adapter:       ${ADAPTER:-'(none found for '"$ADAPTER_GLOB"')'}"
  log "  archive dir:   $ARCHIVE_DIR (retain last $RETAIN per harness)"
  log "  lock dir:      $LOCK_DIR ($([ -d "$LOCK_DIR" ] && echo HELD || echo free))"
  log "  plan: multiturn first (needs :8741), then fidelity (loads own model), serial"
  exit 0
fi

# ---- Acquire single-instance lock (atomic mkdir) ----------------------------
if ! mkdir "$LOCK_DIR" 2>/dev/null; then
  log "another nightly_eval run holds the lock ($LOCK_DIR) — exiting without stacking"
  exit 0
fi
echo "$$" > "${LOCK_DIR}/pid"
cleanup() { rm -rf "$LOCK_DIR"; }
trap cleanup EXIT INT TERM

log "=== nightly_eval start (smoke=$SMOKE) ==="

# ---- 1) rung-3 multi-turn coherence (uses :8741) ----------------------------
MT_OUT="${LOG_DIR}/eval-multiturn-local.json"
if server_up; then
  log "[1/2] multi-turn: :8741 UP — running"
  MT_ARGS=(--server-url "$SERVER_URL" --output-json "$MT_OUT")
  if [ "$SMOKE" -eq 1 ]; then
    MT_ARGS+=(--limit-scenarios 1 --max-turns 3)
  fi
  # exit 0=PASS 1=FAIL 2=server-unreachable 3=judge-SKIPPED — all are "ran".
  set +e
  "$PY" "$MULTITURN" "${MT_ARGS[@]}"; mt_rc=$?
  set -e
  log "[1/2] multi-turn exit=$mt_rc ($(case $mt_rc in 0)echo PASS;;1)echo FAIL;;2)echo DEFERRED-server-down;;3)echo SKIPPED-judge;;*)echo "rc=$mt_rc";;esac))"
  archive_verdict multiturn "$MT_OUT"
else
  log "[1/2] multi-turn: :8741 DOWN — deferring (mlx-server not running)"
fi

# ---- 2) persona-fidelity SOTA gate (loads its own model; serial after #1) ---
if [ "$SMOKE" -eq 1 ]; then
  log "[2/2] fidelity: skipped (--smoke)"
elif [ -z "$ADAPTER" ]; then
  log "[2/2] fidelity: skipped (no adapter matched $ADAPTER_GLOB)"
else
  FID_OUT="${LOG_DIR}/eval-fidelity-nightly-latest.json"
  log "[2/2] fidelity: adapter=$ADAPTER — running"
  set +e
  "$PY" "$FIDELITY" \
    --adapter-path "$ADAPTER" \
    --model-id "$MODEL_ID" \
    --output-json "$FID_OUT" \
    --log-dir "$LOG_DIR"; fid_rc=$?
  set -e
  log "[2/2] fidelity exit=$fid_rc ($(case $fid_rc in 0)echo PASS-or-SKIP;;1)echo FAIL;;*)echo "rc=$fid_rc";;esac))"
  archive_verdict fidelity "$FID_OUT"
fi

log "=== nightly_eval done ==="
exit 0
