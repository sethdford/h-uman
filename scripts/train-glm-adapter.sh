#!/usr/bin/env bash
# Train seth-glm-air-v6 (ORPO preference round) on the GLM serving base.
#
# This box cannot hold two large MLX models: 128 GB total, the GLM-4.5-Air-4bit
# base is ~56 GB, and production mlx-server holds ~44 GB wired. Training therefore
# REQUIRES stopping the production server first -- see the user rule recorded in
# memory `never_two_llm_instances` (2026-07-26): a second loader exhausts swap and
# the OS kills processes with no traceback.
#
# Production is restored from a trap, so it comes back even if training crashes,
# is interrupted, or the config is rejected. The server is restarted on its
# EXISTING v5 config -- this script never repoints or promotes anything.
#
# Usage: scripts/train-glm-v6-orpo.sh [--dry-run]
set -uo pipefail

TRAIN_PY=/Users/sethford/.human/venvs/train312/bin/python
SERVICE=gui/501/ai.human.mlx-server

# Defaults are the v6 run; v6.1 and later pass --config/--beta/--tag rather than
# forking this script, so every run keeps the same guards.
CONFIG=/Users/sethford/.human/training-data/glm-v62-sft-config.yaml
ORPO_BETA=0.05
TAG=v62-sft
TRAINER=mlx_lm            # mlx_lm (SFT, known-good) | mlx_lm_lora (preference modes)
TRAIN_MODE=""             # only meaningful for mlx_lm_lora
EST_MINUTES=25            # estimated run length; gates the arena-overlap check
DRY_RUN=0

while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run)    DRY_RUN=1; shift ;;
    --config)     CONFIG=$2; shift 2 ;;
    --beta)       ORPO_BETA=$2; shift 2 ;;
    --tag)        TAG=$2; shift 2 ;;
    --trainer)    TRAINER=$2; shift 2 ;;
    --train-mode) TRAIN_MODE=$2; shift 2 ;;
    --est-minutes) EST_MINUTES=$2; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

# mlx-lm-lora's orpo and cpo modes are KNOWN BROKEN on 3.0.0: they exit 0 and
# write an adapter whose every lora_b is 0.0, i.e. a mathematical no-op. Verified
# with a 60s repro on gemma-2-2b (orpo 0/28, cpo 0/28, dpo 28/28 non-zero).
# The post-run lora_b guard below catches it regardless, but refuse up front
# rather than spend a production-dark window producing a known no-op.
ORPO_SRC=/Users/sethford/.human/venvs/train312/lib/python3.12/site-packages/mlx_lm_lora/trainer/orpo_trainer.py
case "$TRAINER:$TRAIN_MODE" in
  mlx_lm_lora:orpo)
    # Stock 3.0.0 computes the forward pass OUTSIDE the function nn.value_and_grad
    # differentiates, so every gradient is structurally zero and the adapter is a
    # no-op (all lora_b == 0). Verified on gemma-2-2b: 0/28 before, 28/28 after.
    # Allow orpo ONLY when the local patch is present.
    if ! grep -q "PATCHED: the forward pass MUST happen inside" "$ORPO_SRC" 2>/dev/null; then
      echo "[train] REFUSING --train-mode orpo: $ORPO_SRC is unpatched and emits a" >&2
      echo "        no-op adapter (all lora_b == 0). Re-apply the patch first." >&2
      exit 2
    fi
    echo "[train] orpo patch present in mlx_lm_lora — proceeding" ;;
  mlx_lm_lora:cpo)
    echo "[train] REFUSING --train-mode cpo: same no-op defect, not patched." >&2
    exit 2 ;;
esac

STAMP=$(date +%Y%m%d-%H%M%S)
ADAPTER=/Users/sethford/.human/training-data/adapters/seth-glm-air-${TAG}-${STAMP}
LOG=/Users/sethford/.human/logs/train-glm-${TAG}-${STAMP}.log
# The data dir is whatever the config says -- read it back so preflight checks the
# corpus this run will actually use, not a hardcoded guess.
DATA_DIR=$(awk '/^data:/{print $2; exit}' "$CONFIG" 2>/dev/null)

say() { printf '[v6-train] %s\n' "$*"; }
die() { printf '[v6-train] FATAL: %s\n' "$*" >&2; exit 1; }

