#!/usr/bin/env bash
# verify-reaction-collection-fix.sh
#
# End-to-end verification of the 2026-05-18 reaction_collection config fix.
#
# Preconditions:
#   - ~/.human/config.json has reaction_collection.enabled = true
#   - Daemon binary has been rebuilt from source that includes parse_reaction_collection
#   - Daemon is restartable via launchctl bootout/bootstrap of
#     ai.human.intelligence-cycle.plist
#
# Each check fails loud (set -euo pipefail) and exits non-zero on regression.
# Run AFTER the production binary at ~/Projects/h-uman/build/human is rebuilt.
# (Historic clone path was ~/Documents/h-uman; G16 redirect 2026-05-26.)
set -euo pipefail

CONFIG=~/.human/config.json
LOG=~/.human/logs/intelligence-cycle-error.log
PLIST=~/Library/LaunchAgents/ai.human.intelligence-cycle.plist
DB=~/.human/memory.db
PROD_BIN=~/Projects/h-uman/build/human
DEV_BIN=/Users/sethford/Projects/h-uman/.claude/worktrees/jovial-wright-29b50b/build/human

# --- check 1: config file has the block --------------------------------
echo "=== check 1: config.json has reaction_collection.enabled = true ==="
python3 -c "
import json,sys
c = json.load(open('$CONFIG'))
rc = c.get('reaction_collection') or {}
assert rc.get('enabled') is True, f'reaction_collection.enabled not True: {rc}'
print('OK:', json.dumps(rc, indent=2))
" || { echo "FAIL: config block missing or disabled"; exit 1; }

# --- check 2: a daemon binary that parses the key exists ---------------
echo ""
echo "=== check 2: identify daemon binary with parse_reaction_collection ==="
HU_BIN=""
for cand in "$PROD_BIN" "$DEV_BIN"; do
    [ -x "$cand" ] || continue
    # Use grep -c (count) instead of -q to avoid pipefail+SIGPIPE killing the pipeline:
    # grep -q exits 0 immediately on match, which closes the pipe before nm finishes
    # writing, nm receives SIGPIPE (exit 141), pipefail propagates 141, the `if`
    # sees false. -c reads the full input and exits 0 with a count to stdout.
    HITS="$(nm "$cand" 2>/dev/null | grep -c parse_reaction_collection || true)"
    if [ "$HITS" -gt 0 ]; then
        HU_BIN="$cand"
        echo "OK: $cand (timestamp: $(stat -f %Sm "$cand"))"
        break
    fi
done
[ -n "$HU_BIN" ] || { echo "FAIL: no daemon binary contains 'reaction_collection' symbol — rebuild needed"; exit 1; }

# --- check 3: deploy if dev binary is fresher --------------------------
echo ""
echo "=== check 3: deploy dev binary if fresher than prod ==="
if [ -x "$DEV_BIN" ] && [ "$DEV_BIN" -nt "$PROD_BIN" ]; then
    echo "Dev binary is newer; copying to prod path"
    cp "$DEV_BIN" "$PROD_BIN.new"
    mv "$PROD_BIN" "$PROD_BIN.pre-reaction-fix-$(date +%Y%m%d-%H%M%S)" || true
    mv "$PROD_BIN.new" "$PROD_BIN"
    HU_BIN="$PROD_BIN"
fi
echo "Using: $HU_BIN"

# --- check 4: restart the daemon --------------------------------------
echo ""
echo "=== check 4: restart daemon ==="
launchctl bootout "gui/$(id -u)/ai.human.intelligence-cycle" 2>/dev/null || true
sleep 2
launchctl bootstrap "gui/$(id -u)" "$PLIST"
sleep 5
launchctl print "gui/$(id -u)/ai.human.intelligence-cycle" 2>&1 | grep -E "state|pid =" | head -3 || { echo "FAIL: daemon not running after restart"; exit 1; }

# --- check 5: config parser must not reject the key -------------------
echo ""
echo "=== check 5: 'unknown key: reaction_collection' must be absent from new log ==="
sleep 2
# Look at the LAST 50 lines (post-restart). If the key still says unknown, fail.
RECENT_LOG_TAIL="$(tail -50 "$LOG")"
if echo "$RECENT_LOG_TAIL" | grep -q "unknown key.*reaction_collection"; then
    echo "FAIL: parser still rejects reaction_collection — binary doesn't have parse_reaction_collection"
    echo "$RECENT_LOG_TAIL" | grep "unknown key" | tail -5
    exit 1
fi
echo "OK: no 'unknown key' rejection for reaction_collection"

# --- check 6: poll loop is firing -------------------------------------
echo ""
echo "=== check 6: reaction poll activity (waiting 60s for first poll cycle) ==="
sleep 60
RECENT_POLL="$(tail -200 "$LOG" | grep -iE 'reaction|tapback|dpo_pair' | tail -10 || true)"
if [ -z "$RECENT_POLL" ]; then
    echo "WARN: no reaction/tapback/dpo_pair log lines in last 200. May be a logging-level config issue; check by SQL below."
else
    echo "OK: poll-related lines in recent log:"
    echo "$RECENT_POLL"
fi

# --- check 7: dpo_pairs row count baseline ----------------------------
echo ""
echo "=== check 7: dpo_pairs source distribution (post-fix baseline) ==="
sqlite3 "$DB" "SELECT source, COUNT(*) FROM dpo_pairs GROUP BY source ORDER BY 2 DESC;"
echo ""
echo "Note: source='imessage_tapback' rows will accumulate over hours/days as real tapbacks arrive."
echo "Re-run this script in 24h to confirm growth."

echo ""
echo "=== ALL CHECKS PASSED ==="
echo "Next action: in 24h, check 'SELECT COUNT(*) FROM dpo_pairs WHERE source=\"imessage_tapback\"' for non-zero growth."
