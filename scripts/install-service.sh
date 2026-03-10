#!/usr/bin/env bash
# Install human LaunchAgent for auto-start on login.
# Run from the nullclaw repo root or scripts/ directory.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLIST_SRC="${SCRIPT_DIR}/com.human.service.plist"
PLIST_DEST="${HOME}/Library/LaunchAgents/com.human.service.plist"

# Ensure ~/.human exists for log files
mkdir -p "${HOME}/.human"

cp "$PLIST_SRC" "$PLIST_DEST"
launchctl load "$PLIST_DEST"

echo "human LaunchAgent installed successfully."
echo "  Plist: $PLIST_DEST"
echo "  Logs:  ${HOME}/.human/human.log"
echo ""
echo "To unload: launchctl unload ~/Library/LaunchAgents/com.human.service.plist"
