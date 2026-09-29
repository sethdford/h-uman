#!/usr/bin/env bash
# Generate the seth-glm-air-v6 candidate arm for a human cycle-5 blind A/B sheet.
#
# Runs in its OWN short dark window, deliberately not chained onto the training
# run: generation is only worth doing if training produced an adapter worth
# rating, so chaining would spend production downtime on adapters we then reject.
#
# The arm is generated through a SERVED endpoint, not in-process mlx_lm, because
# GLM emits <think> blocks and the only thing that strips them is
# _thinking_suppressed_value() in mlx-server.py. Generating in-process would put
# raw deliberation in front of raters and score v6 for a bug it does not have.
#
# :8741 is never repointed. v6 is served on a spare port and torn down after.
#
# Usage: scripts/gen-v6-arm.sh <adapter-path> [--n 16]
set -uo pipefail

ADAPTER=${1:-}
[ -n "$ADAPTER" ] || { echo "usage: $0 <adapter-path>" >&2; exit 2; }

RUN_DIR=/Users/sethford/blind_ab_run/cycle5-$(date +%Y%m%d)
HEAD="$RUN_DIR/head_live.txt"
CONTEXTS="$RUN_DIR/contexts_c5.json"
OUT="$RUN_DIR/triples_v6.json"
PORT=8747
SERVICE=gui/501/ai.human.mlx-server
BASE=mlx-community/GLM-4.5-Air-4bit
SERVER_PY=/Users/sethford/Documents/gemma-realtime-1/scripts/mlx-server.py
SERVE_PY=/Users/sethford/Documents/gemma-realtime-1/.venv312/bin/python
SERVER_LOG="$RUN_DIR/mlx-${PORT}-v6.log"

say() { printf '[v6-arm] %s\n' "$*"; }
die() { printf '[v6-arm] FATAL: %s\n' "$*" >&2; exit 1; }

# --- preflight ----------------------------------------------------------------
[ -s "$ADAPTER/adapters.safetensors" ] || die "no adapter at $ADAPTER"
[ -s "$HEAD" ]     || die "missing live prompt head: $HEAD"
[ -s "$CONTEXTS" ] || die "missing contexts: $CONTEXTS (run select_cycle5_contexts.py)"
[ -x "$SERVE_PY" ] || die "serving venv missing: $SERVE_PY"

SCALE=$(python3 -c "import json;print(json.load(open('$ADAPTER/adapter_config.json')).get('lora_parameters',{}).get('scale'))" 2>/dev/null)
[ "$SCALE" = "2.0" ] || die "adapter scale is $SCALE, expected 2.0 -- refusing to serve it"

read -r NEXT_ARENA MINS_AWAY <<<"$(date '+%H %M' | awk '{
  h=$1+0; m=$2+0; now=h*60+m; best=-1;
  for (i=0;i<6;i++){ s=(2+4*i)*60+23; if (s>now && best<0) best=s }
  if (best<0) best=(2*60+23)+24*60;
  printf "%02d:23 %d", int(best/60)%24, best-now }')"
say "next conversation-arena slot ${NEXT_ARENA} (${MINS_AWAY} min away)"
[ "$MINS_AWAY" -lt 30 ] && die "arena fires in ${MINS_AWAY} min; wait for it to finish"

lsof -nP -iTCP:${PORT} -sTCP:LISTEN >/dev/null 2>&1 && die "port ${PORT} already in use -- find its owner before reusing it"

# --- teardown trap: v6 server dies and prod comes back, however we exit --------
V6_PID=""
PROD_RESTORED=0
cleanup() {
  [ -n "$V6_PID" ] && kill "$V6_PID" 2>/dev/null && say "stopped v6 server (pid $V6_PID)"
  V6_PID=""
  [ "$PROD_RESTORED" = "1" ] && return 0
  PROD_RESTORED=1
  say "restoring production mlx-server (unchanged v5 config)..."
  # bootstrap first (we booted it out); kickstart only as fallback. And require
  # the launchd JOB to be loaded, not just the port open -- a listening socket
  # alone let prod fall out of launchd unnoticed on 2026-07-27.
  launchctl bootstrap gui/501 ~/Library/LaunchAgents/ai.human.mlx-server.plist 2>/dev/null \
    || launchctl kickstart -k "$SERVICE" 2>/dev/null
  for _ in $(seq 1 60); do
    if lsof -nP -iTCP:8741 -sTCP:LISTEN >/dev/null 2>&1 \
       && launchctl list "${SERVICE##*/}" >/dev/null 2>&1; then
      say "prod listening on :8741 and job loaded"; return 0
    fi
    sleep 5
  done
  say "WARNING: prod not fully restored. Recover with:"
  say "  launchctl bootstrap gui/501 ~/Library/LaunchAgents/ai.human.mlx-server.plist"
}
trap cleanup EXIT INT TERM

