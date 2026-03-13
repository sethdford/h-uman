#!/usr/bin/env bash
# Daily feed scrape — runs all browser automation scrapers.
# Outputs JSONL to ~/.human/feeds/ingest/ for the file_ingest feed to pick up.
#
# Usage:
#   ./scripts/daily_feed_scrape.sh           # run all scrapers
#   ./scripts/daily_feed_scrape.sh twitter   # run only twitter
#
# Automated via: ~/Library/LaunchAgents/com.human.daily-scrape.plist

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENV_PYTHON="$HOME/.human/scraper-venv/bin/python3"
INGEST_DIR="$HOME/.human/feeds/ingest"
LOG_DIR="$HOME/.human/logs"
LOG_FILE="$LOG_DIR/daily-scrape-$(date +%Y%m%d).log"

mkdir -p "$INGEST_DIR" "$LOG_DIR"

log() { echo "[$(date '+%H:%M:%S')] $*" | tee -a "$LOG_FILE"; }

if [ ! -x "$VENV_PYTHON" ]; then
    log "ERROR: venv not found at $VENV_PYTHON — run scripts/setup_automation.sh first"
    exit 1
fi

FILTER="${1:-all}"

log "=== Daily Feed Scrape started ==="

run_scraper() {
    local name="$1" script="$2"; shift 2
    if [ "$FILTER" != "all" ] && [ "$FILTER" != "$name" ]; then return 0; fi
    log "--- $name ---"
    if "$VENV_PYTHON" "$SCRIPT_DIR/$script" "$@" >> "$LOG_FILE" 2>&1; then
        log "$name scrape complete"
    else
        log "WARNING: $name scrape failed (exit $?) — may need re-login"
    fi
}

run_scraper twitter  scrape_twitter_feed.py  --max-tweets 50
run_scraper facebook scrape_facebook_feed.py --max-posts 30
run_scraper tiktok   scrape_tiktok_feed.py   --max-videos 30

log "=== Done. Files in $INGEST_DIR ==="
ls -la "$INGEST_DIR"/*.jsonl 2>/dev/null | tee -a "$LOG_FILE" || log "(no JSONL files)"

# Prune logs older than 30 days
find "$LOG_DIR" -name "daily-scrape-*.log" -mtime +30 -delete 2>/dev/null || true
