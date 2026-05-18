#!/usr/bin/env bash
# Phase C live-fire — full closed loop with REAL training + A/B eval.
#
# Beyond live_fire_m3_loop.sh which uses --simulate-train, this script:
#   1. Reuses live_fire_m3_loop.sh's bootstrap (stub MLX + daemon + chat)
#   2. After outcomes accumulate, invokes training_loop.py for REAL LoRA
#      training (lora-persona, ~1 second wall clock against reference GPT)
#   3. Runs m3_eval_adapter.py against the baseline (empty-tensors stub)
#      and the candidate (real LoRA) — should report PASS
#
# Proves C3+C4+C5 wire end-to-end. C6 (the ASan finding) is sidecar and
# doesn't block this script.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$REPO/build-release/human"
[[ -x "$BIN" ]] || { echo "missing $BIN — run 'make demo-loop-build'"; exit 2; }

echo "═══ Phase C full-loop live-fire ═══"

# Step 1: lean on existing live-fire to populate outcomes JSONL
echo
echo "--- Step 1: produce outcomes via existing live-fire ---"
bash "$REPO/scripts/live_fire_m3_loop.sh" > /tmp/c-full-stage1.log 2>&1 || true
JSONL="$HOME/.human/training-data/m3-outcomes.jsonl"
[[ -f "$JSONL" ]] || { echo "no outcomes JSONL at $JSONL"; exit 3; }
echo "    outcomes JSONL: $JSONL ($(grep -c '^{' "$JSONL") lines)"

# Step 1b: deferred until after step 1d (we want the baseline to point
# at the same fixture DB as the candidate for a clean A/B).

echo
echo "--- Step 1c: settle (wait for SQLite WAL locks to clear) ---"
# The previous live-fire's daemon held memory.db's WAL lock; hard-kill
# doesn't immediately release it. lora-persona will hit I/O error
# trying to read the DB if we don't wait. 2s is enough on macOS.
for _ in 1 2 3 4 5; do
    if ! pgrep -f "human service" >/dev/null && ! pgrep -f "human-daemon" >/dev/null; then
        break
    fi
    sleep 1
done
sleep 2
echo "    daemon settled"

echo
echo "--- Step 1d: build a FIXTURE memory.db (deterministic, isolates from prod) ---"
# lora-persona's --from-history reads from a memory.db. Using the
# production ~/.human/memory.db has non-deterministic behavior across
# live-fire runs (the daemon writes between invocations, sometimes
# pushing the PII / quality / dedup filters past their headroom and
# leaving zero valid examples → spurious I/O error). A fresh fixture
# DB with 60 known-clean rows lets the trainer reliably extract 32
# examples every run.
FIXTURE_DB=/tmp/m3-fixture-memory.db
rm -f "$FIXTURE_DB"
sqlite3 "$FIXTURE_DB" <<'SQL'
CREATE TABLE messages(id INTEGER PRIMARY KEY AUTOINCREMENT,
                     session_id TEXT NOT NULL,
                     role TEXT NOT NULL,
                     content TEXT NOT NULL,
                     created_at TEXT DEFAULT(datetime('now')));
SQL
# Insert 30 user / 30 assistant rows of varying length. Plain-ASCII,
# no PII patterns, well over the entropy + length floors lora-persona
# applies. The exact text doesn't matter for proving the wire — we
# just need the trainer to extract a non-zero batch.
python3 -c "
import sqlite3
conn = sqlite3.connect('$FIXTURE_DB')
samples = [
    'How was your weekend? Anything fun?',
    'Caught the late showing, popcorn was solid.',
    'Did you finish the kitchen project yet?',
    'Yeah, mostly, just need to seal the counter.',
    'Sending you the playlist later tonight.',
    'Pumped, been waiting on that one all week.',
    'Got the meeting moved to Thursday afternoon.',
    'Works for me, I will block the calendar.',
    'The bus was packed this morning, eesh.',
    'Tell me about it, I gave up and walked.',
]
for i, content in enumerate(samples * 6):
    role = 'user' if i % 2 == 0 else 'assistant'
    conn.execute('INSERT INTO messages(session_id, role, content) VALUES (?, ?, ?)',
                 ('s1', role, content))
conn.commit()
conn.close()
print(f'  fixture db: $FIXTURE_DB ({60} rows)')
"

echo
echo "--- Step 1b: produce BASELINE via training_loop.py --dry-run ---"
BASELINE="$HOME/.human/training-data/adapters/baseline-empty.safetensors"
python3 "$REPO/scripts/training_loop.py" \
    --source-jsonl "$JSONL" \
    --adapter-out "$BASELINE" \
    --memory-db "$FIXTURE_DB" \
    --dry-run \
    > /tmp/c-full-stage1b.log 2>&1
[[ -f "$BASELINE" ]] || { echo "dry-run baseline not produced"; exit 3; }
echo "    baseline (dry-run): $BASELINE ($(wc -c < "$BASELINE") bytes)"

echo
echo "--- Step 2: REAL training via training_loop.py --source-jsonl (C3+C4) ---"
CANDIDATE="$HOME/.human/training-data/adapters/candidate-real.bin"
python3 "$REPO/scripts/training_loop.py" \
    --source-jsonl "$JSONL" \
    --adapter-out "$CANDIDATE" \
    --memory-db "$FIXTURE_DB" \
    2>&1 | tee /tmp/c-full-stage2.log | tail -10
[[ -f "$CANDIDATE" ]] || { echo "training did not produce candidate"; exit 4; }
echo "    candidate (real): $CANDIDATE ($(wc -c < "$CANDIDATE") bytes)"

echo
echo "--- Step 3: A/B eval (C5) — should report PASS (real > empty) ---"
VERDICT_JSON=/tmp/c-full-verdict.json
python3 "$REPO/scripts/m3_eval_adapter.py" \
    --baseline "$BASELINE" \
    --candidate "$CANDIDATE" \
    --judge metadata \
    --json-out "$VERDICT_JSON" \
    2>&1 | tail -10

VERDICT=$(python3 -c "import json; print(json.load(open('$VERDICT_JSON'))['verdict'])")
echo
if [[ "$VERDICT" == "pass" ]]; then
    echo "═══ ✅ FULL C3+C4+C5 LOOP PROVEN END-TO-END ═══"
    echo "    Verdict: PASS  ($VERDICT_JSON)"
    exit 0
else
    echo "═══ ❌ Unexpected verdict: $VERDICT ═══"
    cat "$VERDICT_JSON"
    exit 5
fi