# --- free the memory ----------------------------------------------------------
PROD_PID=$(lsof -tnP -iTCP:8741 -sTCP:LISTEN 2>/dev/null | head -1)
say "stopping production (pid ${PROD_PID:-none})..."
launchctl bootout "$SERVICE" 2>/dev/null
for _ in $(seq 1 60); do
  { [ -z "$PROD_PID" ] || ! kill -0 "$PROD_PID" 2>/dev/null; } && break
  sleep 2
done
[ -n "$PROD_PID" ] && kill -0 "$PROD_PID" 2>/dev/null && die "prod pid $PROD_PID did not reap"

# --- serve v6 on the spare port -----------------------------------------------
# GEMMA_DISABLE_THINKING=1 mirrors the production launchd environment; without it
# the <think>-suppression polarity differs from what prod actually serves.
say "serving v6 on :${PORT} ..."
GEMMA_DISABLE_THINKING=1 "$SERVE_PY" "$SERVER_PY" \
    --model "$BASE" --port "$PORT" --adapter-path "$ADAPTER" \
    >"$SERVER_LOG" 2>&1 &
V6_PID=$!

for _ in $(seq 1 120); do
  curl -s -m 3 -o /dev/null "http://127.0.0.1:${PORT}/v1/models" && break
  kill -0 "$V6_PID" 2>/dev/null || die "v6 server died while loading -- see $SERVER_LOG"
  sleep 5
done
curl -s -m 5 -o /dev/null "http://127.0.0.1:${PORT}/v1/models" \
  || die "v6 server never became ready -- see $SERVER_LOG"
say "v6 server ready (pid $V6_PID)"

# --- base-capability probe, same served session -------------------------------
# Reuses the server we already stood up rather than calling adapter_smoke_test.py,
# which would load the 56 GB base a second time and double the dark window.
# Catches the ORPO over-correction mode (blank/degenerate/truncated output).
PROBE_OUT="$RUN_DIR/capability_v6.json"
say "base-capability probe..."
python3 "$(dirname "$0")/blind_ab/capability_probe_http.py" \
    --endpoint "http://127.0.0.1:${PORT}/v1/chat/completions" \
    --model "$BASE" --out "$PROBE_OUT"
PROBE_RC=$?
if [ "$PROBE_RC" -ne 0 ]; then
  say "capability probe FAILED -- generating the arm anyway would ask humans to"
  say "rate a broken adapter. Stopping; see $PROBE_OUT"
  exit 1        # trap tears down v6 and restores prod
fi

# --- generate -----------------------------------------------------------------
# --prompt is the LIVE production head captured before training, so the arm is
# generated under the same prompt config the pending cycle-4 sheet measured.
say "generating $(python3 -c "import json;print(len(json.load(open('$CONTEXTS'))))") replies..."
python3 /Users/sethford/blind_ab_run/gen_direct.py "$CONTEXTS" \
    --endpoint "http://127.0.0.1:${PORT}/v1/chat/completions" \
    --prompt "$HEAD" --model "$BASE" --mode generic \
    --out "$OUT" 2>&1 | tail -20
GEN_RC=${PIPESTATUS[0]}

cleanup    # v6 server down, prod back up, before we do anything else

[ "$GEN_RC" -eq 0 ] || die "generation exited $GEN_RC"

# --- refuse a partial arm -----------------------------------------------------
# A short arm that self-certifies against its own length is the failure recorded
# in .claude/rules/no-number-without-a-measurement.md. Check against the SOURCE.
python3 - "$CONTEXTS" "$OUT" <<'PY' || exit 1
import json, sys
want = json.load(open(sys.argv[1]))
got  = json.load(open(sys.argv[2]))
have = {t["id"]: (t.get("huuman_reply") or "").strip() for t in got}
missing = [c["id"] for c in want if not have.get(c["id"])]
print(f"[v6-arm] generated {len(want)-len(missing)}/{len(want)}")
if missing:
    print(f"[v6-arm] FATAL: {len(missing)} empty replies: {missing}", file=sys.stderr)
    sys.exit(1)
PY

say "DONE. arm=$OUT"
say "NEXT: python3 scripts/blind_ab/make_rating_sheet.py $OUT --out-dir $RUN_DIR"
say "      answer_key.json is PRIVATE -- never send it to a rater."
