#!/usr/bin/env python3
"""rating_ingest — harvest Seth's replies from chat.db and write into rating sheet.

The rating_drip sends one blind-A/B question per day to Seth's self-chat
(sethford@me.com). This script reads his replies and ingests them into the
rating_sheet.csv.

Ingest is idempotent: re-running never double-ingests (tracks ingested message
ROWIDs in the state file). When all rows are rated, score.py is invoked to
compute results and update docs/evaluation/blind_ab_gate.json.

Usage:
  python3 rating_ingest.py ingest        # harvest pending answers
  python3 rating_ingest.py status        # show progress + pending answers
  python3 rating_ingest.py ingest --dry-run
"""
import csv
import json
import os
import sqlite3
import subprocess
import sys

# One parser, one clock: rating_drip owns the answer grammar (lenient but
# enumerable — "A", "option a", "the first one 4") so the send and harvest
# halves can never drift apart on what counts as an answer.
from rating_drip import APPLE_EPOCH, apple_ts_to_unix, parse_answer  # noqa: F401

HOME = os.path.expanduser("~")
SHEET_DIR = os.path.join(HOME, ".human", "blind_ab_human")
SHEET = os.path.join(SHEET_DIR, "rating_sheet.csv")
ANSWER_KEY = os.path.join(SHEET_DIR, "answer_key.json")
STATE = os.path.join(SHEET_DIR, "drip_state.json")
CHAT_DB = os.path.join(HOME, "Library", "Messages", "chat.db")
SCORE_PY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "score.py")

DEFAULT_TARGET = "sethford@me.com"  # Seth's self-chat


def load_sheet(path=None):
    """Load rating sheet CSV as list of dicts."""
    path = path or SHEET  # resolved at call time so tests can patch SHEET
    if not os.path.exists(path):
        return [], []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        return list(reader), reader.fieldnames or []


def load_state():
    """Load drip state (pending_row, question_unix, etc.)."""
    try:
        with open(STATE) as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return {
            "target": DEFAULT_TARGET,
            "pending_row": None,
            "question_unix": 0,
            "ingested_rowids": [],  # Track which message ROWIDs we've ingested
        }


def save_state(st):
    """Persist drip state."""
    os.makedirs(SHEET_DIR, exist_ok=True)
    tmp = STATE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(st, f, indent=1)
    os.replace(tmp, STATE)


def msg_text(text, body):
    """Extract text from a message, preferring text over attributedBody."""
    if text and text.strip():
        return text.strip()
    # Import the shared decoder
    try:
        from imessage_text import decode_attributed_body
        return decode_attributed_body(body)
    except ImportError:
        return None


def query_chat_replies(target, since_unix, db_path=CHAT_DB):
    """Query chat.db for Seth's replies in the target chat after since_unix.

    Returns list of (message_rowid, text, attributedBody, apple_ns) tuples
    in reverse chronological order (newest first). Self-chat means both
    directions are is_from_me=1 — the drip sends questions, Seth replies
    both sending back the choice.
    """
    q = (
        "SELECT m.ROWID, m.text, m.attributedBody, m.date FROM message m "
        "JOIN chat_message_join cmj ON cmj.message_id = m.ROWID "
        "JOIN chat c ON c.ROWID = cmj.chat_id "
        "WHERE c.chat_identifier = ? AND m.is_from_me = 1 "
        "ORDER BY m.date DESC LIMIT 40"
    )
    try:
        con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
        rows = con.execute(q, (target,)).fetchall()
        con.close()
    except sqlite3.Error as e:
        print(f"chat.db query failed: {e}", file=sys.stderr)
        return []
    return rows


def first_answer_after(rows_desc, since_unix):
    """Find the earliest A/B-shaped message after since_unix.

    rows_desc = [(rowid, text, attr_blob, apple_ns), ...] newest-first.
    Returns (rowid, choice, confidence) for the first reply matching the
    A/B pattern, or None. Descending time order => last hit is earliest.
    """
    best = None
    for rowid, text, attr_blob, apple_ns in rows_desc:
        if apple_ts_to_unix(apple_ns) <= since_unix:
            break
        content = msg_text(text, attr_blob)
        parsed = parse_answer(content)
        if parsed:
            best = (rowid, parsed[0], parsed[1])
    return best


def ingest_answer(target, since_unix, sheet_rows, db_path=CHAT_DB):
    """Harvest one A/B answer from chat.db after since_unix.

    Returns (message_rowid, choice, confidence) or None.
    Skips ambiguous/unmatched replies by returning None.
    """
    rows = query_chat_replies(target, since_unix, db_path)
    return first_answer_after(rows, since_unix)


