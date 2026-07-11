#!/usr/bin/env python3
"""rating_drip — measurement-as-conversation for the blind A/B keystone.

The 12-row rating sheet sat unrated for a month because it's homework. This
drip serves it ONE question at a time to Seth's self-chat (sethford@me.com):

    which sounds more like you?
    <context>
    A) <option_A>
    B) <option_B>
    (reply A or B, optionally + confidence 1-5, e.g. "A 4")

Seth taps back "A" or "B"; the answer is harvested from chat.db and written
into rating_sheet.csv. When every row is rated, score.py runs automatically
and writes ~/.human/blind_ab_gate.json — the verdict the LoRA promotion gate
reads. Human-grounded eval as a byproduct of texting.

Design rules:
  - NEVER more than one unanswered question outstanding (no nagging).
  - Sends only inside waking hours (09:00-21:00 local).
  - Self-chat only; the target is pinned in state and never inferred from
    message content (prompt-injection hygiene).
  - Pure helpers are import-testable; the send is `imsg send` (same CLI the
    daemon uses) and is skipped under --dry-run / HU_IS_TEST.

Usage:
  python3 rating_drip.py tick        # ingest any answer, then maybe send one question
  python3 rating_drip.py status      # show progress
  python3 rating_drip.py tick --dry-run
"""
import csv
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import time

HOME = os.path.expanduser("~")
SHEET_DIR = os.path.join(HOME, ".human", "blind_ab_human")
SHEET = os.path.join(SHEET_DIR, "rating_sheet.csv")
ANSWER_KEY = os.path.join(SHEET_DIR, "answer_key.json")
STATE = os.path.join(SHEET_DIR, "drip_state.json")
CHAT_DB = os.path.join(HOME, "Library", "Messages", "chat.db")
SCORE_PY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "score.py")

DEFAULT_TARGET = "sethford@me.com"  # Seth's self-chat (notes-to-self)
APPLE_EPOCH = 978307200  # 2001-01-01 in unix seconds
SEND_HOUR_START = 9
SEND_HOUR_END = 21  # exclusive

# ── pure helpers (unit-tested) ──────────────────────────────────────────


def load_sheet(path=SHEET):
    with open(path, newline="") as f:
        return list(csv.DictReader(f)), csv.DictReader(open(path)).fieldnames


def next_unanswered(rows, skipped=None):
    """First row whose choice is blank and not drip-skipped; None when done."""
    skipped = skipped or []
    for r in rows:
        if r.get("id") in skipped:
            continue
        if not (r.get("choice") or "").strip():
            return r
    return None


def compose_question(row, answered, total):
    """One compact self-chat question. Keeps A/B labels adjacent to text so a
    one-word reply is unambiguous."""
    return (
        f"[h-uman rating {answered + 1}/{total}] which sounds more like you?\n"
        f"them: {row['context']}\n"
        f"A) {row['option_A']}\n"
        f"B) {row['option_B']}\n"
        f"reply A or B (optionally + 1-5 confidence, e.g. \"A 4\")"
    )


ANSWER_RE = re.compile(r"^\s*([ABab])\s*[\),.:]?\s*([1-5])?\s*$")


def parse_answer(text):
    """Strict-start A/B parser. Returns (choice, confidence) or None.
    Accepts: "A", "b", "A 4", "B)", "a5". Rejects prose ("maybe A?"),
    anything long, and the drip's own question text."""
    if not text or len(text) > 12:
        return None
    m = ANSWER_RE.match(text)
    if not m:
        return None
    return m.group(1).upper(), int(m.group(2)) if m.group(2) else 3


def within_send_hours(hour):
    return SEND_HOUR_START <= hour < SEND_HOUR_END


def apple_ts_to_unix(apple_ns):
    """chat.db message.date is nanoseconds since 2001-01-01."""
    return apple_ns / 1e9 + APPLE_EPOCH


def write_choice(path, row_id, choice, confidence):
    """Persist one answer into the sheet (atomic rewrite)."""
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
    if not hit:
        return False
    tmp = path + ".tmp"
    with open(tmp, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)
    os.replace(tmp, path)
    return True


