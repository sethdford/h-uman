#!/usr/bin/env bash
# Phase G3 (2026-05-18) — canary live-fire against production MLX.
#
# Briefly swaps a new adapter into the production MLX server, sends ONE
# chat completion, then unconditionally restores the previous adapter.
# Proves the END-TO-END production path: trained-adapter → swap →
# chat → observable difference → rollback.
#
# Safety:
#   1. Captures the current adapter BEFORE touching anything
#   2. Always restores on exit via `trap` (even on Ctrl-C / errors)
#   3. Refuses to run if --i-know-this-touches-production isn't passed
#   4. Times out the chat probe at 10s so a hung MLX doesn't strand
#      the canary
#
# Use:
#   bash scripts/live_fire_m3_canary.sh \
#        --candidate /Users/sethford/.human/training-data/adapters/X \
#        --i-know-this-touches-production
set -uo pipefail

MLX_URL="${HUMAN_MLX_URL:-http://127.0.0.1:8741}"
CANDIDATE=""
CONFIRM=0
PROMPT="${CANARY_PROMPT:-hello}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --candidate) CANDIDATE="$2"; shift 2;;
        --mlx-url) MLX_URL="$2"; shift 2;;
        --prompt) PROMPT="$2"; shift 2;;
        --i-know-this-touches-production) CONFIRM=1; shift;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done

[[ -n "$CANDIDATE" ]] || { echo "ERROR: --candidate required" >&2; exit 2; }
[[ "$CONFIRM" = "1" ]] || {
    echo "ERROR: this canary touches production MLX. Pass" >&2
    echo "       --i-know-this-touches-production to confirm." >&2
    exit 3
}

echo "═══ M3 PRODUCTION CANARY ═══"
echo "  MLX URL:    $MLX_URL"
echo "  Candidate:  $CANDIDATE"
echo "  Prompt:     $PROMPT"
echo

# Step 1: capture current adapter (rollback target).
echo "--- Step 1: capture current adapter ---"
CURRENT=$(curl -s --max-time 5 "$MLX_URL/v1/adapters/current" | \
          python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('adapter_path',''))" \
          2>/dev/null || echo "")
if [[ -z "$CURRENT" ]]; then
    echo "  WARN: could not read current adapter (server unreachable?)"
    echo "  Rollback will swap to empty string — proceed only if you're sure."
fi
echo "  Current:  ${CURRENT:-(empty)}"

# Setup rollback trap. Fires on EVERY exit path including Ctrl-C, errors,
# and normal completion. Idempotent: rolling back to the SAME adapter is a
# no-op for MLX.
rollback() {
    echo
    echo "--- ROLLBACK: restoring ${CURRENT:-(none)} ---"
    if [[ -n "$CURRENT" ]]; then
        local rb
        rb=$(curl -s --max-time 30 -X POST "$MLX_URL/v1/adapters/swap" \
             -H "Content-Type: application/json" \
             -d "{\"adapter_path\":\"$CURRENT\"}" \
             -w "%{http_code}" -o /tmp/g3-rollback-resp.json)
        echo "  rollback HTTP $rb"
        if [[ "$rb" != "200" ]]; then
            echo "  !!! ROLLBACK FAILED — manual recovery required:"
            echo "      curl -X POST $MLX_URL/v1/adapters/swap \\"
            echo "           -H 'Content-Type: application/json' \\"
            echo "           -d '{\"adapter_path\":\"$CURRENT\"}'"
        fi
    else
        echo "  (no current adapter captured — nothing to restore)"
    fi
}
trap rollback EXIT INT TERM

# Step 2: swap to candidate.
echo
echo "--- Step 2: swap to candidate ---"
SWAP_RC=$(curl -s --max-time 60 -X POST "$MLX_URL/v1/adapters/swap" \
          -H "Content-Type: application/json" \
          -d "{\"adapter_path\":\"$CANDIDATE\"}" \
          -w "%{http_code}" -o /tmp/g3-swap-resp.json)
echo "  swap HTTP $SWAP_RC"
[[ "$SWAP_RC" = "200" ]] || {
    echo "  ERROR: swap failed — see /tmp/g3-swap-resp.json"
    cat /tmp/g3-swap-resp.json 2>&1 | head -5
    exit 4  # trap will rollback (no-op since swap didn't take)
}

# Verify the swap landed.
LOADED=$(curl -s --max-time 5 "$MLX_URL/v1/adapters/current" | \
         python3 -c "import json,sys; print(json.load(sys.stdin).get('adapter_path',''))" \
         2>/dev/null || echo "")
echo "  loaded:   $LOADED"
[[ "$LOADED" = "$CANDIDATE" ]] || {
    echo "  WARN: loaded path doesn't match candidate — proceeding anyway"
}

# Step 3: send one chat completion with the new adapter.
echo
echo "--- Step 3: chat probe with candidate adapter ---"
START_MS=$(python3 -c "import time; print(int(time.time()*1000))")
HTTP_CODE=$(curl -s --max-time 30 -X POST "$MLX_URL/v1/chat/completions" \
            -H "Content-Type: application/json" \
            -d "{\"model\":\"mlx_local\",\"messages\":[{\"role\":\"user\",\"content\":\"$PROMPT\"}],\"max_tokens\":40}" \
            -o /tmp/g3-chat-resp.json -w "%{http_code}")
END_MS=$(python3 -c "import time; print(int(time.time()*1000))")
DUR_MS=$((END_MS - START_MS))
echo "  HTTP $HTTP_CODE  latency=${DUR_MS}ms"
if [[ "$HTTP_CODE" = "200" ]]; then
    python3 -c 'import json,sys; d=json.load(open("/tmp/g3-chat-resp.json")); m=d.get("choices",[{}])[0].get("message",{}); print("  response:", m.get("content",""))' 2>&1 | head -3
else
    echo "  body: $(head -c 200 /tmp/g3-chat-resp.json 2>&1)"
fi

# Trap fires here — rollback runs.
echo
echo "--- Step 4: trap restores prior adapter ---"
exit 0
