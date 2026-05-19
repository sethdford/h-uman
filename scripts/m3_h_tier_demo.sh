#!/usr/bin/env bash
# Phase H demo (2026-05-19) — full data-acquisition tier in one shot.
#
# Runs every H-tier producer end-to-end against ISOLATED paths so it
# doesn't touch the operator's real ~/.human/training-data state:
#
#   H1   extract real corpus (iMessage + memory.db, PII-redacted)
#   H2   generate counterfactual preference pairs
#   H3   queue an active-learning probe
#   H3b  collector consumes the queue, converts a simulated reply
#        to Alpaca-DPO pairs
#   merge concatenate counterfactual + probe pairs into a single
#        training file; report shape + record counts
#
# After this runs, $DEMO_DIR/combined-pairs.jsonl is a ready-to-train
# Alpaca-DPO file mixing both producer sources. This is the artifact
# the G-tier loop expects.
#
# Usage:
#   bash scripts/m3_h_tier_demo.sh            # demo against /tmp/m3-h-demo
#   bash scripts/m3_h_tier_demo.sh --keep     # don't wipe DEMO_DIR first
#   bash scripts/m3_h_tier_demo.sh --fixture  # use fixture DBs (CI mode)
#   DEMO_DIR=/custom/path bash scripts/m3_h_tier_demo.sh
#
# Exit codes:
#   0 — full chain succeeded; combined-pairs.jsonl has ≥ 1 record
#   2 — any step failed OR no records were produced
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEMO_DIR="${DEMO_DIR:-/tmp/m3-h-demo}"
KEEP=0
FIXTURE=0
for arg in "$@"; do
    case "$arg" in
        --keep) KEEP=1 ;;
        --fixture) FIXTURE=1 ;;
    esac
done

if [ "$KEEP" = "0" ]; then
    rm -rf "$DEMO_DIR"
fi
mkdir -p "$DEMO_DIR"

CORPUS="$DEMO_DIR/corpus.jsonl"
CF_PAIRS="$DEMO_DIR/counterfactual-pairs.jsonl"
PROBE_QUEUE="$DEMO_DIR/probe-queue.jsonl"
PROBE_PAIRS="$DEMO_DIR/probe-pairs.jsonl"
COMBINED="$DEMO_DIR/combined-pairs.jsonl"

banner() { printf "\n══ %s ══\n" "$*"; }
require_lines() {
    local f="$1" min="$2"
    if [ ! -f "$f" ]; then
        echo "  ERROR: expected $f to exist"; return 1
    fi
    local n; n=$(wc -l < "$f" | tr -d ' ')
    if [ "$n" -lt "$min" ]; then
        echo "  ERROR: expected $f to have ≥ $min lines, got $n"; return 1
    fi
    echo "  OK: $f has $n lines"; return 0
}

banner "STEP 1 — H1 corpus extract"
if [ "$FIXTURE" = "1" ]; then
    # CI / no-FDA mode: build minimal fixture DBs so the rest of the
    # chain has data to chew on. Keeps the demo runnable on hosts
    # without ~/Library/Messages/chat.db.
    echo "  building fixture chat.db + memory.db..."
    python3 - "$DEMO_DIR" <<'PY'
import sqlite3, sys
from pathlib import Path
d = Path(sys.argv[1])
imsg = d / "fixture-chat.db"
mem  = d / "fixture-memory.db"
c = sqlite3.connect(str(imsg))
c.execute("CREATE TABLE message (ROWID INTEGER PRIMARY KEY, text TEXT, is_from_me INTEGER, date INTEGER, handle_id INTEGER)")
c.execute("CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT)")
c.execute("INSERT INTO handle(ROWID,id) VALUES (1,'+15555550001'),(2,'friend@example.com')")
rows = [
    ("you free for coffee?", 0, 1_000_000_000, 1),
    ("yeah sounds good", 1, 1_100_000_000, 1),
    ("when?", 0, 1_200_000_000, 1),
    ("3pm works", 1, 1_300_000_000, 1),
    ("just got home", 0, 1_400_000_000, 2),
    ("nice, long day?", 1, 1_500_000_000, 2),
]
for r in rows: c.execute("INSERT INTO message(text,is_from_me,date,handle_id) VALUES (?,?,?,?)", r)
c.commit(); c.close()

