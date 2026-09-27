#!/usr/bin/env bash
# profile_check_weekly.sh — weekly stale-contact-profile check.
#
# Why (2026-09-26): a contact's profile was written from the first 53 messages
# of a 1,200-message thread and still said "prefers short texts, no emoji";
# h-uman sent her 13 one-word replies in a row. Nothing compared profiles with
# the real threads. This runs scripts/check_contact_profiles.py via the
# nightly-watchdog "profile-check" job (lookback 7 = weekly), writes
# ~/.human/logs/profile-check-DATE.json, and posts one macOS banner naming any
# stale profiles. It never edits the persona: fixing a profile is a human
# decision (the report says what disagrees and by how much).
#
# Exit: the checker's own code — 0 nothing stale, 1 stale found (report and
# banner written), 2 could not measure (nothing written).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATE="$(date +%Y-%m-%d)"
LOGDIR="$HOME/.human/logs"
PY="${HU_EVAL_PYTHON:-/opt/homebrew/bin/python3}"; [ -x "$PY" ] || PY="python3"
mkdir -p "$LOGDIR"
echo "[$DATE] profile_check_weekly: start"
"$PY" "$HERE/check_contact_profiles.py" --out "$LOGDIR/profile-check-${DATE}.json" --notify
rc=$?
echo "[$DATE] profile_check_weekly: rc=$rc"
exit "$rc"