# --- preflight: refuse rather than half-run -----------------------------------
[ -x "$TRAIN_PY" ]  || die "training venv missing: $TRAIN_PY"
[ -f "$CONFIG" ]    || die "config missing: $CONFIG"
[ -s "$DATA_DIR/train.jsonl" ] || die "corpus missing: $DATA_DIR/train.jsonl"

# The config is the only thing standing between us and a scale=10.0 adapter.
grep -qE '^\s+scale:\s*2\.0\s*$' "$CONFIG" \
  || die "config has no nested 'scale: 2.0' -- refusing (lora-scale-default-or-die)"
grep -qE '^lora_parameters:' "$CONFIG" \
  || die "lora_parameters is not a top-level nested block -- flat keys are silently ignored"

# A second model-loading launchd job mid-run reproduces the exact thrash this
# script exists to avoid. conversation-arena runs at :23 on hours 2,6,10,14,18,22.
# Minutes matter: at 06:16 the next slot is 06:23, NOT 10:23.
read -r NEXT_ARENA MINS_AWAY <<<"$(date '+%H %M' | awk '{
  h=$1+0; m=$2+0; now=h*60+m;
  best=-1;
  for (i=0; i<6; i++) { s=(2+4*i)*60+23; if (s>now && best<0) best=s }
  if (best<0) best=(2*60+23)+24*60;          # wrap to tomorrow 02:23
  printf "%02d:23 %d", int(best/60)%24, best-now
}')"
say "now $(date '+%H:%M'), next conversation-arena slot ${NEXT_ARENA} (${MINS_AWAY} min away)"
# The hazard is OVERLAP, not proximity: what matters is whether this run finishes
# before the arena loads its own model. Gate on the estimated run length plus a
# buffer rather than a flat constant, so a 15-minute job is not blocked by a
# threshold sized for a 40-minute one. --est-minutes is the caller's honest
# estimate; the buffer absorbs a slow model load.
NEEDED=$(( EST_MINUTES + 15 ))
if [ "$MINS_AWAY" -lt "$NEEDED" ]; then
  die "conversation-arena fires in ${MINS_AWAY} min; this run needs ~${EST_MINUTES} min
       plus a 15 min buffer (${NEEDED}). A second loader beside the 56 GB base is the
       documented thrash condition. Wait for that arena run to finish, then re-run."
fi

if [ "$DRY_RUN" = "1" ]; then
  say "DRY RUN -- would train into $ADAPTER"
  say "corpus: $(wc -l < "$DATA_DIR/train.jsonl") train / $(wc -l < "$DATA_DIR/valid.jsonl") valid"
  exit 0
fi

# --- restore trap: prod comes back no matter how we leave ----------------------
PROD_RESTORED=0
restore_prod() {
  [ "$PROD_RESTORED" = "1" ] && return 0
  PROD_RESTORED=1
  say "restoring production mlx-server (unchanged v5 config)..."
  # bootstrap FIRST: we booted the job out, so it is not in the domain and
  # `kickstart` would fail. kickstart is only the fallback for an already-loaded
  # job. Order matters -- the reverse leaves a window where neither ran.
  launchctl bootstrap gui/501 ~/Library/LaunchAgents/ai.human.mlx-server.plist 2>/dev/null \
    || launchctl kickstart -k "$SERVICE" 2>/dev/null
  for _ in $(seq 1 60); do
    # A listening socket alone is NOT enough: on 2026-07-27 the port came up,
    # this loop returned success, and the job was out of launchd 30s later.
    # Require the launchd JOB to be loaded as well, so KeepAlive can revive it.
    if lsof -nP -iTCP:8741 -sTCP:LISTEN >/dev/null 2>&1 \
       && launchctl list "${SERVICE##*/}" >/dev/null 2>&1; then
      say "production listening on :8741 and job is loaded in launchd"
      return 0
    fi
    sleep 5
  done
  say "WARNING: prod not fully restored after 5 min. Recover with:"
  say "  launchctl bootstrap gui/501 ~/Library/LaunchAgents/ai.human.mlx-server.plist"
}
trap restore_prod EXIT INT TERM

