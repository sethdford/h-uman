#!/usr/bin/env bash
# One-time setup for daily feed monitoring automation.
#
# What this does:
#   1. Creates ~/.human directory structure
#   2. Sets up Python venv with playwright + chromium
#   3. Builds h-uman with feed support
#   4. Installs macOS launchd agents for daily scraping + feed processing
#   5. Opens browsers for initial login to Twitter, Facebook, TikTok
#
# Usage:
#   ./scripts/setup_automation.sh          # full setup
#   ./scripts/setup_automation.sh --skip-login  # skip browser login step
#
# After running, log into each browser window that opens.
# Browser state is saved to ~/.human/browser_state/ for headless reuse.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
HUMAN_DIR="$HOME/.human"
VENV_DIR="$HUMAN_DIR/scraper-venv"
VENV_PYTHON="$VENV_DIR/bin/python3"
LAUNCH_AGENTS="$HOME/Library/LaunchAgents"

SKIP_LOGIN=false
for arg in "$@"; do
    case "$arg" in
        --skip-login) SKIP_LOGIN=true ;;
    esac
done

info()  { printf "\033[32m✓\033[0m %s\n" "$*"; }
warn()  { printf "\033[33m!\033[0m %s\n" "$*"; }
err()   { printf "\033[31m✗\033[0m %s\n" "$*" >&2; }
step()  { printf "\n\033[1m=== %s ===\033[0m\n" "$*"; }

# ── 1. Directory structure ──────────────────────────────────────────────────

step "Creating directory structure"

dirs=(
    "$HUMAN_DIR/feeds/ingest"
    "$HUMAN_DIR/browser_state"
    "$HUMAN_DIR/logs"
    "$HUMAN_DIR/config"
)
for d in "${dirs[@]}"; do
    mkdir -p "$d"
    info "$d"
done

# ── 2. Python venv + playwright ─────────────────────────────────────────────

step "Setting up Python environment"

if [ ! -x "$VENV_PYTHON" ]; then
    info "Creating venv at $VENV_DIR"
    python3 -m venv "$VENV_DIR"
fi

"$VENV_DIR/bin/pip" install --quiet --upgrade pip
"$VENV_DIR/bin/pip" install --quiet playwright
info "Playwright installed"

"$VENV_PYTHON" -m playwright install chromium 2>/dev/null
info "Chromium browser installed"

# ── 3. Build h-uman with feeds ──────────────────────────────────────────────

step "Building h-uman with feed support"

BUILD_DIR="$PROJECT_DIR/build"
if [ ! -f "$BUILD_DIR/human" ]; then
    mkdir -p "$BUILD_DIR"
    cmake -B "$BUILD_DIR" -S "$PROJECT_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DHU_ENABLE_ALL_CHANNELS=ON \
        -DHU_ENABLE_FEEDS=ON \
        -DHU_ENABLE_SKILLS=ON \
        -DHU_ENABLE_PERSONA=ON \
        -DHU_ENABLE_CRON=ON \
        -DHU_ENABLE_SOCIAL=ON \
        -DHU_ENABLE_SQLITE=ON \
        -DHU_ENABLE_LTO=ON 2>&1 | tail -3
    cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)" 2>&1 | tail -3
    info "h-uman built at $BUILD_DIR/human"
else
    info "h-uman binary already exists at $BUILD_DIR/human"
fi

# ── 4. Install launchd agents ───────────────────────────────────────────────

step "Installing launchd agents"

mkdir -p "$LAUNCH_AGENTS"

# 4a. Daily browser scraper (5:30 AM — scrapes Twitter, Facebook, TikTok)
SCRAPE_PLIST="$LAUNCH_AGENTS/com.human.daily-scrape.plist"
cat > "$SCRAPE_PLIST" << PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.human.daily-scrape</string>
    <key>ProgramArguments</key>
    <array>
        <string>${PROJECT_DIR}/scripts/daily_feed_scrape.sh</string>
    </array>
    <key>StartCalendarInterval</key>
    <dict>
        <key>Hour</key>
        <integer>5</integer>
        <key>Minute</key>
        <integer>30</integer>
    </dict>
    <key>StandardOutPath</key>
    <string>${HUMAN_DIR}/logs/launchd-scrape.log</string>
    <key>StandardErrorPath</key>
    <string>${HUMAN_DIR}/logs/launchd-scrape.log</string>
    <key>EnvironmentVariables</key>
    <dict>
        <key>HOME</key>
        <string>${HOME}</string>
        <key>PATH</key>
        <string>/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin</string>
    </dict>
