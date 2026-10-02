#!/usr/bin/env python3
"""rating_drip — measurement-as-conversation for the blind A/B keystone.

The 12-row rating sheet sat unrated for a month because it's homework. This
drip serves it ONE question at a time to Seth's self-chat (his own number):

    which sounds more like you?
    <context>
    A) <option_A>
    B) <option_B>
    (reply A or B, optionally + confidence 1-5, e.g. "A 4")

Seth taps back "A" or "B"; the answer is harvested from chat.db and written
into rating_sheet.csv. When every row is rated, score.py runs automatically
and writes ~/.human/blind_ab_gate.json — the verdict the LoRA promotion gate
reads. Human-grounded eval as a byproduct of texting.

Once the detection pass is answered for a sheet, a SECOND pass (if enabled)
asks a different question over the same items, re-randomizing which letter
holds which reply so the answer can't be recalled from the first pass:

    which reply is BETTER for this person — more caring, more useful,
    more like a great friend?
    <context>
    A) <option>
    B) <option>

    (reply A, B, or T = tie / can't tell)
This measures better-than-human, not just indistinguishable-from-human — a
0.50 detection rate says nothing about whether the reply is actually
preferred. Answers land in the sheet's `better_choice` column; when that
pass completes, better_score.py scores it into
~/.human/blind_ab_better.json — a SEPARATE file, never the LoRA promotion
gate. score.py still runs the tick the DETECTION pass completes; it never
waits on the better pass. The pass activates only when drip_state.json's "better_enabled" is
true: a freshly-seeded sheet defaults it on; an existing in-progress sheet
stays off until the owner/lead runs `rating_drip.py enable-better`.

Design rules:
  - NEVER more than one unanswered question outstanding (no nagging).
  - Sends only inside waking hours (09:00-21:00 local).
  - Self-chat only; the target is pinned in state and never inferred from
    message content (prompt-injection hygiene).
  - Pure helpers are import-testable; the send is `imsg send` (same CLI the
    daemon uses) and is skipped under --dry-run / HU_IS_TEST.

Usage:
  python3 rating_drip.py tick            # ingest any answer, then maybe send one question
  python3 rating_drip.py status          # show progress
  python3 rating_drip.py tick --dry-run
  python3 rating_drip.py enable-better   # turn on the "better" pass for the CURRENT sheet
"""
import csv
import json
import os
import random
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
# "better" measurement (US: which reply is BETTER, not just which sounds more
# like you) -- id -> "A"|"B", the displayed letter holding h-uman's reply for
# that specific re-randomized posing. See decide_better_key().
BETTER_KEY = os.path.join(SHEET_DIR, "better_key.json")
CHAT_DB = os.path.join(HOME, "Library", "Messages", "chat.db")
SCORE_PY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "score.py")
BETTER_SCORE_PY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "better_score.py")
# Repo-side gate JSON (human half) — same target the nightly's stage 3 refreshes.
REPO_GATE = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..",
    "docs", "evaluation", "blind_ab_gate.json"))

# Seth's self-chat (notes-to-self), addressed by his own number. The previous
# value, `sethford@me.com`, stopped being an alias on the iMessage account
# around 2026-09-05; from then until 09-19 every question recorded
# is_sent=0 error=22 in chat.db while `imsg send` exited 0, so the drip
# re-asked into the void, skipped 4 rows as "unanswered", and reported 0/48.
# The state file pins the live target; this default only seeds a fresh state.
DEFAULT_TARGET = "+18012017497"
# How long to wait for Messages to write the sent row before calling a send
# unconfirmed. Observed latency is ~1-6 s; a miss here costs one duplicate
# question next tick, a false "sent" costs the whole sheet.
CONFIRM_WAIT_SECS = 10
APPLE_EPOCH = 978307200  # 2001-01-01 in unix seconds
SEND_HOUR_START = 9
SEND_HOUR_END = 21  # exclusive

# ── pure helpers (unit-tested) ──────────────────────────────────────────