# --- stop prod and wait for a FULL reap ---------------------------------------
PROD_PID=$(lsof -tnP -iTCP:8741 -sTCP:LISTEN 2>/dev/null | head -1)
say "stopping production mlx-server (pid ${PROD_PID:-none}) to free ~44 GB..."
launchctl bootout "$SERVICE" 2>/dev/null
for _ in $(seq 1 60); do
  # Scope the reap check to the process WE stopped. A broad `pgrep -f mlx-server`
  # also matches unrelated servers on other ports (e.g. an orphaned :8743 arena
  # server), which is not what "did prod reap?" asks. A `?E` zombie still holds
  # wired pages, so wait for the pid to be GONE, not merely exiting.
  if { [ -z "$PROD_PID" ] || ! kill -0 "$PROD_PID" 2>/dev/null; } \
     && ! lsof -nP -iTCP:8741 -sTCP:LISTEN >/dev/null 2>&1; then
    break
  fi
  sleep 2
done
if [ -n "$PROD_PID" ] && kill -0 "$PROD_PID" 2>/dev/null; then
  die "production pid $PROD_PID did not reap after 120s -- refusing to load a second model"
fi

# Ground truth, not a proxy: the hazard is memory exhaustion, so measure memory.
# On macOS, reclaimable == free + inactive; counting only `Pages free` understates
# headroom by tens of GB and would refuse on a perfectly healthy box.
PAGE=$(vm_stat | awk -F'of ' '/page size of/{print $2+0}')
free_pages=$(vm_stat | awk '/Pages free/{gsub("\\.","",$3); print $3}')
inact_pages=$(vm_stat | awk '/Pages inactive/{gsub("\\.","",$3); print $3}')
AVAIL_GB=$(( (free_pages + inact_pages) * PAGE / 1073741824 ))
NEED_GB=70   # ~56 GB base + LoRA activations/optimizer headroom
say "reaped. available=${AVAIL_GB}GB (free+inactive), need >=${NEED_GB}GB"
OTHERS=$(pgrep -fl "mlx-server" 2>/dev/null | grep -v "port 8741" | wc -l | tr -d ' ')
[ "$OTHERS" -gt 0 ] && say "NOTE: $OTHERS other mlx-server process(es) resident; counted in the figure above"
[ "$AVAIL_GB" -lt "$NEED_GB" ] && die "only ${AVAIL_GB}GB available -- a second large load here is the documented thrash condition"

# --- train --------------------------------------------------------------------
mkdir -p "$ADAPTER" "$(dirname "$LOG")"
say "training -> $ADAPTER"
say "log      -> $LOG"
# --train-mode and --beta MUST be CLI flags. mlx_lm_lora applies a YAML key only
# when the corresponding argparse default is None; these two default to "sft" and
# 0.1 respectively, so setting them in the config is a SILENT no-op. Verified on
# 3.0.0 -- `train_mode: orpo` in YAML produced "Unsupported data format for SFT
# training", and `beta: 0.05` would have silently trained at 0.1.
# ORPO_BETA comes from --beta (or its default) at the top; do NOT reassign it here.
if [ "$TRAINER" = "mlx_lm" ]; then
  say "trainer: mlx_lm.lora (SFT) -- the path that produced the live v5 adapter"
  "$TRAIN_PY" -m mlx_lm.lora -c "$CONFIG" --adapter-path "$ADAPTER" 2>&1 | tee "$LOG"
else
  say "trainer: mlx_lm_lora --train-mode ${TRAIN_MODE:-dpo} --beta $ORPO_BETA"
  "$TRAIN_PY" -m mlx_lm_lora.train -c "$CONFIG" \
      --train-mode "${TRAIN_MODE:-dpo}" --beta "$ORPO_BETA" \
      --adapter-path "$ADAPTER" 2>&1 | tee "$LOG"
fi
RC=${PIPESTATUS[0]}

if [ "$RC" -ne 0 ]; then
  say "training exited $RC -- see $LOG"
  exit "$RC"          # the EXIT trap restores production
fi

# The banner echoes the mode the trainer actually resolved. Assert on it rather
# than trusting the flag took -- a silently-ignored train_mode is the failure
# that produced a full "successful" SFT run instead of the ORPO we asked for.
if [ "$TRAINER" = "mlx_lm_lora" ]; then
  grep -qiE "Training Mode:.*${TRAIN_MODE:-dpo}" "$LOG" \
    || die "trainer did not resolve mode=${TRAIN_MODE:-dpo} (see $LOG)"
  say "confirmed: trainer resolved mode=${TRAIN_MODE:-dpo}, beta=$ORPO_BETA"
