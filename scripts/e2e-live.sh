#!/usr/bin/env bash
# e2e-live.sh — Build human, start gateway, prove the full stack works via Playwright.
#
# Usage: GEMINI_API_KEY=... bash scripts/e2e-live.sh
#
# Requirements:
#   - GEMINI_API_KEY environment variable set
#   - cmake, node, npm installed
#   - Ports 3000 free
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="$REPO_ROOT/build-release/human"
UI_DIR="$REPO_ROOT/ui"
UI_DIST="$UI_DIR/dist"
E2E_HOME=$(mktemp -d)
E2E_CONFIG_DIR="$E2E_HOME/.human"
GW_PID=""
EXIT_CODE=0

cleanup() {
    if [ -n "$GW_PID" ] && kill -0 "$GW_PID" 2>/dev/null; then
        echo "[e2e] Stopping gateway (pid $GW_PID)..."
        kill "$GW_PID" 2>/dev/null || true
        wait "$GW_PID" 2>/dev/null || true
    fi
    rm -rf "$E2E_HOME"
    echo "[e2e] Cleanup done."
}
trap cleanup EXIT

if [ -z "${GEMINI_API_KEY:-}" ]; then
    echo "Error: GEMINI_API_KEY is not set."
    echo "Usage: GEMINI_API_KEY=your-key bash scripts/e2e-live.sh"
    exit 1
fi

echo "[e2e] Step 1: Build release binary"
if [ -x "$BINARY" ]; then
    echo "  Binary exists: $BINARY ($(stat -f%z "$BINARY" 2>/dev/null || stat -c%s "$BINARY") bytes)"
else
    (cd "$REPO_ROOT" && cmake --preset release && cmake --build build-release -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)")
fi
echo "  $("$BINARY" --version 2>&1 || echo 'version check failed')"

echo ""
echo "[e2e] Step 2: Build UI"
(cd "$UI_DIR" && npm run build --silent)
echo "  UI built: $UI_DIST"

echo ""
echo "[e2e] Step 3: Write test config"
mkdir -p "$E2E_CONFIG_DIR"
cat > "$E2E_CONFIG_DIR/config.json" <<CONF
{
  "default_provider": "gemini",
  "default_model": "gemini-2.0-flash",
  "max_tokens": 256,
  "memory": { "backend": "sqlite" },
  "gateway": {
    "enabled": true,
    "port": 3000,
    "host": "127.0.0.1",
    "control_ui_dir": "$UI_DIST",
    "require_pairing": false
  }
}
CONF
echo "  Config: $E2E_CONFIG_DIR/config.json"

echo ""
echo "[e2e] Step 4: Start gateway"
HOME="$E2E_HOME" GEMINI_API_KEY="$GEMINI_API_KEY" "$BINARY" gateway --with-agent &
GW_PID=$!
echo "  Gateway PID: $GW_PID"

echo "  Waiting for gateway to be ready..."
for i in $(seq 1 20); do
    if curl -sf http://127.0.0.1:3000/health >/dev/null 2>&1; then
        echo "  Gateway ready after ${i}s"
        break
    fi
    if ! kill -0 "$GW_PID" 2>/dev/null; then
        echo "  ERROR: Gateway process exited unexpectedly"
        exit 1
    fi
    sleep 1
done

if ! curl -sf http://127.0.0.1:3000/health >/dev/null 2>&1; then
    echo "  ERROR: Gateway did not become healthy within 20s"
    exit 1
fi

echo ""
echo "[e2e] Step 5: Run Playwright E2E tests"
mkdir -p "$UI_DIR/e2e-results"
(
    cd "$UI_DIR"
    HUMAN_LIVE_E2E=1 npx playwright test e2e/live-gateway.spec.ts \
        --config=playwright-live.config.ts 2>&1
) || EXIT_CODE=$?

echo ""
if [ "$EXIT_CODE" -eq 0 ]; then
    echo "[e2e] ALL TESTS PASSED"
    if [ -f "$UI_DIR/e2e-results/live-gateway-chat.png" ]; then
        echo "  Screenshot: $UI_DIR/e2e-results/live-gateway-chat.png"
    fi
else
    echo "[e2e] TESTS FAILED (exit code $EXIT_CODE)"
fi

exit "$EXIT_CODE"
