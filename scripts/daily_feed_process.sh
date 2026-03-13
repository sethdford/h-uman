#!/usr/bin/env bash
# Daily feed processor — polls all feed sources and triggers the research agent.
# Runs after daily_feed_scrape.sh has deposited JSONL files.
#
# What it does:
#   1. Polls Gmail (OAuth), iMessage (chat.db), RSS feeds, and ingests JSONL files
#   2. Triggers the research agent to analyze new items for h-uman improvements
#   3. If the research agent finds actionable improvements, creates a branch + PR
#
# Usage:
#   ./scripts/daily_feed_process.sh
#
# Automated via: ~/Library/LaunchAgents/com.human.daily-feeds.plist

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
HUMAN_DIR="$HOME/.human"
LOG_DIR="$HUMAN_DIR/logs"
LOG_FILE="$LOG_DIR/daily-feeds-$(date +%Y%m%d).log"
HUMAN_BIN="$PROJECT_DIR/build/human"

mkdir -p "$LOG_DIR"

log() { echo "[$(date '+%H:%M:%S')] $*" | tee -a "$LOG_FILE"; }

log "=== Daily Feed Process started ==="

if [ ! -x "$HUMAN_BIN" ]; then
    log "ERROR: h-uman binary not found at $HUMAN_BIN — run scripts/setup_automation.sh"
    exit 1
fi

# Poll all feed sources (Gmail, iMessage, RSS, file ingest)
log "Polling feed sources..."
if "$HUMAN_BIN" feed poll --all >> "$LOG_FILE" 2>&1; then
    log "Feed poll complete"
else
    log "WARNING: Feed poll returned non-zero (exit $?) — some sources may have failed"
fi

# Show what was ingested
log "Checking ingested items..."
"$HUMAN_BIN" feed status >> "$LOG_FILE" 2>&1 || true

# Trigger the research agent
log "Running research agent..."
if "$HUMAN_BIN" agent run --prompt research --once >> "$LOG_FILE" 2>&1; then
    log "Research agent complete"
else
    log "WARNING: Research agent returned non-zero (exit $?)"
fi

log "=== Daily Feed Process complete ==="

# Prune logs older than 30 days
find "$LOG_DIR" -name "daily-feeds-*.log" -mtime +30 -delete 2>/dev/null || true