def load_sheet(path=None):
    path = path or SHEET  # resolved at call time so tests can patch SHEET
    with open(path, newline="") as f:
        return list(csv.DictReader(f)), csv.DictReader(open(path)).fieldnames


def next_unanswered(rows, skipped=None, field="choice"):
    """First row whose `field` is blank and not drip-skipped; None when done.

    `field` defaults to "choice" (the detection column) so every existing
    caller is unchanged; the "better" pass calls this with
    field="better_choice"."""
    skipped = skipped or []
    for r in rows:
        if r.get("id") in skipped:
            continue
        if not (r.get(field) or "").strip():
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


# Whole-message anchored: the entire (stripped) reply must BE an answer.
# In a self-chat both directions are "from me", so this anchor + the length
# cap are the only boundary between an answer and an ordinary note-to-self.
ANSWER_RE = re.compile(
    r"^(?:"
    r"(?:option\s+)?(?P<letter>[ab])"
    r"|(?:the\s+)?(?P<ordinal>first|1st|second|2nd)(?:\s+one)?"
    r")\s*[\),.:]?\s*(?P<conf>[1-5])?$",
    re.IGNORECASE,
)
MAX_ANSWER_CHARS = 18  # longest legit form: "the second one, 5" (17 chars)


def parse_answer(text):
    """Lenient but enumerable A/B parser. Returns (choice, confidence) or None.
    Accepts: "A", "b", "A 4", "B)", "a5", "option a", "first one",
    "the second one 4". Rejects prose ("maybe A?", "first thing tomorrow"),
    ambiguous forms ("a second", bare digits), anything long, and the drip's
    own question text. Un-parseable replies are handled by the 24h re-ask."""
    if not text:
        return None
    text = text.strip()
    if not text or len(text) > MAX_ANSWER_CHARS:
        return None
    m = ANSWER_RE.match(text)
    if not m:
        return None
    if m.group("letter"):
        choice = m.group("letter").upper()
    else:
        choice = "A" if m.group("ordinal").lower() in ("first", "1st") else "B"
    return choice, int(m.group("conf")) if m.group("conf") else 3


# ── batch mode ──────────────────────────────────────────────────────────
# One question per message, gated on a reply, caps throughput at roughly one
# row per (reply latency + up to one 2h tick): a 48-row sheet takes a week even
# if every question is answered at once. Batching asks several rows in one
# message and takes one reply.
#
# The safety boundary is unchanged in kind: whole-message anchored, exact shape.
# A batch reply is EXACTLY n A/B letters, separated by nothing, spaces or commas.
# Positional mapping means a wrong COUNT would assign answers to the wrong rows,
# so any mismatch is refused, never guessed at.
BATCH_SIZE = max(1, int(os.environ.get("HU_RATING_DRIP_BATCH", "5") or "5"))
# The better question's answers: A, B, or T = tie / can't tell. A forced A/B
# choice coerced genuine indifference into a direction and biased the rate;
# better_score.py excludes T from the rate's denominator and reports it.
BETTER_ANSWER_LETTERS = "ABT"
_BATCH_SEPARATORS = re.compile(r"[\s,]+")


def parse_batch_answer(text, n, allowed="AB"):
    """Exactly n letters from `allowed`, in order. Returns ["A", "B", ...] or None.

    Accepts "ABBAB", "a b b a b", "A, B, B, A, B". Rejects any other count, any
    other character, and prose — which is what keeps an ordinary note-to-self
    (or the drip reading its own long question back) from parsing as ratings.
    The detection pass keeps the default "AB"; the better pass passes
    BETTER_ANSWER_LETTERS so "T" (tie / can't tell) is a legal answer."""
    if not text or n < 1:
        return None
    stripped = text.strip()
    # Generous for "A, B, B, A, B" (3 chars per answer) yet far below any prose
    # that could carry n A/B letters by coincidence.
    if len(stripped) > 3 * n:
        return None
    letters = _BATCH_SEPARATORS.sub("", stripped)
    ok = set(allowed.upper()) | set(allowed.lower())
    if len(letters) != n or any(ch not in ok for ch in letters):
        return None
    return [ch.upper() for ch in letters]


