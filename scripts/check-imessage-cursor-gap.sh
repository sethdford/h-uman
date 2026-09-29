#!/usr/bin/env bash
# check-imessage-cursor-gap.sh — refuse to (re)start the daemon on a stale
# iMessage poll cursor.
#
# Incident 2026-09-01: after a reboot the daemon resumed the iMessage poll from
# ~/.human/imessage.rowid two weeks behind chat.db and replayed ~2,000 old
# inbound messages to real contacts. The binary now caps that replay
# (hu_imessage_resume_rowid), but the cap lives in the binary being REPLACED,
# so an install/restart is exactly the moment to check the on-disk cursor
# against ground truth — and to say so loudly instead of trusting a log line.
#
# Contract (LOOP pattern): exit 0 = healthy (one line), exit 1 = gap over the
# cap (prints the numbers and the fix), exit 0 with a NOTE when it cannot read
# chat.db (no Full Disk Access in this shell is not evidence of a stale cursor).
#
# Usage: check-imessage-cursor-gap.sh [max_gap_rows]   (default 50, matches
#        HU_IMESSAGE_MAX_REPLAY_ROWS_DEFAULT in include/human/channels/imessage.h)
# Env:   HU_CURSOR_GAP_FORCE=1 downgrades a FAIL to a warning (deliberate replay).
#        HU_IMESSAGE_ROWID_FILE / HU_CHAT_DB override the paths (tests).
set -uo pipefail

MAX_GAP="${1:-50}"
ROWID_FILE="${HU_IMESSAGE_ROWID_FILE:-$HOME/.human/imessage.rowid}"
CHAT_DB="${HU_CHAT_DB:-$HOME/Library/Messages/chat.db}"

if [[ ! -f "$ROWID_FILE" ]]; then
    echo "cursor-gap: no $ROWID_FILE — first run; the daemon seeds from db max. OK"
    exit 0
fi
cursor="$(tr -cd '0-9' < "$ROWID_FILE")"
if [[ -z "$cursor" ]]; then
    echo "cursor-gap: $ROWID_FILE is empty/garbage — the daemon will seed from db max. OK"
    exit 0
fi

if ! command -v sqlite3 >/dev/null 2>&1; then
    echo "cursor-gap: NOTE sqlite3 not on PATH; cannot compare to chat.db (not a FAIL)"
    exit 0
fi
dbmax="$(sqlite3 -readonly "$CHAT_DB" 'SELECT max(ROWID) FROM message;' 2>/dev/null || true)"
if [[ -z "$dbmax" ]]; then
    echo "cursor-gap: NOTE cannot read $CHAT_DB from this shell (Full Disk Access?) — cannot compare (not a FAIL)"
    exit 0
fi

gap=$((dbmax - cursor))
if (( gap < 0 )); then
    echo "cursor-gap: cursor $cursor is AHEAD of db max $dbmax (rowid reset / new Mac?) — daemon reseeds from db max. OK"
    exit 0
fi
if (( gap <= MAX_GAP )); then
    echo "cursor-gap: OK cursor=$cursor dbmax=$dbmax gap=$gap (cap $MAX_GAP)"
    exit 0
fi

echo "cursor-gap: FAIL cursor=$cursor is $gap rows behind chat.db max=$dbmax (cap $MAX_GAP)" >&2
echo "            A restart on a binary WITHOUT the resume cap would replay $gap messages as fresh." >&2
echo "            Fix: echo $dbmax > $ROWID_FILE   (those rows were already seen on the phone)" >&2
echo "            or set HU_CURSOR_GAP_FORCE=1 for a deliberate replay." >&2
if [[ "${HU_CURSOR_GAP_FORCE:-0}" == "1" ]]; then
    echo "cursor-gap: HU_CURSOR_GAP_FORCE=1 — continuing anyway" >&2
    exit 0
fi
exit 1