fi

# --- verify the artifact BEFORE spending a smoke run on it --------------------
CFG="$ADAPTER/adapter_config.json"
[ -s "$ADAPTER/adapters.safetensors" ] || die "no adapters.safetensors at $ADAPTER"
[ -s "$CFG" ] || die "no adapter_config.json at $ADAPTER"

SCALE=$("$TRAIN_PY" -c "import json;print(json.load(open('$CFG')).get('lora_parameters',{}).get('scale'))")
say "adapter_config.json lora_parameters.scale = $SCALE"
[ "$SCALE" = "2.0" ] || die "scale is $SCALE, expected 2.0 -- adapter is NOT safe to serve"

# --- THE NO-OP GUARD ----------------------------------------------------------
# LoRA computes  out = x@W + scale * (x@A)@B.  B is ZERO-INITIALISED, so if every
# lora_b is still 0.0 after training, the adapter is mathematically identical to
# the base model -- a perfect no-op that a green exit code, a moving loss curve
# and a saved .safetensors file all fail to reveal.
#
# This is not hypothetical: mlx-lm-lora 3.0.0's ORPO produced exactly that twice
# (v6 on 2026-07-27, v6.1 on 07-28), and it cost two production-dark windows and
# a wrong "under-trained" diagnosis before anyone checked the weights. Reference
# point: the working v5 adapter has 80/80 lora_b non-zero, max|B| = 2.03e-02.
say "checking the adapter actually learned something..."
"$TRAIN_PY" - "$ADAPTER/adapters.safetensors" <<'PY' || die "adapter is a NO-OP -- refusing to register or serve it"
import sys
import mlx.core as mx
w = mx.load(sys.argv[1])
b = [k for k in w if k.endswith("lora_b")]
if not b:
    print("[v6-train] FATAL: no lora_b tensors at all", file=sys.stderr); sys.exit(1)
nz = sum(1 for k in b if float(mx.abs(w[k]).max()) > 0)
mx_abs = max(float(mx.abs(w[k]).max()) for k in b)
print(f"[v6-train] lora_b non-zero {nz}/{len(b)}, max|B| = {mx_abs:.3e}")
if nz == 0:
    print("[v6-train] FATAL: every lora_b is 0.0 -> adapter == base model, "
          "nothing was learned", file=sys.stderr)
    sys.exit(1)
if nz < len(b) // 2:
    print(f"[v6-train] FATAL: only {nz}/{len(b)} lora_b are non-zero -- partial "
          "or corrupt training", file=sys.stderr)
    sys.exit(1)
PY
say "confirmed: the adapter has real weight movement"

# --- base-capability smoke, in the SAME dark window ---------------------------
# Two dark windows cost more downtime than one longer one, and the smoke test
# loads the 56 GB base itself so it cannot run beside production. run_adapter()
# frees each model before loading the next, so peak residency is one model.
#
# --base is MANDATORY: adapter_smoke_test.py's DEFAULT_BASE is the GEMMA id, and
# pairing a GLM adapter with a gemma base is the cross-family mismatch that
# manufactured a fake pre==post verdict in eval_fidelity_nightly (fixed 32d1011b4).
SMOKE_OUT=/Users/sethford/.human/logs/v6-smoke-${STAMP}.json
V5=/Users/sethford/.human/training-data/adapters/seth-glm-air-v5-20260725-093742
say "base-capability smoke: v5 (served) vs v6 (candidate)..."
"$TRAIN_PY" "$(dirname "$0")/blind_ab/adapter_smoke_test.py" \
    --base mlx-community/GLM-4.5-Air-4bit \
    --a "$V5" --b "$ADAPTER" --label-a v5 --label-b v6 \
    --out "$SMOKE_OUT" 2>&1 | tee -a "$LOG"
SMOKE_RC=${PIPESTATUS[0]}

restore_prod

say "DONE. adapter=$ADAPTER"
say "smoke  = $SMOKE_OUT (exit $SMOKE_RC)"
say "NEXT: register via scripts/adapter_registry.py. Do NOT promote without a human gate."