def compose_batch_question(rows, answered, total):
    """Several rows in one self-chat message, numbered, with the exact reply
    shape stated so a one-line reply is unambiguous."""
    n = len(rows)
    first, last = answered + 1, answered + n
    example = ("ABBAB" * n)[:n]
    parts = [
        f"[h-uman rating {first}-{last}/{total}] which sounds more like you?",
        f"reply with {n} letters in order, e.g. {example}",
    ]
    for i, row in enumerate(rows):
        parts.append(
            f"\n{i + 1}) them: {row['context']}\n"
            f"   A) {row['option_A']}\n"
            f"   B) {row['option_B']}"
        )
    return "\n".join(parts)


# ── "better" question: a SECOND pass over the same items, asked only after
# the detection pass is answered for a given batch. Measures better-than-
# human (not just indistinguishable-from-human) -- see better_score.py. The
# A/B sides are re-randomized independently of the detection labeling (a
# fresh coin flip per row, decided once and persisted) so the owner answers
# from the content, not from memory of which letter was "more like you". ──


def decide_better_key(row_ids, detection_key, better_key_store, rng=None):
    """Assign, for each row id not already decided, which displayed letter
    ("A" or "B") will hold h-uman's reply in the "better" posing -- an
    independent 50/50 draw, unrelated to the detection sheet's labeling.

    Mutates and returns `better_key_store`. A row already present keeps its
    prior decision, so re-asking the same pending batch (a timeout re-ask)
    never re-shuffles which text is under which letter mid-flight -- the
    owner's eventual reply still maps onto the posing they actually read.
    Rows whose id is missing from `detection_key` (or whose value isn't
    "A"/"B") are skipped -- there is no model/real pairing to randomize."""
    rng = rng or random.Random()
    for rid in row_ids:
        if rid in better_key_store:
            continue
        if detection_key.get(rid) not in ("A", "B"):
            continue
        better_key_store[rid] = "A" if rng.random() < 0.5 else "B"
    return better_key_store


def better_display_options(row, detection_key, better_key_store):
    """Return (display_A_text, display_B_text) for the "better" question,
    honoring the independently-randomized letter assignment in
    `better_key_store` -- NOT the detection sheet's original A/B order."""
    rid = row["id"]
    seth_letter = detection_key.get(rid)
    seth_text = row["option_A"] if seth_letter == "A" else row["option_B"]
    model_text = row["option_B"] if seth_letter == "A" else row["option_A"]
    model_displayed = better_key_store.get(rid)
    if model_displayed == "A":
        return model_text, seth_text
    return seth_text, model_text


def compose_better_question(rows, detection_key, better_key_store, answered, total):
    """Several rows in one self-chat message, re-randomized A/B, asking
    which reply is BETTER (not which sounds more like Seth)."""
    n = len(rows)
    first, last = answered + 1, answered + n
    example = ("ABTAB" * n)[:n]
    parts = [
        f"[h-uman rating {first}-{last}/{total}] which reply is BETTER for "
        f"this person -- more caring, more useful, more like a great friend?",
        f"reply with {n} letters in order (A, B, or T = tie / can't tell), e.g. {example}",
    ]
    for i, row in enumerate(rows):
        a_text, b_text = better_display_options(row, detection_key, better_key_store)
        parts.append(
            f"\n{i + 1}) them: {row['context']}\n"
            f"   A) {a_text}\n"
            f"   B) {b_text}"
        )
    return "\n".join(parts)


def pending_ids(st):
    """Row ids awaiting an answer. Migrates the legacy single `pending_row`
    (the live state file carried one when batching shipped) into a batch of
    one, so an in-flight question is never stranded by the schema change."""
    rows = st.get("pending_rows") or []
    if rows:
        return list(rows)
    single = st.get("pending_row")
    return [single] if single else []


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


