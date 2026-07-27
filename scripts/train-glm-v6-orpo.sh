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
CONFIG=/Users/sethford/.human/training-data/glm-v6-orpo-config.yaml
DATA_DIR=/Users/sethford/.human/training-data/glm-v6-pref
SERVICE=gui/501/ai.human.mlx-server
STAMP=$(date +%Y%m%d-%H%M%S)
ADAPTER=/Users/sethford/.human/training-data/adapters/seth-glm-air-v6-orpo-${STAMP}
LOG=/Users/sethford/.human/logs/train-glm-v6-orpo-${STAMP}.log

DRY_RUN=0
[ "${1:-}" = "--dry-run" ] && DRY_RUN=1

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
if [ "$MINS_AWAY" -lt 45 ]; then
  die "conversation-arena fires in ${MINS_AWAY} min and loads a model. Refusing to start:
       a second loader beside the 56 GB base is the documented thrash condition.
       Wait for that run to finish, then re-run this script."
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
ORPO_BETA=0.05
"$TRAIN_PY" -m mlx_lm_lora.train -c "$CONFIG" \
    --train-mode orpo --beta "$ORPO_BETA" \
    --adapter-path "$ADAPTER" 2>&1 | tee "$LOG"
RC=${PIPESTATUS[0]}

if [ "$RC" -ne 0 ]; then
  say "training exited $RC -- see $LOG"
  exit "$RC"          # the EXIT trap restores production
fi

# The banner echoes the mode the trainer actually resolved. Assert on it rather
# than trusting the flag took -- a silently-ignored train_mode is the failure
# that produced a full "successful" SFT run instead of the ORPO we asked for.
grep -qiE "Training Mode:.*ORPO" "$LOG" \
  || die "trainer did not resolve mode=ORPO (see $LOG) -- refusing to trust this run"
say "confirmed: trainer resolved mode=ORPO, beta=$ORPO_BETA"

# --- verify the artifact BEFORE spending a smoke run on it --------------------
CFG="$ADAPTER/adapter_config.json"
[ -s "$ADAPTER/adapters.safetensors" ] || die "no adapters.safetensors at $ADAPTER"
[ -s "$CFG" ] || die "no adapter_config.json at $ADAPTER"

SCALE=$("$TRAIN_PY" -c "import json;print(json.load(open('$CFG')).get('lora_parameters',{}).get('scale'))")
say "adapter_config.json lora_parameters.scale = $SCALE"
[ "$SCALE" = "2.0" ] || die "scale is $SCALE, expected 2.0 -- adapter is NOT safe to serve"

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
