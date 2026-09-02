#!/usr/bin/env bash
# rotate-logs.sh — copy-truncate oversized files under ~/.human/logs.
#
# Why copy-truncate: the daemon's stderr is a launchd-owned O_APPEND fd on
# service-loop-error.log. Renaming the file would leave the daemon writing
# into the renamed inode; truncating in place keeps the live fd valid and
# the next write lands at offset 0. Rotated copies are gzipped beside the
# original (name.YYYYmmdd-HHMMSS.gz) and only the newest HU_ROTATE_KEEP are
# kept. Writes one dated marker line per run to logs/logrotate.log so the
# nightly watchdog can tell it ran.
set -u
LOGDIR="${HU_LOG_DIR:-$HOME/.human/logs}"
MAX="${HU_ROTATE_MAX_BYTES:-20000000}"   # 20 MB
KEEP="${HU_ROTATE_KEEP:-5}"
MARK="$LOGDIR/logrotate.log"
mkdir -p "$LOGDIR"
stamp=$(date '+%Y%m%d-%H%M%S')
rotated=0; scanned=0
for f in "$LOGDIR"/*.log; do
  [ -f "$f" ] || continue
  case "$(basename "$f")" in logrotate.log) continue ;; esac
  scanned=$((scanned+1))
  size=$(stat -f %z "$f" 2>/dev/null || stat -c %s "$f" 2>/dev/null || echo 0)
  [ "$size" -gt "$MAX" ] || continue
  cp -p "$f" "$f.$stamp" && : > "$f" && gzip -f "$f.$stamp" && rotated=$((rotated+1))
  # prune: keep the newest $KEEP rotated copies of this file
  ls -t "$f".*.gz 2>/dev/null | tail -n +$((KEEP+1)) | xargs rm -f 2>/dev/null
done
echo "[$(date '+%Y-%m-%d %H:%M:%S')] rotate-logs scanned=$scanned rotated=$rotated max_bytes=$MAX keep=$KEEP" >> "$MARK"
exit 0