def write_better_choice(path, row_id, choice):
    """Persist one "better" answer into the sheet (atomic rewrite).

    Backward compatible with sheets that have no `better_choice` column:
    the column is appended to the fieldnames on first write, and every
    other row is written with it blank (csv.DictWriter's default
    restval='' for a key missing from a row dict) -- a sheet in flight on
    the detection-only drip is never disturbed until a "better" answer is
    actually written into it."""
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        fields = list(reader.fieldnames or [])
        rows = list(reader)
    if "better_choice" not in fields:
        fields = fields + ["better_choice"]
    hit = False
    for r in rows:
        if r["id"] == row_id:
            r["better_choice"] = choice
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
        # better_enabled defaults ON for a freshly-seeded drip (no state file
        # yet). An EXISTING drip_state.json that predates this feature loads
        # successfully above and simply lacks the key -- st.get("better_enabled")
        # then reads False, so the in-progress sheet is untouched until the
        # owner/lead explicitly runs `rating_drip.py enable-better`.
        return {"target": DEFAULT_TARGET, "pending_row": None, "question_unix": 0,
                "sent": 0, "answered": 0, "complete": False, "better_enabled": True}


def save_state(st):
    os.makedirs(SHEET_DIR, exist_ok=True)
    tmp = STATE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(st, f, indent=1)
    os.replace(tmp, STATE)


def load_better_key():
    try:
        with open(BETTER_KEY) as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def save_better_key(d):
    os.makedirs(SHEET_DIR, exist_ok=True)
    tmp = BETTER_KEY + ".tmp"
    with open(tmp, "w") as f:
        json.dump(d, f, indent=1)
    os.replace(tmp, BETTER_KEY)


def load_detection_key():
    try:
        with open(ANSWER_KEY) as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


# ── chat.db ingest ──────────────────────────────────────────────────────


REASK_AFTER_SECS = 24 * 3600
MAX_ASKS_PER_ROW = 3  # 1 original + 2 re-asks, then the row is skipped


def should_reask(now_unix, question_unix, asks):
    """Re-send the pending question after 24h of silence, up to MAX_ASKS."""
    return question_unix > 0 and (now_unix - question_unix) >= REASK_AFTER_SECS \
        and asks < MAX_ASKS_PER_ROW


def first_answer_after(rows_desc, since_unix, decoder=None, parser=None):
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
        parsed = (parser or parse_answer)(text)
        if parsed:
            best = parsed  # keep overwriting: DESC order => last hit is earliest
    return best


def harvest_answer(target, since_unix, db_path=CHAT_DB, n=1, allowed="AB"):
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
    if n <= 1 and allowed == "AB":
        # Unchanged contract: (choice, conf) or None. voice_ab.py calls this
        # with the default n and depends on that tuple shape.
        return first_answer_after(rows, since_unix, decoder=decode_attributed_body)
    letters = first_answer_after(rows, since_unix, decoder=decode_attributed_body,
                                 parser=lambda t: parse_batch_answer(t, max(1, n), allowed))
    return [(c, 3) for c in letters] if letters else None


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


def delivery_verdict(rows_desc, since_unix):
    """Pure. rows_desc = [(is_sent, error, apple_ns), ...] — the from-me rows
    in the target chat, newest first. Judges only rows written after
    since_unix (this send); an older failed row is not this send's failure.

    Returns ("delivered", 0) | ("failed", <chat.db error>) | ("pending", None).
    error=22 is what Messages records for a recipient that is not registered
    with iMessage — a dead alias looks exactly like `test@example.com`."""
    for is_sent, error, apple_ns in rows_desc:
        if apple_ts_to_unix(apple_ns) < since_unix:
            break
        if error:
            return ("failed", int(error))
        if is_sent:
            return ("delivered", 0)
    return ("pending", None)


