#!/bin/bash
# Daily feed scrape — runs all browser automation scrapers.
# Outputs JSONL to ~/.human/feeds/ingest/ for the file_ingest feed to pick up.
#
# Usage:
#   ./scripts/daily_feed_scrape.sh
#   # Or schedule via cron: 0 5 * * * /path/to/scripts/daily_feed_scrape.sh
#
# Prerequisites:
#   pip install playwright
#   playwright install chromium

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INGEST_DIR="$HOME/.human/feeds/ingest"
mkdir -p "$INGEST_DIR"

echo "=== Daily Feed Scrape $(date) ==="

# Twitter/X PWA
echo "--- Twitter/X ---"
if python3 "$SCRIPT_DIR/scrape_twitter_feed.py" --max-tweets 50 2>&1; then
    echo "Twitter scrape complete"
else
    echo "Twitter scrape failed (may need login)"
fi

# Facebook
echo "--- Facebook ---"
if python3 "$SCRIPT_DIR/scrape_facebook_feed.py" --max-posts 30 2>&1; then
    echo "Facebook scrape complete"
else
    echo "Facebook scrape failed (may need login)"
fi

# TikTok
echo "--- TikTok ---"
if python3 "$SCRIPT_DIR/scrape_tiktok_feed.py" --max-videos 30 2>&1; then
    echo "TikTok scrape complete"
else
    echo "TikTok scrape failed (may need login)"
fi

echo "=== Done. Files in $INGEST_DIR ==="
ls -la "$INGEST_DIR"/*.jsonl 2>/dev/null || echo "(no JSONL files)"