c = sqlite3.connect(str(mem))
c.execute("CREATE TABLE messages(id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL, role TEXT NOT NULL, content TEXT NOT NULL, created_at TEXT DEFAULT(datetime('now')))")
for i,(role,content) in enumerate([("user","hey"),("assistant","what's up"),("user","dinner saturday?")]):
    c.execute("INSERT INTO messages(session_id,role,content,created_at) VALUES (?,?,?,?)", ("s1", role, content, f"2026-05-{15+i} 12:00:00"))
c.commit(); c.close()
print(f"  fixture DBs ready: {imsg.name}, {mem.name}")
PY
    python3 "$REPO_ROOT/scripts/m3_extract_corpus.py" \
        --out "$CORPUS" \
        --sources imessage,memory_db \
        --imessage-db "$DEMO_DIR/fixture-chat.db" \
        --memory-db "$DEMO_DIR/fixture-memory.db" \
        --max-per-source 200 || { echo "  H1 (fixture) failed"; exit 2; }
else
    python3 "$REPO_ROOT/scripts/m3_extract_corpus.py" \
        --out "$CORPUS" \
        --sources imessage,memory_db \
        --max-per-source 200 || {
            echo "  H1 failed (FDA missing? no chat.db?)"; exit 2; }
fi
require_lines "$CORPUS" 1 || exit 2

banner "STEP 2 — H2 counterfactual pairs (no-llm path)"
python3 "$REPO_ROOT/scripts/m3_generate_counterfactuals.py" \
    --corpus "$CORPUS" \
    --out "$CF_PAIRS" \
    --no-llm \
    --max-records 50 || { echo "  H2 failed"; exit 2; }
require_lines "$CF_PAIRS" 1 || exit 2

banner "STEP 3 — H3 active probe (queue write)"
# Use --delivery=queue so the entry lands in the queue file with full
# user_message + candidates metadata (the schema H3b's collector needs)
python3 "$REPO_ROOT/scripts/m3_active_probe.py" \
    --corpus "$CORPUS" \
    --queue "$PROBE_QUEUE" \
    --pairs-out "$PROBE_PAIRS" \
    --delivery queue \
    --gateway-url "http://127.0.0.1:1" || {
        echo "  H3 failed"; exit 2; }
require_lines "$PROBE_QUEUE" 1 || exit 2

banner "STEP 4 — H3b collector consumes queue (simulated reply 'A')"
python3 "$REPO_ROOT/scripts/m3_probe_collector.py" \
    --queue "$PROBE_QUEUE" \
    --pairs-out "$PROBE_PAIRS" \
    --mode simulate-tick \
    --simulate-response "A" || { echo "  H3b failed"; exit 2; }
require_lines "$PROBE_PAIRS" 1 || exit 2

banner "STEP 5 — merge H2 + H3b pairs into combined training set"
: > "$COMBINED"
cat "$CF_PAIRS" >> "$COMBINED"
cat "$PROBE_PAIRS" >> "$COMBINED"
require_lines "$COMBINED" 2 || exit 2

banner "STEP 6 — verify shape of combined preference set"
python3 - "$COMBINED" <<'PY' || exit 2
import json, sys
path = sys.argv[1]
ok = bad = 0
sources = {}
for line in open(path):
    line = line.strip()
    if not line: continue
    r = json.loads(line)
    if {"prompt", "chosen", "rejected"} <= set(r):
        ok += 1
        sources[r.get("_source", "(none)")] = sources.get(r.get("_source", "(none)"), 0) + 1
    else:
        bad += 1
print(f"  {ok} well-formed pairs, {bad} malformed")
print(f"  by source: {sources}")
if bad > 0 or ok == 0:
    sys.exit(2)
PY

banner "RESULT"
echo "  Corpus:          $(wc -l < "$CORPUS" | tr -d ' ') records"
echo "  H2 pairs:        $(wc -l < "$CF_PAIRS" | tr -d ' ')"
echo "  H3b pairs:       $(wc -l < "$PROBE_PAIRS" | tr -d ' ')"
echo "  Combined train:  $(wc -l < "$COMBINED" | tr -d ' ')"
echo ""
echo "  $COMBINED is the ready-to-train Alpaca-DPO artifact."
echo "  Feed to: human ml dpo-train --pairs $COMBINED"
echo ""
echo "  ✓ H-tier demo complete"
exit 0