def confirm_delivery(target, since_unix, db_path=CHAT_DB, wait_secs=CONFIRM_WAIT_SECS):
    """Read the artifact, not the exit code: poll chat.db for the row Messages
    wrote for this send and return delivery_verdict() on it. Gives up as
    ("pending", None) after wait_secs so a slow write costs at most one
    duplicate question on the next tick, never a phantom "sent"."""
    q = (
        "SELECT m.is_sent, m.error, m.date FROM message m "
        "JOIN chat_message_join cmj ON cmj.message_id = m.ROWID "
        "JOIN chat c ON c.ROWID = cmj.chat_id "
        "WHERE c.chat_identifier = ? AND m.is_from_me = 1 "
        "ORDER BY m.date DESC LIMIT 5"
    )
    deadline = time.time() + wait_secs
    while True:
        try:
            con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
            rows = con.execute(q, (target,)).fetchall()
            con.close()
        except sqlite3.Error:
            rows = []
        verdict = delivery_verdict(rows, since_unix)
        if verdict[0] != "pending" or time.time() >= deadline:
            return verdict
        time.sleep(1)


def send_question(target, text, dry_run=False):
    if dry_run or os.environ.get("HU_IS_TEST"):
        print(f"[dry-run] would send to {target}:\n{text}")
        return True
    # One second of slack: Messages stamps the row at its own clock, and the
    # verdict must not miss a row written a few ms before this timestamp.
    sent_at = time.time() - 1.0
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
    # imsg exit 0 means Messages ACCEPTED the message, not that it left the
    # machine. 2026-09-05 -> 09-19: 13 sends to a dead alias all exited 0 and
    # all sat in chat.db as is_sent=0 error=22. Only the row is evidence.
    verdict, err = confirm_delivery(target, sent_at)
    if verdict == "delivered":
        return True
    if verdict == "failed":
        hint = " (22 = recipient not registered with iMessage; check the account's aliases)" \
            if err == 22 else ""
        print(f"send NOT delivered to {target}: chat.db error={err}{hint} — "
              "row left unsent, will retry next tick", file=sys.stderr)
    else:
        print(f"send unconfirmed: no from-me row for {target} in chat.db within "
              f"{CONFIRM_WAIT_SECS}s — treating as not sent, will retry next tick",
              file=sys.stderr)
    return False


# ── the tick ────────────────────────────────────────────────────────────


def score_argv(st):
    """argv for score.py. The sheet's arm provenance (which adapter generated
    the AI replies) lives in drip_state.json as arm_adapter / arm_note, set
    when the sheet is seeded; without it score.py writes a human verdict that
    names no adapter and doctor's blind_ab_gate check cannot tie it to what
    :8741 serves (the 2026-09-04 error: v5 rated, v6 served)."""
    argv = [sys.executable, SCORE_PY, SHEET, "--key", ANSWER_KEY,
            "--rater", "human", "--emit-gate", REPO_GATE]
    if st.get("arm_adapter"):
        argv += ["--arm-adapter", st["arm_adapter"]]
        if st.get("arm_note"):
            argv += ["--arm-note", st["arm_note"]]
    return argv


def run_score(st=None):
    st = st if st is not None else load_state()  # tick's test double calls run_score()
    r = subprocess.run(score_argv(st),
                       capture_output=True, text=True, timeout=60)
    print(r.stdout[-500:] if r.stdout else r.stderr[-300:])
    # score.py exit semantics: 0 = PASS verdict, 1 = ran but verdict != PASS
    # (still a successful scoring run), >=2 = usage error/crash.
    return r.returncode in (0, 1)


def better_score_argv(st):
    """argv for better_score.py. NEVER touches the LoRA promotion gate file —
    writes only to ~/.human/blind_ab_better.json (better_score.py's own
    default)."""
    argv = [sys.executable, BETTER_SCORE_PY, SHEET, "--better-key", BETTER_KEY]
    if st.get("arm_adapter"):
        argv += ["--arm-adapter", st["arm_adapter"]]
        if st.get("arm_note"):
            argv += ["--arm-note", st["arm_note"]]
    return argv