</dict>
</plist>
PLIST

launchctl bootout "gui/$(id -u)/com.human.daily-scrape" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$SCRAPE_PLIST"
info "Installed daily scrape agent (5:30 AM)"

# 4b. Daily feed processor (6:00 AM — polls Gmail, iMessage, RSS, ingests JSONL, runs research agent)
FEED_PLIST="$LAUNCH_AGENTS/com.human.daily-feeds.plist"
cat > "$FEED_PLIST" << PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.human.daily-feeds</string>
    <key>ProgramArguments</key>
    <array>
        <string>${PROJECT_DIR}/scripts/daily_feed_process.sh</string>
    </array>
    <key>StartCalendarInterval</key>
    <dict>
        <key>Hour</key>
        <integer>6</integer>
        <key>Minute</key>
        <integer>0</integer>
    </dict>
    <key>StandardOutPath</key>
    <string>${HUMAN_DIR}/logs/launchd-feeds.log</string>
    <key>StandardErrorPath</key>
    <string>${HUMAN_DIR}/logs/launchd-feeds.log</string>
    <key>EnvironmentVariables</key>
    <dict>
        <key>HOME</key>
        <string>${HOME}</string>
        <key>PATH</key>
        <string>/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin</string>
    </dict>
</dict>
</plist>
PLIST

launchctl bootout "gui/$(id -u)/com.human.daily-feeds" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$FEED_PLIST"
info "Installed daily feed processor agent (6:00 AM)"

# ── 5. Browser login (interactive) ─────────────────────────────────────────

if [ "$SKIP_LOGIN" = false ]; then
    step "Browser login sessions"
    warn "A browser will open for each platform. Log in, then close the browser."
    warn "Browser state is saved automatically for future headless runs."

    for platform in twitter facebook tiktok; do
        echo ""
        read -rp "Press Enter to open $platform login (or 's' to skip): " choice
        if [ "$choice" = "s" ]; then
            warn "Skipped $platform login"
            continue
        fi

        "$VENV_PYTHON" -c "
from playwright.sync_api import sync_playwright
from pathlib import Path
import sys

urls = {'twitter': 'https://x.com/login', 'facebook': 'https://www.facebook.com/login', 'tiktok': 'https://www.tiktok.com/login'}
url = urls['$platform']
state_dir = Path.home() / '.human' / 'browser_state'
state_dir.mkdir(parents=True, exist_ok=True)
state_file = state_dir / '${platform}.json'

with sync_playwright() as p:
    browser = p.chromium.launch(headless=False)
    context = browser.new_context()
    page = context.new_page()
    page.goto(url, wait_until='networkidle', timeout=60000)
    input('Log in to $platform, then press Enter here to save session...')
    context.storage_state(path=str(state_file))
    browser.close()
print(f'Session saved to {state_file}')
" 2>&1 || warn "$platform login failed"

        info "$platform session saved"
    done
else
    warn "Skipped browser login (use --skip-login was set)"
    warn "Run later: ./scripts/setup_automation.sh  (without --skip-login)"
fi

# ── Summary ─────────────────────────────────────────────────────────────────

step "Setup Complete"
echo ""
info "Directory structure:  $HUMAN_DIR/"
info "Python venv:          $VENV_DIR/"
info "Browser state:        $HUMAN_DIR/browser_state/"
info "Feed ingest dir:      $HUMAN_DIR/feeds/ingest/"
info "Logs:                 $HUMAN_DIR/logs/"
echo ""
info "Scheduled agents:"
echo "  5:30 AM  — Browser scrape (Twitter, Facebook, TikTok)"
echo "  6:00 AM  — Feed processor (Gmail, iMessage, RSS, JSONL ingest, research agent)"
echo ""
warn "Next steps:"
echo "  1. If you skipped login, run: ./scripts/setup_automation.sh"
echo "  2. Set Gmail OAuth in ~/.human/config.json (client_id, client_secret, refresh_token)"
echo "  3. Grant Full Disk Access to Terminal.app for iMessage reading"
echo "  4. Test manually: ./scripts/daily_feed_scrape.sh"
echo "  5. Check logs: tail -f ~/.human/logs/daily-scrape-*.log"
