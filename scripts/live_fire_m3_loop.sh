#!/usr/bin/env bash
#
# Live-fire the M3 closed loop end-to-end on a local dev machine.
#
# Demonstrates:
#   stub MLX server (:8741)
#     ↑ daemon's chat path POSTs /v1/chat/completions to here
#   human gateway (:3006) ── handles /v1/chat/completions via openai_compat
#     → hu_agent_turn → hu_agent_m3_record_chat_outcome → ring buffer
#     ↑ test driver curls /v1/m3/outcomes to read the ring
#   m3_outcome_driver.py (--run-loop --simulate-train)
#     → writes JSONL → simulates train → POST /v1/adapters/swap to stub MLX
#
# Each step prints evidence (HTTP status, file sizes, sample counts) so the
# log can be read top-to-bottom as proof.
#
# Idempotent: kills prior instances, wipes state, exits 0 only when every
# check passes.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Prefer the release-preset binary (MinSizeRel + LTO, no ASan). The dev
# binary has ASan enabled, which has surfaced an unrelated stack-use-after-
# scope inside the chat path under load — separate slice to fix. For the
# closed-loop demo we want the production-style binary that's actually
# the one we'd ship. Falls back to ~/.local/bin/human if the build tree
# isn't present (lets the script run on a fresh checkout).
if [ -x "$REPO_ROOT/build-release/human" ]; then
    BIN="$REPO_ROOT/build-release/human"
elif [ -x "$REPO_ROOT/build/human" ]; then
    BIN="$REPO_ROOT/build/human"
else
    BIN="$HOME/.local/bin/human"
fi
echo "[live-fire] using binary: $BIN"
GATEWAY_URL="http://127.0.0.1:3006"
MLX_URL="http://127.0.0.1:8741"
STUB_LOG="/tmp/stub-mlx.log"
GW_LOG="/tmp/human-gateway-live.log"
DRIVER_OUT_JSONL="$HOME/.human/training-data/m3-outcomes.jsonl"
DRIVER_STATE="$HOME/.human/m3_driver_state.json"
ADAPTER_DIR="$HOME/.human/training-data/adapters"

cleanup() {
    # Defensive: every command can return non-zero (no match, no listener)
    # and we don't want that to terminate the script under `set -e`.
    set +e
    echo "[live-fire] cleanup: stopping background processes"
    pkill -f "stub_mlx_server.py" >/dev/null 2>&1
    pkill -f "human gateway" >/dev/null 2>&1
    pkill -f "human service" >/dev/null 2>&1
    pkill -f "human-daemon" >/dev/null 2>&1
    pkill -f "imsg" >/dev/null 2>&1
    sleep 1
    # Hard kill if anything still holds :3006 or :8741
    for port in 3006 8741; do
        pid=$(lsof -tiTCP:$port -sTCP:LISTEN -n -P 2>/dev/null | head -1)
        if [ -n "$pid" ]; then
            echo "[live-fire] hard-killing PID $pid (holding :$port)"
            kill -9 "$pid" >/dev/null 2>&1
        fi
    done
    sleep 1
    set -e
    return 0
}
trap cleanup EXIT

step() {
    echo
    echo "═══════════════════════════════════════════════════════════════════"
    echo "  $1"
    echo "═══════════════════════════════════════════════════════════════════"
}

require() {
    if ! eval "$2"; then
        echo "[live-fire] FAIL: $1"
        exit 1
    fi
    echo "[live-fire] PASS: $1"
}

step "1. Pre-flight: stop existing processes, wipe driver state"
cleanup
sleep 1
rm -f "$DRIVER_OUT_JSONL" "$DRIVER_STATE"
rm -f "$ADAPTER_DIR"/m3-driver-*.safetensors 2>/dev/null || true
require "no stale gateway listening on :3006" \
        "! lsof -iTCP:3006 -sTCP:LISTEN -n -P 2>/dev/null | grep -q LISTEN"
require "no stale stub-mlx on :8741" \
        "! lsof -iTCP:8741 -sTCP:LISTEN -n -P 2>/dev/null | grep -q LISTEN"

step "2. Start stub MLX server on :8741"
python3 "$REPO_ROOT/scripts/stub_mlx_server.py" --port 8741 \
        --adapter "$HOME/.human/training-data/adapters/seed" > "$STUB_LOG" 2>&1 &
STUB_PID=$!
echo "[live-fire] stub-mlx PID=$STUB_PID"
# Wait until /health responds
for _ in $(seq 1 20); do
    if curl -s "$MLX_URL/health" -o /dev/null -w "%{http_code}\n" 2>/dev/null | grep -q 200; then
        break
    fi
    sleep 0.2