def run_better_score(st=None):
    st = st if st is not None else load_state()
    r = subprocess.run(better_score_argv(st),
                       capture_output=True, text=True, timeout=60)
    print(r.stdout[-500:] if r.stdout else r.stderr[-300:])
    # better_score.py exit semantics: 0 = measurement written; 3 = nothing
    # to score (all skipped or ties) -- final, no rate exists, so the pass is
    # complete; any other non-zero = a real failure to retry.
    if r.returncode == 3:
        print("better_score.py: no non-tie better answers -- no rate; pass complete")
        return True
    return r.returncode == 0


def _set_pending(st, ids, now):
    st["pending_rows"] = list(ids)
    # A batch of one keeps the legacy field so rating_ingest.py's single-row
    # harvest (guarded on `pending_row`) behaves exactly as before; a larger
    # batch clears it so that harvest leaves the batch to this module.
    st["pending_row"] = ids[0] if len(ids) == 1 else None
    st["question_unix"] = now


def _clear_pending(st):
    st["pending_rows"] = []
    st["pending_row"] = None
    st["question_unix"] = 0
    st["asks"] = 1


def better_pending_ids(st):
    """Row ids awaiting a "better" answer. No legacy single-row migration —
    this is a new state shape, with nothing predating it on disk."""
    return list(st.get("better_pending_rows") or [])


def _set_better_pending(st, ids, now):
    st["better_pending_rows"] = list(ids)
    st["better_question_unix"] = now


def _clear_better_pending(st):
    st["better_pending_rows"] = []
    st["better_question_unix"] = 0
    st["better_asks"] = 1


def _next_batch(rows, skipped, size, field="choice"):
    """Up to `size` unanswered, non-skipped rows, in sheet order — the same
    order next_unanswered() walks, so a batch of one is identical to before.
    `field` defaults to "choice" so every existing caller is unchanged."""
    skipped = skipped or []
    out = [r for r in rows
           if r.get("id") not in skipped and not (r.get(field) or "").strip()]
    return out[:size]


def _compose(batch, answered, total):
    # A batch of one keeps the original message byte-for-byte.
    if len(batch) == 1:
        return compose_question(batch[0], answered, total)
    return compose_batch_question(batch, answered, total)


def _label(ids):
    return f"row {ids[0]}" if len(ids) == 1 else f"rows {ids[0]}..{ids[-1]} ({len(ids)})"


