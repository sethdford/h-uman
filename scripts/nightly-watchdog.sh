#!/usr/bin/env bash
# nightly-watchdog.sh — run any nightly whose marker for today/yesterday is missing.
#
# Why: launchd StartCalendarInterval jobs do not catch up across a multi-week
# sleep (nothing under ~/.human was written 2026-08-10 -> 08-31). A StartInterval
# job DOES fire on wake, so this runs every 6h, checks each nightly's artifact,
# and runs the missing ones serially under one lock — never while :8741 is down
# or a trainer is loading a model (never two model loaders).
#
# Markers are ARTIFACTS (a verdict file, a dated log line), never a launchd
# "runs" counter: a job that ran and produced nothing counts as not run.
#
# Usage: nightly-watchdog.sh [--dry-run]   (exit 0; prints one summary line)
set -uo pipefail
DRY=0; [ "${1:-}" = "--dry-run" ] && DRY=1
HOME_DIR="${HOME}"
LOGDIR="$HOME_DIR/.human/logs"; LOCKDIR="$HOME_DIR/.human/locks"; mkdir -p "$LOGDIR" "$LOCKDIR"
LOG="$LOGDIR/nightly-watchdog.log"
REPO="${HU_REPO_DIR:-$HOME_DIR/Projects/h-uman}"
TODAY=$(date +%Y-%m-%d); YDAY=$(date -v-1d +%Y-%m-%d 2>/dev/null || date -d yesterday +%Y-%m-%d)
MLX="${HU_MLX_HEALTH:-http://127.0.0.1:8741/health}"

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG"; }

# name|marker-kind|marker|wrapper|allowed-hours (HH-HH local, or "any")
# retrain STOPS the production server for its run; it may only be started in
# its contract window so a wake at 14:00 never takes :8741 down.
JOBS=(
  "humanness|file|$LOGDIR/humanness-verdict-DATE.json|$HOME_DIR/.human/bin/humanness-nightly.sh|any"
  "doctor|line|$LOGDIR/doctor-nightly.log|$HOME_DIR/.human/bin/doctor-nightly.sh|any"
  "retrain|line|$LOGDIR/nightly-retrain.log|$REPO/scripts/nightly-retrain.sh|02-05"
)
HOUR_NOW=${HU_WATCHDOG_HOUR:-$(date +%H)}
in_window() {  # "any" | "HH-HH"
  [ "$1" = "any" ] && return 0
  local lo=${1%-*} hi=${1#*-}
  [ "$((10#$HOUR_NOW))" -ge "$((10#$lo))" ] && [ "$((10#$HOUR_NOW))" -lt "$((10#$hi))" ]
}

marker_present() {  # kind marker
  local kind="$1" m="$2" d
  for d in "$TODAY" "$YDAY"; do
    case "$kind" in
      file) [ -s "${m//DATE/$d}" ] && return 0 ;;
      line) [ -f "$m" ] && grep -q "^\[$d" "$m" && return 0 ;;
    esac
  done
  return 1
}

serving_up() { curl -s -m 5 "$MLX" >/dev/null 2>&1; }
trainer_running() { pgrep -f "mlx_lm.lora|mlx_lm_lora|train-glm-adapter|embed_server.py --port 8741" >/dev/null 2>&1; }

missing=(); ran=(); skipped=()
for spec in "${JOBS[@]}"; do
  IFS='|' read -r name kind marker wrapper window <<<"$spec"
  if marker_present "$kind" "$marker"; then continue; fi
  missing+=("$name")
  [ -x "$wrapper" ] || { skipped+=("$name:no-wrapper"); continue; }
  if ! in_window "$window"; then skipped+=("$name:outside-window-$window"); continue; fi
  if [ "$DRY" = 1 ]; then ran+=("$name(dry)"); continue; fi
  if ! serving_up; then skipped+=("$name:mlx-down"); continue; fi
  if trainer_running; then skipped+=("$name:trainer-busy"); continue; fi
  log "running $name ($wrapper) — marker missing for $TODAY/$YDAY"
  ( flock -w 600 9 || exit 75; bash "$wrapper" >> "$LOG" 2>&1 ) 9>"$LOCKDIR/nightly.lock"
  rc=$?
  log "$name exited rc=$rc"
  if marker_present "$kind" "$marker"; then ran+=("$name"); else skipped+=("$name:ran-but-no-artifact(rc=$rc)"); fi
done
summary="watchdog $TODAY missing=[${missing[*]:-}] ran=[${ran[*]:-}] skipped=[${skipped[*]:-}]"
[ "$DRY" = 1 ] || log "$summary"
echo "$summary"
exit 0
