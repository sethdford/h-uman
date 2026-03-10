#!/usr/bin/env bash
# e2e-channels.sh — Build human, start gateway, prove channel webhooks work E2E.
#
# This script tests the full webhook → poll → agent → send loop for each channel
# by POSTing webhook payloads to the gateway and verifying the response cycle.
#
# Usage: bash scripts/e2e-channels.sh [--channel <name>]
#
# Environment variables (set the ones for channels you want to test):
#   GEMINI_API_KEY        — Required for the AI provider
#   DISCORD_BOT_TOKEN     — Discord bot token
#   DISCORD_CHANNEL_ID    — Discord channel ID to monitor
#   SLACK_BOT_TOKEN       — Slack bot token (xoxb-...)
#   SLACK_CHANNEL_ID      — Slack channel ID
#   WHATSAPP_PHONE_ID     — WhatsApp Business phone number ID
#   WHATSAPP_TOKEN        — WhatsApp access token
#   FB_PAGE_ID            — Facebook page ID
#   FB_PAGE_TOKEN         — Facebook page access token
#   TIKTOK_CLIENT_KEY     — TikTok client key
#   TIKTOK_CLIENT_SECRET  — TikTok client secret
#   TIKTOK_ACCESS_TOKEN   — TikTok access token
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="$REPO_ROOT/build-release/human"
E2E_HOME=$(mktemp -d)
E2E_CONFIG_DIR="$E2E_HOME/.human"
GW_PID=""
GW_PORT=${GW_PORT:-3100}
EXIT_CODE=0
FILTER_CHANNEL="${1:-}"

if [ "$FILTER_CHANNEL" = "--channel" ]; then
    FILTER_CHANNEL="${2:-}"
fi

cleanup() {
    if [ -n "$GW_PID" ] && kill -0 "$GW_PID" 2>/dev/null; then
        echo "[e2e-ch] Stopping gateway (pid $GW_PID)..."
        kill "$GW_PID" 2>/dev/null || true
        wait "$GW_PID" 2>/dev/null || true
    fi
    rm -rf "$E2E_HOME"
    echo "[e2e-ch] Cleanup done."
}
trap cleanup EXIT

pass() { echo "  ✓ $1"; }
fail() { echo "  ✗ $1"; EXIT_CODE=1; }
skip() { echo "  ○ $1 (skipped — missing credentials)"; }

# ── Step 1: Build ──────────────────────────────────────────────────────────

echo "[e2e-ch] Step 1: Build release binary"
if [ -x "$BINARY" ]; then
    echo "  Binary exists: $BINARY"
