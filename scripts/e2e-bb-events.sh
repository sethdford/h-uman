#!/usr/bin/env bash
# e2e-bb-events.sh — end-to-end proof for the IMCore bridge event stream.
#
# Drives the REAL pipeline on a live macOS box:
#
#   dylib inbox file  →  real `imsg watch --bb-events`  →  real stdout bytes
#        →  tests/fixtures/bb-events/live-capture.jsonl  →  h-uman's parser
#
# The captured golden file is committed so the parse half replays in CI on any
# host, without a bridge. This script regenerates it and re-proves the live
# half on a Mac with SIP disabled and `imsg launch` done.
#
# Usage:  bash scripts/e2e-bb-events.sh [--write-fixture]
#
# NOTE: injecting into the inbox exercises every hop EXCEPT the dylib's own
# emit, which requires a remote party to actually type at this Mac. See
# docs/plans/2026-07-19-native-imessage/bb-events-schema.md §5.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIXTURE_DIR="$REPO/tests/fixtures/bb-events"
FIXTURE="$FIXTURE_DIR/live-capture.jsonl"
INBOX="$HOME/Library/Containers/com.apple.MobileSMS/Data/.imsg-events.jsonl"
READY="$HOME/Library/Containers/com.apple.MobileSMS/Data/.imsg-bridge-ready"
WRITE_FIXTURE=0
[ "${1:-}" = "--write-fixture" ] && WRITE_FIXTURE=1

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

echo "== 1. bridge liveness =="
command -v imsg >/dev/null || fail "imsg not on PATH"
# Capture, then match. Do NOT pipe into `grep -q` under `set -o pipefail`:
# grep exits on first match and closes the pipe, the producer takes SIGPIPE
# (141), and pipefail propagates that — so a SUCCESSFUL match reports failure.
IMSG_STATUS="$(imsg status 2>&1 || true)"
case "$IMSG_STATUS" in
*"IMCore bridge connected"*) ;;
*) fail "IMCore bridge not connected (SIP enabled, or run: imsg launch)" ;;
esac
[ -f "$READY" ] || fail "missing bridge-ready sentinel: $READY"
# The env var alone is NOT proof the dylib loaded — check the open handle.
MSGPID="$(pgrep -x Messages || true)"
[ -n "$MSGPID" ] || fail "Messages.app is not running (run: imsg launch)"
MSG_MAPS="$(lsof -p "$MSGPID" 2>/dev/null || true)"
case "$MSG_MAPS" in
*imsg-bridge*) ;;
*) fail "imsg-bridge dylib is NOT loaded into Messages (library validation blocked it)" ;;
esac
echo "   OK: bridge connected, dylib loaded into Messages pid $MSGPID"

echo "== 2. start the REAL watch subprocess (production argv shape) =="
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; kill "${WPID:-}" 2>/dev/null || true' EXIT
INBOX_BACKUP="$TMP/inbox.bak"
cp "$INBOX" "$INBOX_BACKUP" 2>/dev/null || : >"$INBOX_BACKUP"

imsg watch --json --since-rowid 999999999 --bb-events >"$TMP/stdout.jsonl" 2>"$TMP/stderr.log" &
WPID=$!
sleep 4
kill -0 "$WPID" 2>/dev/null || fail "watch subprocess died immediately: $(cat "$TMP/stderr.log")"
echo "   OK: watch running (pid $WPID)"

echo "== 3. inject bridge records into the REAL dylib inbox =="
# --since-rowid 999999999 suppresses chat.db rows, so anything that appears on
# stdout came through the bridge-event path specifically.
{
  echo '{"event":"started-typing","data":{"chatGuid":"iMessage;-;+15551234567","handle":"+15551234567","timestamp":1784510000.5}}'
  echo '{"event":"stopped-typing","data":{"chatGuid":"iMessage;-;+15551234567","handle":"+15551234567","timestamp":1784510006.1}}'
  echo '{"event":"aliases-removed","data":{"aliasType":"phone","aliases":["+15559876543"]}}'
} >>"$INBOX"
sleep 5
kill "$WPID" 2>/dev/null || true
wait "$WPID" 2>/dev/null || true
WPID=""

# Restore the inbox to exactly what it was before we touched it.
cp "$INBOX_BACKUP" "$INBOX"

echo "== 4. verify the CLI re-wrapped them onto stdout =="
COUNT="$(grep -c '"kind":"bridge-event"' "$TMP/stdout.jsonl" || true)"
[ "$COUNT" -ge 3 ] || {
  echo "--- captured stdout ---"
  cat "$TMP/stdout.jsonl"
  fail "expected >=3 bridge-event lines, got $COUNT"
}
grep -q '"event":"started-typing"' "$TMP/stdout.jsonl" || fail "no started-typing on stdout"
grep -q '"event":"stopped-typing"' "$TMP/stdout.jsonl" || fail "no stopped-typing on stdout"
echo "   OK: $COUNT bridge-event lines captured from the live CLI"

if [ "$WRITE_FIXTURE" = "1" ]; then
  mkdir -p "$FIXTURE_DIR"
  cp "$TMP/stdout.jsonl" "$FIXTURE"
  echo "   wrote fixture: $FIXTURE"
fi

echo "== 5. replay the captured bytes through h-uman's parser =="
[ -x "$REPO/build/human_tests" ] || fail "build/human_tests missing (cmake --build build --target human_tests)"
# cd so the replay test resolves tests/fixtures/bb-events/live-capture.jsonl —
# it feeds THESE captured bytes through the production stream splitter.
cd "$REPO"
REPLAY="$("$REPO/build/human_tests" --suite=imessage_bb_event 2>&1 || true)"
case "$REPLAY" in
*"PASS  bb_stream_replays_live_capture"*) ;;
*) printf '%s\n' "$REPLAY" | tail -20; fail "replay of the live capture did not pass" ;;
esac
printf '%s\n' "$REPLAY" | grep -E "Results:" || true

echo
echo "E2E PASS: live bridge → imsg CLI → captured stdout → h-uman parser"