def tick(dry_run=False, now=None):
    now = now if now is not None else time.time()
    st = load_state()
    rows, _ = load_sheet()
    total = len(rows)
    better_on = bool(st.get("better_enabled"))

    # 1) ingest a pending DETECTION answer — one row, or a whole batch — if any
    ids = pending_ids(st)
    if ids and st.get("question_unix"):
        ans = harvest_answer(st["target"], st["question_unix"], n=len(ids))
        if ans:
            answers = [ans] if isinstance(ans, tuple) else list(ans)
            # parse_batch_answer already refuses a count mismatch; this guard is
            # the second wall, because positional mapping onto the wrong row
            # would corrupt the only human-rated data the program has.
            if len(answers) == len(ids):
                for rid, (choice, conf) in zip(ids, answers):
                    if write_choice(SHEET, rid, choice, conf):
                        print(f"ingested: row {rid} = {choice} (conf {conf})")
                        st["answered"] += 1
                _clear_pending(st)
                rows, _ = load_sheet()  # reload with the new answers

    # 1b) ingest a pending "better" answer — a SEPARATE pass over the same
    #     rows, asked only after the detection pass is answered for them.
    #     Activates only when better_on (newly-seeded sheets default this on;
    #     the current in-progress sheet stays off unless the owner/lead runs
    #     `rating_drip.py enable-better`).
    bids = better_pending_ids(st)
    if better_on and bids and st.get("better_question_unix"):
        ans = harvest_answer(st["target"], st["better_question_unix"], n=len(bids),
                             allowed=BETTER_ANSWER_LETTERS)
        if ans:
            answers = [ans] if isinstance(ans, tuple) else list(ans)
            if len(answers) == len(bids):
                for rid, (choice, _conf) in zip(bids, answers):
                    if write_better_choice(SHEET, rid, choice):
                        print(f"ingested (better): row {rid} = {choice}")
                        st["better_answered"] = st.get("better_answered", 0) + 1
                _clear_better_pending(st)
                rows, _ = load_sheet()

    detect_done = next_unanswered(rows, st.get("skipped")) is None
    better_done = better_on and (
        next_unanswered(rows, st.get("better_skipped"), field="better_choice") is None)

    # 2) detection complete? score.py -> ~/.human/blind_ab_gate.json, the
    #    LoRA promotion-gate verdict, the tick the detection pass completes --
    #    the pre-better-pass cadence. It never waits on the better pass: that
    #    measurement is not promotion-gating, so it must not gate the timing
    #    of the one that is.
    if detect_done and not st.get("complete"):
        print(f"sheet complete ({total}/{total}) — running score.py -> gate verdict")
        if run_score():
            st["complete"] = True
        else:
            # complete stays False so the next tick retries; a silently-
            # unscored complete sheet blocks the human tier.
            print("score.py FAILED — sheet is fully rated but the gate "
                  "verdict was NOT emitted; will retry next tick",
                  file=sys.stderr)

    # 2b) better pass complete (only when better_on)? better_score.py ->
    #     ~/.human/blind_ab_better.json, a SEPARATE file — never the gate.
    if detect_done and better_done and not st.get("better_complete"):
        print("running better_score.py -> better-than-human measurement")
        if run_better_score():
            st["better_complete"] = True
        else:
            print("better_score.py FAILED — the better-than-human measurement "
                  "was NOT written; will retry next tick", file=sys.stderr)

    if detect_done and (not better_on or better_done):
        save_state(st)
        return

    in_hours = within_send_hours(time.localtime(now).tm_hour)

    # 3) detection pass not yet done for every row: drive it exactly as
    #    before (unchanged behavior when better_on is False).
    if not detect_done:
        ids = pending_ids(st)
        if ids:
            asks = st.get("asks", 1)
            if should_reask(now, st.get("question_unix", 0), asks) and in_hours:
                by_id = {r["id"]: r for r in rows}
                batch = [by_id[i] for i in ids if i in by_id]
                if len(batch) != len(ids):
                    # The sheet changed under a pending batch, so a reply could
                    # no longer map positionally onto the right rows. Reset
                    # rather than risk writing a rating onto the wrong pair.
                    print(f"{_label(ids)} no longer all on the sheet — resetting")
                    _clear_pending(st)
                else:
                    answered = sum(1 for r in rows if (r.get("choice") or "").strip())
                    if send_question(st["target"], _compose(batch, answered, total),
                                     dry_run=dry_run):
                        st["asks"] = asks + 1
                        st["question_unix"] = now
                        print(f"re-asked {_label(ids)} (ask {asks + 1}/{MAX_ASKS_PER_ROW})")
            elif st.get("question_unix", 0) > 0 and \
                    (now - st["question_unix"]) >= REASK_AFTER_SECS and \
                    st.get("asks", 1) >= MAX_ASKS_PER_ROW:
                st.setdefault("skipped", []).extend(ids)
                print(f"{_label(ids)} unanswered after {MAX_ASKS_PER_ROW} asks — skipping")
                _clear_pending(st)
            else:
                print(f"waiting on answer for {_label(ids)} — not re-asking yet")
        elif not in_hours:
            print("outside send hours (09-21 local) — skipping")
        else:
            batch = _next_batch(rows, st.get("skipped"), BATCH_SIZE)
            answered = sum(1 for r in rows if (r.get("choice") or "").strip())
            if batch and send_question(st["target"], _compose(batch, answered, total),
                                       dry_run=dry_run):
                new_ids = [r["id"] for r in batch]
                _set_pending(st, new_ids, now)
                st["asks"] = 1
                st["sent"] += 1
                print(f"sent question for {_label(new_ids)} ({answered + 1}/{total})")
        save_state(st)
        return

    # 4) detection pass is done for every row and better_on: drive the
    #    "better" pass the same way, over its own pending/asks/skipped state,
    #    so the two measurements never corrupt each other's in-flight batch.
    bids = better_pending_ids(st)
    if bids:
        basks = st.get("better_asks", 1)
        if should_reask(now, st.get("better_question_unix", 0), basks) and in_hours:
            by_id = {r["id"]: r for r in rows}
            batch = [by_id[i] for i in bids if i in by_id]
            if len(batch) != len(bids):
                print(f"{_label(bids)} (better) no longer all on the sheet — resetting")
                _clear_better_pending(st)
            else:
                detection_key = load_detection_key()
                better_key_store = load_better_key()
                answered = sum(1 for r in rows if (r.get("better_choice") or "").strip())
                q = compose_better_question(batch, detection_key, better_key_store,
                                            answered, total)
                if send_question(st["target"], q, dry_run=dry_run):
                    st["better_asks"] = basks + 1
                    st["better_question_unix"] = now
                    print(f"re-asked {_label(bids)} (better, ask "
                          f"{basks + 1}/{MAX_ASKS_PER_ROW})")
        elif st.get("better_question_unix", 0) > 0 and \
                (now - st["better_question_unix"]) >= REASK_AFTER_SECS and \
                st.get("better_asks", 1) >= MAX_ASKS_PER_ROW:
            st.setdefault("better_skipped", []).extend(bids)
            print(f"{_label(bids)} (better) unanswered after {MAX_ASKS_PER_ROW} "
                  "asks — skipping")
            _clear_better_pending(st)
        else:
            print(f"waiting on answer for {_label(bids)} (better) — not re-asking yet")
    elif not in_hours:
        print("outside send hours (09-21 local) — skipping (better)")
    else:
        batch = _next_batch(rows, st.get("better_skipped"), BATCH_SIZE,
                            field="better_choice")
        if batch:
            detection_key = load_detection_key()
            better_key_store = load_better_key()
            decide_better_key([r["id"] for r in batch], detection_key, better_key_store)
            save_better_key(better_key_store)
            answered = sum(1 for r in rows if (r.get("better_choice") or "").strip())
            q = compose_better_question(batch, detection_key, better_key_store,
                                        answered, total)
            if send_question(st["target"], q, dry_run=dry_run):
                new_ids = [r["id"] for r in batch]
                _set_better_pending(st, new_ids, now)
                st["better_asks"] = 1
                st["better_sent"] = st.get("better_sent", 0) + 1
                print(f"sent question for {_label(new_ids)} (better) "
                      f"({answered + 1}/{total})")
    save_state(st)


