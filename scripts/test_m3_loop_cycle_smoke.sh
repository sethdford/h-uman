#!/usr/bin/env bash
# Phase H integration smoke (2026-05-19) — m3_loop_cycle.sh in isolation.
#
# Closes the last honest gap: the autonomous loop script itself was
# never end-to-end tested. This script:
#
#   1. Builds an isolated HUMAN_HOME tree (no real ~/.human touched)
#   2. Seeds fixture chat.db + memory.db so H1 has data to extract
#   3. Pre-creates an empty rewrite-pairs file (G-tier expects it)
#   4. Runs m3_loop_cycle.sh end-to-end against that tree
#   5. Asserts the H-tier artifacts (corpus, counterfactuals, queue,
#      probe pairs) all landed, AND that the merge step combined
#      H-tier preference pairs into the alpaca-DPO export file
#
# G-tier steps will soft-fail (no gateway, no daemon) — that's correct
# behavior and not the focus of this test.
#
# Exit codes:
#   0 — full chain succeeded with all expected artifacts
#   2 — any check failed; smoke directory left for forensics
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SMOKE_DIR="${SMOKE_DIR:-/tmp/m3-cycle-smoke}"
KEEP=0
for arg in "$@"; do
    case "$arg" in --keep) KEEP=1 ;; esac
done

[ "$KEEP" = "0" ] && rm -rf "$SMOKE_DIR"
mkdir -p "$SMOKE_DIR"
HUMAN_HOME="$SMOKE_DIR/.human"
mkdir -p "$HUMAN_HOME/training-data" "$HUMAN_HOME/logs"

# Pre-create the rewrites pairs file the G-tier expects (empty is fine —
# the cycle script's wc -l on a missing file would print 0 anyway, but
# being explicit makes the test idempotent).
: > "$HUMAN_HOME/training-data/m3-rewrite-pairs.jsonl"

banner() { printf "\n══ %s ══\n" "$*"; }
fail() { echo "  FAIL: $*" >&2; FAILED=1; }

FAILED=0

banner "STEP 1 — Build fixture DBs"
python3 - "$SMOKE_DIR" <<'PY'
import sqlite3, sys
from pathlib import Path
d = Path(sys.argv[1])
imsg = d / "fixture-chat.db"
mem  = d / "fixture-memory.db"

c = sqlite3.connect(str(imsg))
c.execute("CREATE TABLE message (ROWID INTEGER PRIMARY KEY, text TEXT, "
          "is_from_me INTEGER, date INTEGER, handle_id INTEGER, "
          "attributedBody BLOB)")
c.execute("CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT)")
c.execute("INSERT INTO handle(ROWID,id) VALUES (1,'+15555550001')")
rows = [
    ("hey how was your day", 0, 1_000_000_000, 1),
    ("yeah good, just got home", 1, 1_100_000_000, 1),
    ("you free saturday for hiking?", 0, 1_200_000_000, 1),
    ("hmm let me check, lemme get back to you", 1, 1_300_000_000, 1),
]
for r in rows:
    c.execute("INSERT INTO message(text,is_from_me,date,handle_id) "
              "VALUES (?,?,?,?)", r)
c.commit(); c.close()

c = sqlite3.connect(str(mem))
c.execute("CREATE TABLE messages(id INTEGER PRIMARY KEY AUTOINCREMENT, "
          "session_id TEXT NOT NULL, role TEXT NOT NULL, "
          "content TEXT NOT NULL, created_at TEXT DEFAULT(datetime('now')))")
for role, content in [
    ("user", "dinner?"),
    ("assistant", "yes lets go"),
]:
    c.execute("INSERT INTO messages(session_id,role,content,created_at) "
              "VALUES (?,?,?,?)", ("s1", role, content, "2026-05-15 12:00:00"))
c.commit(); c.close()
print(f"  built {imsg.name} + {mem.name}")
PY
[ -f "$SMOKE_DIR/fixture-chat.db" ] || fail "chat.db fixture not created"