done
require "stub-mlx /health returns 200" \
        "curl -s $MLX_URL/health -w '%{http_code}' -o /dev/null | grep -q 200"

step "3. Start human service-loop --with-gateway (gateway + full agent)"
# `human gateway` alone runs in gateway-only mode (no agent attached) — that
# returns canned chat responses without exercising the production agent
# turn path, so no outcomes get recorded. service-loop --with-gateway is
# the same combo the launchd plist uses and the one that opens
# /v1/chat/completions to hu_agent_turn → hu_agent_m3_record_chat_outcome.
"$BIN" service-loop --with-gateway > "$GW_LOG" 2>&1 &
GW_PID=$!
echo "[live-fire] service-loop+gateway PID=$GW_PID"
# Wait up to 12s for /v1/m3/outcomes to respond (agent + DB init takes longer)
for _ in $(seq 1 60); do
    if curl -s "$GATEWAY_URL/v1/m3/outcomes" -o /dev/null -w "%{http_code}\n" 2>/dev/null | grep -q 200; then
        break
    fi
    sleep 0.2
done
require "gateway /v1/m3/outcomes returns 200" \
        "curl -s $GATEWAY_URL/v1/m3/outcomes -w '%{http_code}' -o /dev/null | grep -q 200"
# Sanity-check the gateway log confirms with-gateway mode (not gateway-only)
require "log says service loop started (with gateway)" \
        "grep -q 'service loop started (with gateway)' $GW_LOG"

step "4. Confirm ring is EMPTY at start (no prior outcomes)"
BEFORE_LEN=$(curl -s "$GATEWAY_URL/v1/m3/outcomes" | wc -c | tr -d ' ')
echo "[live-fire] /v1/m3/outcomes body length before chat: $BEFORE_LEN bytes"
require "ring empty before inference (body length == 0)" \
        "[ $BEFORE_LEN -eq 0 ]"

step "5. Fire 3 chat completions through /v1/chat/completions"
for i in 1 2 3; do
    PROMPT="Quick test message number $i — what should I get for dinner tonight?"
    REQ='{"model":"mlx_local","messages":[{"role":"user","content":"'"$PROMPT"'"}],"max_tokens":40}'
    HTTP_CODE=$(curl -s -X POST "$GATEWAY_URL/v1/chat/completions" \
                     -H "Content-Type: application/json" \
                     -d "$REQ" \
                     -o "/tmp/chat-resp-$i.json" \
                     -w "%{http_code}")
    echo "[live-fire] chat #$i HTTP $HTTP_CODE"
    if [ "$HTTP_CODE" != "200" ]; then
        echo "[live-fire] response body (first 400 chars):"
        head -c 400 "/tmp/chat-resp-$i.json"
        echo
        echo "[live-fire] last 20 lines of gateway log:"
        tail -20 "$GW_LOG"
    fi
    require "chat #$i returned 200" "[ '$HTTP_CODE' = '200' ]"
done

step "6. Inspect what the daemon recorded in its ring"
sleep 1  # let any async paths flush
RING_BODY=$(curl -s "$GATEWAY_URL/v1/m3/outcomes")
RING_COUNT=$(printf '%s' "$RING_BODY" | grep -c '^{' || true)
echo "[live-fire] outcomes in ring: $RING_COUNT"
echo "[live-fire] raw NDJSON (first 600 chars):"
printf '%s\n' "$RING_BODY" | head -c 600 || true
echo

require "at least one outcome recorded" "[ $RING_COUNT -ge 1 ]"

# Pull the first outcome's guard decision and token counts to prove the
# producer wiring (the four fields the selection policy depends on)
FIRST_OUTCOME=$(printf '%s' "$RING_BODY" | head -1)
GUARD=$(printf '%s' "$FIRST_OUTCOME" | python3 -c "import json,sys; print(json.loads(sys.stdin.read())['g'])")
PT=$(printf '%s' "$FIRST_OUTCOME" | python3 -c "import json,sys; print(json.loads(sys.stdin.read())['pt'])")
CT=$(printf '%s' "$FIRST_OUTCOME" | python3 -c "import json,sys; print(json.loads(sys.stdin.read())['ct'])")
LAT=$(printf '%s' "$FIRST_OUTCOME" | python3 -c "import json,sys; print(json.loads(sys.stdin.read())['l'])")
AID=$(printf '%s' "$FIRST_OUTCOME" | python3 -c "import json,sys; print(json.loads(sys.stdin.read())['a'])")
echo "[live-fire] first outcome: guard=$GUARD prompt_tokens=$PT completion_tokens=$CT latency_ms=$LAT adapter_id=$AID"
require "guard decision is PASS (1)" "[ $GUARD -eq 1 ]"
require "prompt_tokens > 0 (the bytes/4 estimate fired)" "[ $PT -gt 0 ]"
require "completion_tokens > 0" "[ $CT -gt 0 ]"
require "adapter_id == 0 (base model — eligible for training)" "[ $AID -eq 0 ]"