def status():
    st = load_state()
    rows, _ = load_sheet()
    answered = sum(1 for r in rows if (r.get("choice") or "").strip())
    print(f"rated {answered}/{len(rows)} | pending: {pending_ids(st) or None} | "
          f"sent: {st.get('sent', 0)} | complete: {st.get('complete', False)}")
    if st.get("better_enabled"):
        better_answered = sum(1 for r in rows if (r.get("better_choice") or "").strip())
        print(f"better  {better_answered}/{len(rows)} | "
              f"pending: {better_pending_ids(st) or None} | "
              f"sent: {st.get('better_sent', 0)}")
    else:
        print("better: disabled for this sheet "
              "(run `rating_drip.py enable-better` to turn it on)")


def enable_better():
    """Turn on the "better" pass for the CURRENT sheet. Merges into the
    existing drip_state.json (does not touch anything else) -- a
    newly-seeded sheet already defaults this on via load_state()'s fallback,
    so this subcommand exists for the in-progress sheet the lead must not
    otherwise disturb."""
    st = load_state()
    if st.get("better_enabled"):
        print("better already enabled for this sheet")
        return
    st["better_enabled"] = True
    save_state(st)
    print("better_enabled=true written to drip_state.json — the next tick "
          "will start the \"better\" pass once the detection pass is fully "
          "rated")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "tick"
    dry = "--dry-run" in sys.argv
    if cmd == "tick":
        tick(dry_run=dry)
    elif cmd == "status":
        status()
    elif cmd == "enable-better":
        enable_better()
    else:
        print(__doc__)