def write_choice(path, row_id, choice, confidence):
    """Persist one answer into the rating sheet (atomic rewrite)."""
    if not os.path.exists(path):
        print(f"rating sheet not found: {path}", file=sys.stderr)
        return False
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        fields = reader.fieldnames
        rows = list(reader)
    hit = False
    for r in rows:
        if r["id"] == row_id:
            r["choice"] = choice
            r["confidence"] = str(confidence)
            hit = True
            break
    if not hit:
        return False
    tmp = path + ".tmp"
    with open(tmp, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)
    os.replace(tmp, path)
    return True


def run_score():
    """Invoke score.py to compute results and update blind_ab_gate.json."""
    if not os.path.exists(ANSWER_KEY):
        print("answer_key.json not found; score.py cannot run", file=sys.stderr)
        return False
    try:
        r = subprocess.run(
            [sys.executable, SCORE_PY, SHEET, ANSWER_KEY],
            capture_output=True,
            text=True,
            timeout=60,
        )
        # Print last 500 chars of output for visibility
        if r.stdout:
            print(r.stdout[-500:])
        if r.returncode != 0:
            if r.stderr:
                print(f"score.py failed: {r.stderr[-300:]}", file=sys.stderr)
            return False
        return True
    except subprocess.TimeoutExpired:
        print("score.py timed out", file=sys.stderr)
        return False
    except Exception as e:
        print(f"score.py invocation failed: {e}", file=sys.stderr)
        return False


def ingest(dry_run=False):
    """Main ingest loop: read drip state, harvest answers, update sheet, score if done."""
    st = load_state()
    rows, _ = load_sheet()
    if not rows:
        print("rating sheet not found or empty", file=sys.stderr)
        return

    total = len(rows)
    ingested = st.get("ingested_rowids", [])

    # 1. If there's a pending question, try to ingest an answer
    if st.get("pending_row") and st.get("question_unix"):
        ans = ingest_answer(
            st["target"], st["question_unix"], rows, db_path=CHAT_DB
        )
        if ans:
            msg_rowid, choice, conf = ans
            if msg_rowid not in ingested:
                if not dry_run and write_choice(SHEET, st["pending_row"], choice, conf):
                    ingested.append(msg_rowid)
                    st["ingested_rowids"] = ingested
                    print(f"ingested: row {st['pending_row']} = {choice} (conf {conf})")
                    st["answered"] = st.get("answered", 0) + 1  # the sensor reads this
                    st["pending_row"] = None
                    st["question_unix"] = 0
                    st["asks"] = 1
                    # Reload sheet with the new answer
                    rows, _ = load_sheet()
                elif dry_run:
                    print(
                        f"[dry-run] would ingest: row {st['pending_row']} = {choice} (conf {conf})"
                    )
            else:
                print(f"already ingested ROWID {msg_rowid}; skipping")
        else:
            print(f"no answer found for pending row {st['pending_row']}")

    # 2. Check if complete (all rated)
    answered = sum(1 for r in rows if (r.get("choice") or "").strip())
    if answered >= total:
        if not st.get("complete"):
            print(
                f"sheet complete ({answered}/{total}) — running score.py -> gate verdict"
            )
            if not dry_run:
                run_score()
                st["complete"] = True
        save_state(st)
        return

    # 3. Report status
    pending = st.get("pending_row")
    if pending:
        print(f"waiting on answer for row {pending} ({answered}/{total} rated so far)")
    else:
        print(f"ready for next question ({answered}/{total} rated so far)")

    save_state(st)


def status():
    """Show rating progress and pending answers."""
    st = load_state()
    rows, _ = load_sheet()
    if not rows:
        print("rating sheet not found or empty")
        return

    answered = sum(1 for r in rows if (r.get("choice") or "").strip())
    total = len(rows)
    pending = st.get("pending_row")
    complete = st.get("complete", False)

    print(f"Progress: {answered}/{total} rated | pending: {pending} | complete: {complete}")

    # Show unanswered rows
    unanswered = [r for r in rows if not (r.get("choice") or "").strip()]
    if unanswered:
        print(f"\nUnanswered rows ({len(unanswered)}):")
        for r in unanswered[:5]:  # Show first 5
            print(f"  {r['id']}: {r['context'][:60]}...")
        if len(unanswered) > 5:
            print(f"  ... and {len(unanswered) - 5} more")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "ingest"
    dry = "--dry-run" in sys.argv
    if cmd == "ingest":
        ingest(dry_run=dry)
    elif cmd == "status":
        status()
    else:
        print(__doc__)