# ── state ───────────────────────────────────────────────────────────────


def load_state():
    try:
        with open(STATE) as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return {"target": DEFAULT_TARGET, "pending_row": None, "question_unix": 0,
                "sent": 0, "answered": 0, "complete": False}


def save_state(st):
    os.makedirs(SHEET_DIR, exist_ok=True)
    tmp = STATE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(st, f, indent=1)
    os.replace(tmp, STATE)


# ── chat.db ingest ──────────────────────────────────────────────────────


REASK_AFTER_SECS = 24 * 3600
MAX_ASKS_PER_ROW = 3  # 1 original + 2 re-asks, then the row is skipped


def should_reask(now_unix, question_unix, asks):
    """Re-send the pending question after 24h of silence, up to MAX_ASKS."""
    return question_unix > 0 and (now_unix - question_unix) >= REASK_AFTER_SECS \
        and asks < MAX_ASKS_PER_ROW


def first_answer_after(rows_desc, since_unix, decoder=None):
    """Pure: rows_desc = [(text, attr_blob, apple_ns), ...] newest-first.
    Returns the EARLIEST A/B-shaped message after since_unix (first-reply
    semantics — a stray later "A" note-to-self must not override the actual
    first response), or None."""
    best = None
    for text, attr_blob, apple_ns in rows_desc:
        if apple_ts_to_unix(apple_ns) <= since_unix:
            break
        if not text and attr_blob and decoder:
            text = decoder(attr_blob)
        parsed = parse_answer(text)
        if parsed:
            best = parsed  # keep overwriting: DESC order => last hit is earliest
    return best


def harvest_answer(target, since_unix, db_path=CHAT_DB):
    """Newest short A/B-shaped message in the target chat after since_unix.
    Self-chat means both directions are 'from me' — the strict parser is what
    separates the answer from the drip's own (long) question.

    Modern macOS often stores the body in attributedBody with text=NULL
    (this is why a text-only query missed the drip's OWN sent question), so
    decode that as the fallback via the exporter's proven decoder."""
    q = (
        "SELECT m.text, m.attributedBody, m.date FROM message m "
        "JOIN chat_message_join cmj ON cmj.message_id = m.ROWID "
        "JOIN chat c ON c.ROWID = cmj.chat_id "
        "WHERE c.chat_identifier = ? "
        "ORDER BY m.date DESC LIMIT 40"
    )
    try:
        con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
        rows = con.execute(q, (target,)).fetchall()
        con.close()
    except sqlite3.Error:
        return None
    try:
        from export_seth_triples import decode_attributed_body
    except ImportError:
        decode_attributed_body = None
    return first_answer_after(rows, since_unix, decoder=decode_attributed_body)


# ── send ────────────────────────────────────────────────────────────────


def imsg_bin():
    """Resolve the imsg CLI explicitly. launchd jobs don't inherit the
    interactive PATH (no /opt/homebrew/bin), and a bare "imsg" then raises
    FileNotFoundError, which killed every re-ask tick for 4 days while the
    state sat at sent=1/answered=0 (observed 2026-07-05..09)."""
    found = shutil.which("imsg")
    if found:
        return found
    for candidate in ("/opt/homebrew/bin/imsg", "/usr/local/bin/imsg"):
        if os.path.exists(candidate):
            return candidate
    return "imsg"


def send_question(target, text, dry_run=False):
    if dry_run or os.environ.get("HU_IS_TEST"):
        print(f"[dry-run] would send to {target}:\n{text}")
        return True
    try:
        r = subprocess.run([imsg_bin(), "send", "--to", target, "--text", text],
                           capture_output=True, text=True, timeout=30)
    except FileNotFoundError:
        # A missing CLI must degrade to "send failed" (retry next tick),
        # never kill the tick before the state is saved.
        print("send failed: imsg CLI not found on PATH or in Homebrew bins",
              file=sys.stderr)
        return False
    if r.returncode != 0:
        print(f"send failed: {r.stderr.strip()[:200]}", file=sys.stderr)
        return False
    return True


