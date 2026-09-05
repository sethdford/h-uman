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
TODAY=$(date +%Y-%m-%d)
MLX="${HU_MLX_HEALTH:-http://127.0.0.1:8741/health}"

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG"; }

# name|marker-kind|marker|wrapper|allowed-hours (HH-HH local, or "any")|lookback-days
# lookback-days (optional, default 2 = today/yesterday): how far back a marker
# may be dated and still count. 7 makes a job WEEKLY without a day-of-week:
# it runs on the first tick inside its window once the newest marker is a
# week old, so a laptop asleep on "gate day" catches up instead of waiting.
# retrain STOPS the production server for its run; it may only be started in
# its contract window so a wake at 14:00 never takes :8741 down.
JOBS=(
  "humanness|file|$LOGDIR/humanness-verdict-DATE.json|$HOME_DIR/.human/bin/humanness-nightly.sh|any"
  "doctor|line|$LOGDIR/doctor-nightly.log|$HOME_DIR/.human/bin/doctor-nightly.sh|any"
  "retrain|line|$LOGDIR/nightly-retrain.log|$REPO/scripts/nightly-retrain.sh|02-05"
  "drift|file|$HOME_DIR/.human/drift/drift-DATE.json|$REPO/scripts/drift_monitor.py|any"
  "authorship|file|$LOGDIR/authorship-gap-DATE.json|$REPO/scripts/blind_ab/authorship_nightly.sh|any"
  "llm-judge|file|$LOGDIR/llm-judge-DATE.json|$REPO/scripts/blind_ab/llm_judge_tier.py|any"
  # Product gate (multiturn + fidelity + REAL blind-A/B refresh). Its launchd
  # job is calendar-only (04:05) and was dark 08-08 -> 09-02 while the laptop
  # slept; the watchdog is the only thing that notices. Marker = a dated line
  # in nightly-eval.log (the script logs "[YYYY-MM-DDTHH:MM:SS] ...").
  "eval|line|$LOGDIR/nightly-eval.log|$REPO/scripts/nightly_eval.sh|any"
  # Log hygiene: service-loop-error.log is launchd-appended forever (41 MB,
  # 530k lines, no rotation on 2026-09-02). rotate-logs.sh copy-truncates
  # oversized logs once a day and writes its own dated marker line.
  "logrotate|line|$LOGDIR/logrotate.log|$REPO/scripts/rotate-logs.sh|any"
  # Weekly re-measurement of the HU_SEMANTIC_RECALL LIVE gate (contract C1,
  # flipped live 2026-09-03 on one PROMOTE; EI fell ~0.1 per run for three
  # runs). Lookback 7 = weekly. Window 10-16 local: the 03:07 retrain (window
  # 02-05) STOPS :8741, and the 04:05 nightly eval pushes hours of serial
  # traffic through :8741 — a gate convoyed behind either would time out and
  # refuse. Last in the list so the long run never delays a nightly under the
  # lock. HTTP-only against :8741 (batch priority); on HOLD it writes an alert
  # and flips nothing — the revert is a human decision.
  "semantic-gate|file|$LOGDIR/semantic-gate-DATE.json|$REPO/scripts/semantic_gate_weekly.sh|10-16|7"
)
HOUR_NOW=${HU_WATCHDOG_HOUR:-$(date +%H)}
in_window() {  # "any" | "HH-HH"
  [ "$1" = "any" ] && return 0
  local lo=${1%-*} hi=${1#*-}
  [ "$((10#$HOUR_NOW))" -ge "$((10#$lo))" ] && [ "$((10#$HOUR_NOW))" -lt "$((10#$hi))" ]
}

day_ago() { date -v-"$1"d +%Y-%m-%d 2>/dev/null || date -d "$1 days ago" +%Y-%m-%d; }
marker_present() {  # kind marker [lookback-days]
  local kind="$1" m="$2" days="${3:-2}" i d
  for ((i = 0; i < days; i++)); do
    d=$(day_ago "$i")
    case "$kind" in
      file) [ -s "${m//DATE/$d}" ] && return 0 ;;
      line) [ -f "$m" ] && grep -q "^\[$d" "$m" && return 0 ;;
    esac
  done
  return 1
}

serving_up() { [ "${HU_WATCHDOG_SKIP_HEALTH:-0}" = 1 ] && return 0; curl -s -m 5 "$MLX" >/dev/null 2>&1; }
trainer_running() { pgrep -f "mlx_lm.lora|mlx_lm_lora|train-glm-adapter|embed_server.py --port 8741" >/dev/null 2>&1; }

missing=(); ran=(); skipped=()
for spec in "${JOBS[@]}"; do
  IFS='|' read -r name kind marker wrapper window lookback <<<"$spec"
  lookback="${lookback:-2}"
  if marker_present "$kind" "$marker" "$lookback"; then continue; fi
  missing+=("$name")
  [ -x "$wrapper" ] || { skipped+=("$name:no-wrapper"); continue; }
  if ! in_window "$window"; then skipped+=("$name:outside-window-$window"); continue; fi
  if [ "$DRY" = 1 ]; then ran+=("$name(dry)"); continue; fi
  if ! serving_up; then skipped+=("$name:mlx-down"); continue; fi
  if trainer_running; then skipped+=("$name:trainer-busy"); continue; fi
  log "running $name ($wrapper) — marker missing for the last $lookback day(s) ending $TODAY"
  # macOS has no flock(1): an atomic mkdir is the lock. A lock older than 6h
  # whose pid is gone is stale and reclaimed.
  LOCK="$LOCKDIR/nightly.lock.d"
  if ! mkdir "$LOCK" 2>/dev/null; then
    oldpid=$(cat "$LOCK/pid" 2>/dev/null || echo 0)
    if [ "$oldpid" -gt 0 ] && kill -0 "$oldpid" 2>/dev/null; then skipped+=("$name:locked-by-$oldpid"); continue; fi
    if [ -n "$(find "$LOCK" -maxdepth 0 -mmin +360 2>/dev/null)" ] || [ "$oldpid" -eq 0 ] || ! kill -0 "$oldpid" 2>/dev/null; then
      rm -rf "$LOCK"; mkdir "$LOCK" 2>/dev/null || { skipped+=("$name:lock-race"); continue; }
    fi
  fi
  echo $$ > "$LOCK/pid"
  "$wrapper" >> "$LOG" 2>&1   # executable with its own shebang (bash or python)
  rc=$?
  rm -rf "$LOCK"
  log "$name exited rc=$rc"
  if marker_present "$kind" "$marker" "$lookback"; then ran+=("$name"); else skipped+=("$name:ran-but-no-artifact(rc=$rc)"); fi
done
summary="watchdog $TODAY missing=[${missing[*]:-}] ran=[${ran[*]:-}] skipped=[${skipped[*]:-}]"
[ "$DRY" = 1 ] || log "$summary"
echo "$summary"
exit 0