banner "STEP 2 — Run m3_loop_cycle.sh against isolated tree"
# Override:
#   HUMAN_HOME → isolated training-data tree
#   M3_IMSG_DB → fixture chat.db
#   M3_MEMORY_DB → fixture memory.db
#   M3_DPO_THRESHOLD=99999 → never trigger real training (no MLX server in test)
#   M3_AUTO_PROMOTE=0 → never auto-promote
#   M3_H_TIER_ENABLE=1 → run the H-tier prelude (the thing we're testing)
HUMAN_HOME="$HUMAN_HOME" \
M3_IMSG_DB="$SMOKE_DIR/fixture-chat.db" \
M3_MEMORY_DB="$SMOKE_DIR/fixture-memory.db" \
M3_DPO_THRESHOLD=99999 \
M3_AUTO_PROMOTE=0 \
M3_H_TIER_ENABLE=1 \
M3_PROBE_RESPONSE_MODE=simulate-tick \
M3_PROBE_SIMULATED_REPLY=A \
bash "$REPO_ROOT/scripts/m3_loop_cycle.sh" >"$SMOKE_DIR/cycle.out" 2>&1 || true
# We don't assert rc=0 because G-tier steps will (correctly) soft-fail
# when there's no gateway. We assert ARTIFACTS instead.

banner "STEP 3 — Verify H-tier artifacts landed"
check_file() {
    local f="$1" min_lines="${2:-1}"
    if [ ! -f "$f" ]; then fail "$f missing"; return; fi
    local n; n=$(wc -l < "$f" | tr -d ' ')
    if [ "$n" -lt "$min_lines" ]; then
        fail "$f has $n lines, expected ≥ $min_lines"
    else
        printf "  OK   %s  (%s lines)\n" "$f" "$n"
    fi
}

check_file "$HUMAN_HOME/training-data/m3-corpus.jsonl" 1
check_file "$HUMAN_HOME/training-data/m3-counterfactuals.jsonl" 1
check_file "$HUMAN_HOME/training-data/m3-active-probe-queue.jsonl" 1
check_file "$HUMAN_HOME/training-data/m3-active-probe-pairs.jsonl" 1

banner "STEP 4 — Verify probe collector consumed the queue"
# After simulate-tick with --simulate-response=A, the queue entry
# should have status=done (not pending)
status=$(python3 -c "
import json
with open('$HUMAN_HOME/training-data/m3-active-probe-queue.jsonl') as f:
    for line in f:
        if not line.strip(): continue
        r = json.loads(line)
        print(r.get('status', '?'))
" | head -1)
if [ "$status" = "done" ]; then
    printf "  OK   queue entry status=done\n"
else
    fail "queue entry status='$status', expected 'done'"
fi

banner "STEP 5 — Verify alpaca-DPO merge included H-tier pairs"
# The export filename is m3-alpaca-dpo-YYYYMMDD.jsonl
export_glob=$(ls "$HUMAN_HOME"/training-data/m3-alpaca-dpo-*.jsonl 2>/dev/null | head -1)
if [ -z "$export_glob" ]; then
    fail "alpaca-DPO export file not created"
else
    n=$(wc -l < "$export_glob" | tr -d ' ')
    if [ "$n" -lt 1 ]; then
        fail "alpaca-DPO export is empty (merge didn't run)"
    else
        printf "  OK   %s  (%s pairs from H-tier merge)\n" "$export_glob" "$n"
        # Verify the merge actually pulled from the H-tier sources by
        # checking the cycle log for the merge lines
        if grep -q "merged.*counterfactuals.jsonl" "$HUMAN_HOME/logs/m3-loop-$(date +%Y-%m-%d).log" 2>/dev/null; then
            printf "  OK   log shows counterfactuals merged\n"
        else
            fail "cycle log doesn't show counterfactuals merged"
        fi
        if grep -q "merged.*active-probe-pairs.jsonl" "$HUMAN_HOME/logs/m3-loop-$(date +%Y-%m-%d).log" 2>/dev/null; then
            printf "  OK   log shows probe-pairs merged\n"
        else
            fail "cycle log doesn't show probe-pairs merged"
        fi
    fi
fi

banner "STEP 6 — Verify cycle script wrote its own log"
log_file="$HUMAN_HOME/logs/m3-loop-$(date +%Y-%m-%d).log"
if [ ! -f "$log_file" ]; then
    fail "log file $log_file missing"
elif ! grep -q "m3-loop-cycle complete" "$log_file"; then
    fail "log doesn't show 'm3-loop-cycle complete'"
else
    printf "  OK   log present and shows complete marker\n"
fi

banner "RESULT"
if [ "$FAILED" = "0" ]; then
    echo "  ✓ m3_loop_cycle.sh smoke PASS"
    echo "    smoke dir: $SMOKE_DIR (--keep to preserve)"
    [ "$KEEP" = "0" ] && rm -rf "$SMOKE_DIR"
    exit 0
else
    echo "  ✗ m3_loop_cycle.sh smoke FAIL (smoke dir preserved: $SMOKE_DIR)"
    echo "    cycle output: $SMOKE_DIR/cycle.out"
    exit 2
fi