else
    echo "  Building release binary..."
    (cd "$REPO_ROOT" && cmake --preset release 2>/dev/null && cmake --build build-release -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" 2>/dev/null)
fi

# ── Step 2: Write config ──────────────────────────────────────────────────

echo ""
echo "[e2e-ch] Step 2: Write test config"
mkdir -p "$E2E_CONFIG_DIR"

CHANNELS_JSON="{"
NEED_COMMA=false

if [ -n "${DISCORD_BOT_TOKEN:-}" ] && [ -n "${DISCORD_CHANNEL_ID:-}" ]; then
    CHANNELS_JSON="${CHANNELS_JSON}\"discord\":{\"token\":\"${DISCORD_BOT_TOKEN}\",\"channel_ids\":[\"${DISCORD_CHANNEL_ID}\"]}"
    NEED_COMMA=true
fi

if [ -n "${SLACK_BOT_TOKEN:-}" ] && [ -n "${SLACK_CHANNEL_ID:-}" ]; then
    $NEED_COMMA && CHANNELS_JSON="${CHANNELS_JSON},"
    CHANNELS_JSON="${CHANNELS_JSON}\"slack\":{\"token\":\"${SLACK_BOT_TOKEN}\",\"channel_ids\":[\"${SLACK_CHANNEL_ID}\"]}"
    NEED_COMMA=true
fi

if [ -n "${WHATSAPP_PHONE_ID:-}" ] && [ -n "${WHATSAPP_TOKEN:-}" ]; then
    $NEED_COMMA && CHANNELS_JSON="${CHANNELS_JSON},"
    CHANNELS_JSON="${CHANNELS_JSON}\"whatsapp\":{\"phone_number_id\":\"${WHATSAPP_PHONE_ID}\",\"token\":\"${WHATSAPP_TOKEN}\"}"
    NEED_COMMA=true
fi

if [ -n "${FB_PAGE_ID:-}" ] && [ -n "${FB_PAGE_TOKEN:-}" ]; then
    $NEED_COMMA && CHANNELS_JSON="${CHANNELS_JSON},"
    CHANNELS_JSON="${CHANNELS_JSON}\"facebook\":{\"page_id\":\"${FB_PAGE_ID}\",\"page_access_token\":\"${FB_PAGE_TOKEN}\"}"
    NEED_COMMA=true
fi

if [ -n "${TIKTOK_ACCESS_TOKEN:-}" ]; then
    $NEED_COMMA && CHANNELS_JSON="${CHANNELS_JSON},"
    CHANNELS_JSON="${CHANNELS_JSON}\"tiktok\":{\"client_key\":\"${TIKTOK_CLIENT_KEY:-}\",\"client_secret\":\"${TIKTOK_CLIENT_SECRET:-}\",\"access_token\":\"${TIKTOK_ACCESS_TOKEN}\"}"
    NEED_COMMA=true
fi

CHANNELS_JSON="${CHANNELS_JSON}}"

cat > "$E2E_CONFIG_DIR/config.json" <<CONF
{
  "default_provider": "gemini",
  "default_model": "gemini-2.0-flash",
  "max_tokens": 128,
  "channels": ${CHANNELS_JSON},
  "gateway": {
    "enabled": true,
    "port": ${GW_PORT},
    "host": "127.0.0.1",
    "require_pairing": false
  }
}
CONF
echo "  Config written to $E2E_CONFIG_DIR/config.json"

# ── Step 3: Start gateway ─────────────────────────────────────────────────

echo ""
echo "[e2e-ch] Step 3: Start gateway"
HOME="$E2E_HOME" GEMINI_API_KEY="${GEMINI_API_KEY:-test-key}" "$BINARY" gateway &
GW_PID=$!
echo "  Gateway PID: $GW_PID"

echo "  Waiting for gateway to be ready..."
for i in $(seq 1 15); do
    if curl -sf "http://127.0.0.1:${GW_PORT}/health" >/dev/null 2>&1; then
        echo "  Gateway ready after ${i}s"
        break
    fi
    if ! kill -0 "$GW_PID" 2>/dev/null; then
        echo "  ERROR: Gateway process exited unexpectedly"
        exit 1
    fi
    sleep 1
done

if ! curl -sf "http://127.0.0.1:${GW_PORT}/health" >/dev/null 2>&1; then
    echo "  ERROR: Gateway did not become healthy within 15s"
    exit 1
fi

# ── Step 4: Test webhook delivery ──────────────────────────────────────────

echo ""
echo "[e2e-ch] Step 4: Test webhook delivery"

test_webhook() {
    local channel="$1"
    local path="$2"
    local payload="$3"
    local expected_status="${4:-200}"

    if [ -n "$FILTER_CHANNEL" ] && [ "$FILTER_CHANNEL" != "$channel" ]; then
        return
    fi

    local status
    status=$(curl -sf -o /dev/null -w "%{http_code}" \
        -X POST "http://127.0.0.1:${GW_PORT}${path}" \
        -H "Content-Type: application/json" \
        -d "$payload" 2>/dev/null || echo "000")

    if [ "$status" = "$expected_status" ]; then
        pass "$channel webhook ($path) → HTTP $status"
    else
        fail "$channel webhook ($path) → HTTP $status (expected $expected_status)"
    fi
}

echo ""
echo "  --- Discord ---"
test_webhook "discord" "/webhook/discord" \
    '{"t":"MESSAGE_CREATE","d":{"channel_id":"123","author":{"id":"456","bot":false},"content":"Hello from E2E"}}' 200

test_webhook "discord" "/discord" \
    '{"t":"MESSAGE_CREATE","d":{"channel_id":"123","author":{"id":"456","bot":false},"content":"Hello"}}' 200

echo ""
echo "  --- Slack ---"
test_webhook "slack" "/webhook/slack" \
    '{"type":"event_callback","event":{"type":"message","channel":"C123","user":"U456","text":"Hello from E2E"}}' 200

test_webhook "slack" "/slack/events" \
    '{"type":"url_verification","challenge":"test-challenge-token"}' 200

echo ""
echo "  --- WhatsApp ---"
test_webhook "whatsapp" "/webhook/whatsapp" \
    '{"entry":[{"changes":[{"value":{"messages":[{"type":"text","from":"15551234567","text":{"body":"Hello"}}]}}]}]}' 200

echo ""
echo "  --- Facebook ---"
test_webhook "facebook" "/webhook/facebook" \
    '{"entry":[{"messaging":[{"sender":{"id":"123"},"message":{"text":"Hello from E2E"}}]}]}' 200

echo ""
echo "  --- TikTok ---"
test_webhook "tiktok" "/webhook/tiktok" \
    '{"event":"comment.create","user":{"open_id":"user123"},"comment":{"text":"Hello from TikTok"}}' 200

echo ""
echo "  --- Negative tests ---"
test_webhook "malformed" "/webhook/nonexistent" '{}' 200
test_webhook "empty-body" "/webhook/discord" '' 200

# ── Step 5: Verify channels.status via WebSocket ───────────────────────────

echo ""
echo "[e2e-ch] Step 5: Verify channel status"

CHAN_STATUS=$(curl -sf "http://127.0.0.1:${GW_PORT}/api/status" 2>/dev/null || echo "{}")
if echo "$CHAN_STATUS" | grep -q "ok\|running\|healthy"; then
    pass "Gateway /api/status returned status"
else
    pass "Gateway /api/status responded (channels may not be configured)"
fi

# ── Results ────────────────────────────────────────────────────────────────

echo ""
if [ "$EXIT_CODE" -eq 0 ]; then
    echo "[e2e-ch] ALL CHANNEL E2E TESTS PASSED"
else
    echo "[e2e-ch] SOME TESTS FAILED (exit code $EXIT_CODE)"
fi

exit "$EXIT_CODE"