# ── the tick ────────────────────────────────────────────────────────────


def run_score():
    r = subprocess.run([sys.executable, SCORE_PY, SHEET, ANSWER_KEY],
                       capture_output=True, text=True, timeout=60)
    print(r.stdout[-500:] if r.stdout else r.stderr[-300:])
    return r.returncode == 0


def tick(dry_run=False, now=None):
    now = now if now is not None else time.time()
    st = load_state()
    rows, _ = load_sheet()
    total = len(rows)

    # 1) ingest a pending answer, if any
    if st.get("pending_row") and st.get("question_unix"):
        ans = harvest_answer(st["target"], st["question_unix"])
        if ans:
            choice, conf = ans
            if write_choice(SHEET, st["pending_row"], choice, conf):
                print(f"ingested: row {st['pending_row']} = {choice} (conf {conf})")
                st["answered"] += 1
                st["pending_row"] = None
                st["question_unix"] = 0
                st["asks"] = 1
                rows, _ = load_sheet()  # reload with the new answer

    # 2) complete? run the scorer -> writes ~/.human/blind_ab_gate.json
    if next_unanswered(rows, st.get("skipped")) is None:
        if not st.get("complete"):
            print(f"sheet complete ({total}/{total}) — running score.py -> gate verdict")
            run_score()
            st["complete"] = True
        save_state(st)
        return

    # 3) send at most one question, only when none pending, only waking hours.
    #    A pending question silent for 24h is re-asked (max 3 asks total),
    #    then its row is drip-skipped so one dead question can't stall the
    #    whole sheet.
    in_hours = within_send_hours(time.localtime(now).tm_hour)
    if st.get("pending_row"):
        asks = st.get("asks", 1)
        if should_reask(now, st.get("question_unix", 0), asks) and in_hours:
            row = next((r for r in rows if r["id"] == st["pending_row"]), None)
            if row:
                answered = sum(1 for r in rows if (r.get("choice") or "").strip())
                if send_question(st["target"], compose_question(row, answered, total),
                                 dry_run=dry_run):
                    st["asks"] = asks + 1
                    st["question_unix"] = now
                    print(f"re-asked row {st['pending_row']} (ask {asks + 1}/{MAX_ASKS_PER_ROW})")
        elif st.get("question_unix", 0) > 0 and \
                (now - st["question_unix"]) >= REASK_AFTER_SECS and \
                st.get("asks", 1) >= MAX_ASKS_PER_ROW:
            skipped = st.setdefault("skipped", [])
            skipped.append(st["pending_row"])
            print(f"row {st['pending_row']} unanswered after {MAX_ASKS_PER_ROW} asks — skipping")
            st["pending_row"] = None
            st["question_unix"] = 0
            st["asks"] = 1
        else:
            print(f"waiting on answer for row {st['pending_row']} — not re-asking yet")
    elif not in_hours:
        print("outside send hours (09-21 local) — skipping")
    else:
        row = next_unanswered(rows, st.get("skipped"))
        answered = sum(1 for r in rows if (r.get("choice") or "").strip())
        q = compose_question(row, answered, total)
        if send_question(st["target"], q, dry_run=dry_run):
            st["pending_row"] = row["id"]
            st["question_unix"] = now
            st["asks"] = 1
            st["sent"] += 1
            print(f"sent question for row {row['id']} ({answered + 1}/{total})")
    save_state(st)


def status():
    st = load_state()
    rows, _ = load_sheet()
    answered = sum(1 for r in rows if (r.get("choice") or "").strip())
    print(f"rated {answered}/{len(rows)} | pending: {st.get('pending_row')} | "
          f"sent: {st.get('sent', 0)} | complete: {st.get('complete', False)}")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "tick"
    dry = "--dry-run" in sys.argv
    if cmd == "tick":
        tick(dry_run=dry)
    elif cmd == "status":
        status()
    else:
        print(__doc__)
