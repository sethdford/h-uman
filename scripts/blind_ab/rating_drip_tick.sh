#!/bin/bash
# Wrapper script for launchd: run rating_drip.py tick, then rating_ingest.py ingest.
# This combines the send and harvest loops into one daily tick.
#
# To use: update the launchd plist to call:
#   /absolute/path/to/scripts/blind_ab/rating_drip_tick.sh

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PYTHON="${PYTHON:-python3}"

# Run send (rating_drip.py tick)
echo "[$(date +%Y-%m-%d\ %H:%M:%S)] rating_drip: send phase"
"$PYTHON" "$SCRIPT_DIR/rating_drip.py" tick

# Run harvest (rating_ingest.py ingest)
echo "[$(date +%Y-%m-%d\ %H:%M:%S)] rating_ingest: harvest phase"
"$PYTHON" "$SCRIPT_DIR/rating_ingest.py" ingest

echo "[$(date +%Y-%m-%d\ %H:%M:%S)] rating_drip_tick complete"
