#!/usr/bin/env bash
# Visual regression: capture screenshots of the component catalog
# Requires: npx playwright (will be auto-installed on first run)
set -euo pipefail

cd "$(dirname "$0")/.."

SCREENSHOT_DIR="visual-regression"
mkdir -p "$SCREENSHOT_DIR"

# Start a local server for the catalog
npx vite --port 4174 &
SERVER_PID=$!
trap "kill $SERVER_PID 2>/dev/null" EXIT
sleep 3

# Capture with Playwright
npx playwright screenshot --browser chromium "http://localhost:4174/catalog.html" "$SCREENSHOT_DIR/catalog-$(date +%Y%m%d-%H%M%S).png"

echo "Screenshot saved to $SCREENSHOT_DIR/"
echo "Compare with previous screenshots to detect visual regressions."