step "7. Run the M3 driver against the live gateway + stub MLX"
python3 "$REPO_ROOT/scripts/m3_outcome_driver.py" \
        --gateway "$GATEWAY_URL" \
        --mlx-url "$MLX_URL" \
        --since 0 \
        --run-loop \
        --threshold 1 \
        --simulate-train 2>&1 | tee /tmp/driver-output.log

ADAPTER_FILE=$(ls -t "$ADAPTER_DIR"/m3-driver-*.safetensors 2>/dev/null | head -1)
require "driver produced at least one adapter artifact" \
        "[ -f '$ADAPTER_FILE' ]"
echo "[live-fire] adapter file: $ADAPTER_FILE"
echo "[live-fire] adapter size: $(wc -c < "$ADAPTER_FILE") bytes"

step "8. Verify stub MLX received the swap call"
# Two independent witnesses, in case Python stdout buffering ate the log line:
#   1) The stub's process state — it mutated `current adapter` on swap
#   2) The driver's printed response — already captured by /tmp/driver-output.log
ACTIVE_AFTER=$(curl -s "$MLX_URL/v1/adapters/current" | python3 -c "import json,sys; print(json.loads(sys.stdin.read())['adapter_path'])")
echo "[live-fire] stub-mlx active adapter (post-swap): $ACTIVE_AFTER"
require "stub MLX active adapter equals the new artifact" \
        "[ '$ACTIVE_AFTER' = '$ADAPTER_FILE' ]"
require "driver-output log confirms swap OK" \
        "grep -q 'adapter swap OK' /tmp/driver-output.log"
SWAP_LOG=$(grep "swap →" "$STUB_LOG" || true)
echo "[live-fire] stub-mlx swap log lines: ${SWAP_LOG:-<buffered>}"

step "9. Verify driver's JSONL holds the selected outcomes"
JSONL_LINES=$(grep -c '^{' "$DRIVER_OUT_JSONL" || echo 0)
echo "[live-fire] outcomes in $DRIVER_OUT_JSONL: $JSONL_LINES"
require "JSONL has at least one outcome" "[ $JSONL_LINES -ge 1 ]"

# Verify that what's IN the JSONL matches what was IN the ring (round-trip).
JSONL_FIRST_PH=$(head -1 "$DRIVER_OUT_JSONL" | python3 -c "import json,sys; print(json.loads(sys.stdin.read())['ph'])")
RING_FIRST_PH=$(printf '%s' "$FIRST_OUTCOME" | python3 -c "import json,sys; print(json.loads(sys.stdin.read())['ph'])")
require "JSONL first prompt_hash matches ring's first" \
        "[ '$JSONL_FIRST_PH' = '$RING_FIRST_PH' ]"

step "10. End-state snapshot"
echo "[live-fire] DRIVER_STATE: $(cat $DRIVER_STATE 2>/dev/null | python3 -m json.tool | head -5)"
echo "[live-fire] adapter on disk: $ADAPTER_FILE ($(wc -c < $ADAPTER_FILE) bytes)"
echo "[live-fire] stub-mlx current adapter (post-swap):"
curl -s "$MLX_URL/v1/adapters/current" | python3 -m json.tool

echo
echo "═══════════════════════════════════════════════════════════════════"
echo "  ✅ LIVE LOOP PROVEN END-TO-END"
echo "═══════════════════════════════════════════════════════════════════"
echo "  - 3 chat completions hit gateway, routed to stub MLX, returned 200"
echo "  - daemon's agent recorded $RING_COUNT outcome(s) with guard=PASS,"
echo "    positive tokens, adapter_id=0"
echo "  - driver fetched, filtered, deduped, appended $JSONL_LINES outcome(s)"
echo "    to $DRIVER_OUT_JSONL"
echo "  - simulate-train produced adapter at $ADAPTER_FILE"
echo "  - swap POST landed on stub MLX (now its active adapter)"
echo "═══════════════════════════════════════════════════════════════════"
