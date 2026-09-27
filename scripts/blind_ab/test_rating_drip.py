"""Tests for rating_drip.py pure helpers — the measurement-as-conversation drip.

The parser is the safety-critical piece: in a self-chat BOTH directions are
"from me", so the strict-start A/B regex is the only thing separating Seth's
answer from the drip's own question text (which contains "A)" mid-string) and
from ordinary notes-to-self. Pin it hard.
"""
import csv
import os
import tempfile

import rating_drip as rd


# ── parse_answer: the injection/noise boundary ──────────────────────────

def test_parse_plain_a():
    assert rd.parse_answer("A") == ("A", 3)

def test_parse_lowercase_b():
    assert rd.parse_answer("b") == ("B", 3)

def test_parse_with_confidence():
    assert rd.parse_answer("A 4") == ("A", 4)
    assert rd.parse_answer("a5") == ("A", 5)
    assert rd.parse_answer("B, 2") == ("B", 2)

def test_parse_with_paren():
    assert rd.parse_answer("B)") == ("B", 3)

def test_parse_rejects_prose():
    assert rd.parse_answer("maybe A?") is None
    assert rd.parse_answer("I think B sounds better") is None

def test_parse_rejects_question_text():
    # The drip's own question contains "A)" mid-string — must NOT self-ingest.
    q = "[h-uman rating 1/12] which sounds more like you?\nthem: hi\nA) yo\nB) hello"
    assert rd.parse_answer(q) is None

def test_parse_rejects_long_text():
    assert rd.parse_answer("A" + " " * 20 + "4") is None

def test_parse_rejects_empty_and_none():
    assert rd.parse_answer("") is None
    assert rd.parse_answer(None) is None

def test_parse_rejects_other_letters():
    assert rd.parse_answer("C") is None
    assert rd.parse_answer("AB") is None


# ── lenient forms (chip design default: "option a", "first one" → A) ─────

def test_parse_lenient_option_forms():
    assert rd.parse_answer("option a") == ("A", 3)
    assert rd.parse_answer("Option B") == ("B", 3)
    assert rd.parse_answer("option a 4") == ("A", 4)

def test_parse_lenient_ordinal_forms():
    assert rd.parse_answer("first") == ("A", 3)
    assert rd.parse_answer("first one") == ("A", 3)
    assert rd.parse_answer("the first one") == ("A", 3)
    assert rd.parse_answer("1st") == ("A", 3)
    assert rd.parse_answer("second") == ("B", 3)
    assert rd.parse_answer("the second one") == ("B", 3)
    assert rd.parse_answer("2nd") == ("B", 3)

def test_parse_lenient_ordinal_with_confidence():
    assert rd.parse_answer("the first one 4") == ("A", 4)
    assert rd.parse_answer("second one, 5") == ("B", 5)

def test_parse_lenient_still_rejects_prose():
    # Whole-message anchoring: prose containing an ordinal must NOT parse.
    assert rd.parse_answer("the first one is better tbh") is None
    assert rd.parse_answer("first thing tomorrow") is None
    assert rd.parse_answer("option c") is None
    # "a second" reads as "wait a second" — ambiguous, reject (re-ask instead).
    assert rd.parse_answer("a second") is None
    # Bare digits are ambiguous (confidence-only? stray?) — reject.
    assert rd.parse_answer("1") is None
    assert rd.parse_answer("2") is None


# ── sheet helpers ───────────────────────────────────────────────────────

FIELDS = ["id", "context", "option_A", "option_B", "choice", "confidence"]

def _mk_sheet(rows):
    f = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False, newline="")
    w = csv.DictWriter(f, fieldnames=FIELDS)
    w.writeheader()
    w.writerows(rows)
    f.close()
    return f.name

def test_next_unanswered_picks_first_blank():
    rows = [
        {"id": "r1", "context": "c", "option_A": "a", "option_B": "b", "choice": "A", "confidence": "3"},
        {"id": "r2", "context": "c", "option_A": "a", "option_B": "b", "choice": "", "confidence": ""},
        {"id": "r3", "context": "c", "option_A": "a", "option_B": "b", "choice": "", "confidence": ""},
    ]
    assert rd.next_unanswered(rows)["id"] == "r2"

def test_next_unanswered_none_when_complete():
    rows = [{"id": "r1", "context": "c", "option_A": "a", "option_B": "b", "choice": "B", "confidence": "4"}]
    assert rd.next_unanswered(rows) is None

def test_write_choice_persists_and_is_atomic():
    path = _mk_sheet([
        {"id": "r1", "context": "c", "option_A": "a", "option_B": "b", "choice": "", "confidence": ""},
        {"id": "r2", "context": "c", "option_A": "a", "option_B": "b", "choice": "", "confidence": ""},
    ])
    try:
        assert rd.write_choice(path, "r2", "B", 5) is True
        with open(path, newline="") as f:
            rows = list(csv.DictReader(f))
        assert rows[1]["choice"] == "B" and rows[1]["confidence"] == "5"
        assert rows[0]["choice"] == ""  # untouched
    finally:
        os.unlink(path)

def test_write_choice_unknown_row_returns_false():
    path = _mk_sheet([{"id": "r1", "context": "c", "option_A": "a", "option_B": "b", "choice": "", "confidence": ""}])
    try:
        assert rd.write_choice(path, "nope", "A", 3) is False
    finally:
        os.unlink(path)


# ── question composition + guards ───────────────────────────────────────

def test_compose_question_contains_both_options_and_progress():
    row = {"id": "r7", "context": "you coming?", "option_A": "yeah omw", "option_B": "Indeed, I shall attend."}
    q = rd.compose_question(row, answered=3, total=12)
    assert "4/12" in q and "yeah omw" in q and "Indeed, I shall attend." in q
    assert q.splitlines()[0].startswith("[h-uman rating")

def test_send_hours_window():
    assert rd.within_send_hours(9) is True
    assert rd.within_send_hours(20) is True
    assert rd.within_send_hours(21) is False
    assert rd.within_send_hours(8) is False
    assert rd.within_send_hours(2) is False

def test_apple_ts_conversion():
    # 2001-01-01T00:00:00Z + 1e9 ns = 1 second after the Apple epoch.
    assert abs(rd.apple_ts_to_unix(1e9) - (rd.APPLE_EPOCH + 1)) < 1e-6


# ── v1.1: first-reply semantics + re-ask/skip ───────────────────────────

def _apple(unix):
    return (unix - rd.APPLE_EPOCH) * 1e9

def test_first_answer_picks_earliest_after_question():
    # newest-first rows: a later stray "A" must NOT beat the actual first reply "B".
    rows = [("A", None, _apple(2000)), ("B", None, _apple(1500)), ("old", None, _apple(500))]
    assert rd.first_answer_after(rows, since_unix=1000) == ("B", 3)

def test_first_answer_ignores_everything_before_question():
    rows = [("B", None, _apple(900))]
    assert rd.first_answer_after(rows, since_unix=1000) is None

def test_first_answer_uses_decoder_for_null_text():
    rows = [(None, b"blob", _apple(1500))]
    assert rd.first_answer_after(rows, 1000, decoder=lambda b: "A 5") == ("A", 5)

def test_should_reask_truth_table():
    day = 24 * 3600
    assert rd.should_reask(now_unix=day + 10, question_unix=1, asks=1) is True
    assert rd.should_reask(now_unix=day + 10, question_unix=1, asks=rd.MAX_ASKS_PER_ROW) is False
    assert rd.should_reask(now_unix=100, question_unix=1, asks=1) is False   # too soon
    assert rd.should_reask(now_unix=day + 10, question_unix=0, asks=1) is False  # no pending

def test_next_unanswered_respects_skipped():
    rows = [
        {"id": "r1", "choice": ""},
        {"id": "r2", "choice": ""},
    ]
    assert rd.next_unanswered(rows, skipped=["r1"])["id"] == "r2"
    assert rd.next_unanswered(rows, skipped=["r1", "r2"]) is None


# ── run_score invocation + completion gating (pinned 2026-07-25) ────────
# Bug: the answer key was passed positionally (score.py exit 2 "need
# sheets + --key") and tick() marked complete anyway — a fully-rated
# sheet silently never produced a gate verdict.

class _FakeResult:
    def __init__(self, rc):
        self.returncode = rc
        self.stdout = ""
        self.stderr = ""


def _patched_run(rc, calls):
    def fake(argv, **kwargs):
        calls.append(list(argv))
        return _FakeResult(rc)
    return fake


def test_run_score_passes_key_and_emit_gate():
    import subprocess as sp
    calls, orig = [], sp.run
    sp.run = _patched_run(0, calls)
    try:
        assert rd.run_score() is True
    finally:
        sp.run = orig
    argv = calls[0]
    assert "--key" in argv
    assert argv[argv.index("--key") + 1] == rd.ANSWER_KEY
    assert "--emit-gate" in argv


def test_run_score_fail_verdict_exit1_is_success():
    import subprocess as sp
    orig = sp.run
    sp.run = _patched_run(1, [])  # score.py exit 1 = ran, verdict != PASS
    try:
        assert rd.run_score() is True
    finally:
        sp.run = orig


def test_run_score_usage_error_exit2_is_failure():
    import subprocess as sp
    orig = sp.run
    sp.run = _patched_run(2, [])  # argparse error — nothing scored
    try:
        assert rd.run_score() is False
    finally:
        sp.run = orig


def test_tick_completion_requires_score_success():
    import json as _json
    import tempfile as _tf
    tmpdir = _tf.mkdtemp()
    sheet = os.path.join(tmpdir, "rating_sheet.csv")
    with open(sheet, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["id", "context", "option_A", "option_B",
                                          "choice", "confidence"])
        w.writeheader()
        w.writerow({"id": "r1", "context": "c", "option_A": "a", "option_B": "b",
                    "choice": "A", "confidence": "4"})
    state = os.path.join(tmpdir, "drip_state.json")
    with open(state, "w") as f:
        _json.dump({"target": "t@me.com", "pending_row": None, "question_unix": 0,
                    "sent": 1, "answered": 1, "complete": False, "asks": 1}, f)
    orig = (rd.SHEET, rd.STATE, rd.SHEET_DIR, rd.run_score)
    rd.SHEET, rd.STATE, rd.SHEET_DIR = sheet, state, tmpdir
    try:
        rd.run_score = lambda: False
        rd.tick()
        with open(state) as f:
            assert not _json.load(f).get("complete")
        rd.run_score = lambda: True
        rd.tick()
        with open(state) as f:
            assert _json.load(f).get("complete") is True
    finally:
        rd.SHEET, rd.STATE, rd.SHEET_DIR, rd.run_score = orig
        import shutil as _sh
        _sh.rmtree(tmpdir, ignore_errors=True)


def test_score_argv_records_arm_adapter_from_state():
    st = {"arm_adapter": "seth-glm-air-v6-orpo-real-20260802-190128",
          "arm_note": "49 adapter-bound trials 2026-09-04"}
    argv = rd.score_argv(st)
    assert argv[argv.index("--arm-adapter") + 1] == "seth-glm-air-v6-orpo-real-20260802-190128"
    assert argv[argv.index("--arm-note") + 1] == "49 adapter-bound trials 2026-09-04"
    assert "--rater" in argv and argv[argv.index("--rater") + 1] == "human"


def test_score_argv_without_arm_passes_no_arm_flags():
    argv = rd.score_argv({})
    assert "--arm-adapter" not in argv and "--arm-note" not in argv


# ── delivery confirmation ───────────────────────────────────────────────
# Bug (2026-09-05 → 09-19): the target `sethford@me.com` stopped being an
# alias on the iMessage account. `imsg send` exited 0 on every tick, the log
# said "sent question", and chat.db recorded each row as is_sent=0 error=22.
# 13 questions, 0 delivered, 4 rows skipped as "unanswered", 0/48 rated.
# The exit code is not the artifact; the chat.db row is.

def test_delivery_verdict_truth_table():
    since = 1000.0
    after, before = _apple(1001), _apple(999)
    assert rd.delivery_verdict([(1, 0, after)], since) == ("delivered", 0)
    assert rd.delivery_verdict([(0, 22, after)], since) == ("failed", 22)
    assert rd.delivery_verdict([], since) == ("pending", None)
    # a failed row from BEFORE this send must not be blamed on it
    assert rd.delivery_verdict([(0, 22, before)], since) == ("pending", None)
    # not yet marked sent and no error: still in flight
    assert rd.delivery_verdict([(0, 0, after)], since) == ("pending", None)
    # newest-first: the newest row decides
    assert rd.delivery_verdict([(1, 0, _apple(1002)), (0, 22, after)], since) == ("delivered", 0)


def test_send_question_requires_chat_db_confirmation():
    import subprocess as sp
    orig_run, orig_confirm = sp.run, rd.confirm_delivery
    sp.run = _patched_run(0, [])  # imsg always "succeeds"
    try:
        rd.confirm_delivery = lambda *a, **k: ("failed", 22)
        assert rd.send_question("+15555550100", "q") is False
        rd.confirm_delivery = lambda *a, **k: ("pending", None)
        assert rd.send_question("+15555550100", "q") is False
        rd.confirm_delivery = lambda *a, **k: ("delivered", 0)
        assert rd.send_question("+15555550100", "q") is True
    finally:
        sp.run, rd.confirm_delivery = orig_run, orig_confirm


def _delivery_fixture_db(path, target, rows):
    import sqlite3 as _sq
    con = _sq.connect(path)
    cur = con.cursor()
    cur.execute("CREATE TABLE chat (ROWID INTEGER PRIMARY KEY, chat_identifier TEXT)")
    cur.execute("CREATE TABLE message (ROWID INTEGER PRIMARY KEY, date INTEGER, "
                "is_from_me INTEGER, is_sent INTEGER, error INTEGER)")
    cur.execute("CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER)")
    cur.execute("INSERT INTO chat (chat_identifier) VALUES (?)", (target,))
    cid = cur.lastrowid
    for is_sent, error, apple_ns in rows:
        cur.execute("INSERT INTO message (date, is_from_me, is_sent, error) VALUES (?, 1, ?, ?)",
                    (int(apple_ns), is_sent, error))
        cur.execute("INSERT INTO chat_message_join VALUES (?, ?)", (cid, cur.lastrowid))
    con.commit()
    con.close()


def test_confirm_delivery_reads_the_chat_db_row():
    import os as _os
    import tempfile as _tf
    tmp = _tf.mkdtemp()
    db = _os.path.join(tmp, "chat.db")
    since = 1000.0
    # the 09-05 shape: imsg exit 0, chat.db says error 22
    _delivery_fixture_db(db, "sethford@me.com", [(0, 22, _apple(1001))])
    assert rd.confirm_delivery("sethford@me.com", since, db_path=db, wait_secs=0) == ("failed", 22)
    # the 09-20 shape after retargeting: a real send
    db2 = _os.path.join(tmp, "chat2.db")
    _delivery_fixture_db(db2, "+18012017497", [(1, 0, _apple(1001))])
    assert rd.confirm_delivery("+18012017497", since, db_path=db2, wait_secs=0) == ("delivered", 0)
    # another chat's row is not this send
    assert rd.confirm_delivery("+15555550100", since, db_path=db2, wait_secs=0) == ("pending", None)


def test_default_target_is_a_live_alias_not_the_dead_one():
    import rating_ingest as ri
    assert rd.DEFAULT_TARGET == "+18012017497"
    assert ri.DEFAULT_TARGET == rd.DEFAULT_TARGET
    assert "sethford@me.com" not in (rd.DEFAULT_TARGET, ri.DEFAULT_TARGET)


# ── batch mode: several rows per message ────────────────────────────────
# Same safety contract as the single-row parser: whole-message anchored, exact
# shape. A batch reply is EXACTLY n A/B letters (separators allowed). A note-to-
# self that happens to be exactly n A/B letters and nothing else is implausible,
# which is what keeps the self-chat boundary intact while raising throughput.

def test_batch_parse_exact_letters():
    assert rd.parse_batch_answer("ABBAB", 5) == ["A", "B", "B", "A", "B"]

def test_batch_parse_tolerates_case_spaces_commas():
    want = ["A", "B", "B", "A", "B"]
    assert rd.parse_batch_answer("a b b a b", 5) == want
    assert rd.parse_batch_answer("A, B, B, A, B", 5) == want
    assert rd.parse_batch_answer("  abbab  ", 5) == want

def test_batch_parse_rejects_wrong_count():
    # Positional mapping: a short or long reply would assign answers to the
    # wrong rows, so ANY count mismatch must be refused rather than guessed at.
    assert rd.parse_batch_answer("ABBA", 5) is None
    assert rd.parse_batch_answer("ABBABA", 5) is None

def test_batch_parse_rejects_other_letters_and_prose():
    assert rd.parse_batch_answer("ABCAB", 5) is None
    assert rd.parse_batch_answer("maybe ABBAB", 5) is None
    assert rd.parse_batch_answer("ABBAB?", 5) is None
    assert rd.parse_batch_answer("", 5) is None
    assert rd.parse_batch_answer(None, 5) is None

def test_batch_parse_rejects_ordinary_notes_to_self():
    # The self-chat is also Seth's notes app. These must never read as ratings.
    for note in ("a good idea for tomorrow", "bab", "abba!", "Baba ganoush",
                 "call back about the a/b test"):
        assert rd.parse_batch_answer(note, 5) is None, note

def test_batch_parse_rejects_its_own_question_text():
    # The drip reads its OWN sent message back from the self-chat. That message
    # contains every row's A)/B) and an example reply — it must not parse.
    rows = [{"id": f"r{i}", "context": "hi", "option_A": "yo", "option_B": "hey"}
            for i in range(5)]
    q = rd.compose_batch_question(rows, answered=4, total=48)
    assert rd.parse_batch_answer(q, 5) is None
    assert rd.parse_answer(q) is None  # nor as a single-row answer

def test_batch_question_is_numbered_and_states_the_reply_shape():
    rows = [{"id": f"r{i}", "context": f"ctx{i}", "option_A": f"a{i}",
             "option_B": f"b{i}"} for i in range(3)]
    q = rd.compose_batch_question(rows, answered=4, total=48)
    assert "5-7/48" in q                  # which rows of the sheet these are
    assert "3 letters" in q               # the exact reply shape, stated
    for i in range(3):
        assert f"{i + 1})" in q
        assert f"ctx{i}" in q and f"a{i}" in q and f"b{i}" in q

def test_pending_ids_migrates_the_legacy_single_row():
    # The live state file carries a single `pending_row` at the moment this
    # ships. It must survive as a batch of one, not be stranded.
    assert rd.pending_ids({"pending_row": "orpo1-33"}) == ["orpo1-33"]
    assert rd.pending_ids({"pending_rows": ["a", "b"]}) == ["a", "b"]
    assert rd.pending_ids({"pending_rows": [], "pending_row": "x"}) == ["x"]
    assert rd.pending_ids({}) == []
    assert rd.pending_ids({"pending_row": None, "pending_rows": None}) == []


# ── tick() in batch mode, driven end-to-end through the state file ──────

def _batch_fixture(n_rows, state_extra):
    import json as _json
    import tempfile as _tf
    tmpdir = _tf.mkdtemp()
    sheet = os.path.join(tmpdir, "rating_sheet.csv")
    with open(sheet, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["id", "context", "option_A", "option_B",
                                          "choice", "confidence"])
        w.writeheader()
        for i in range(n_rows):
            w.writerow({"id": f"r{i + 1}", "context": f"c{i + 1}",
                        "option_A": f"a{i + 1}", "option_B": f"b{i + 1}",
                        "choice": "", "confidence": ""})
    state = os.path.join(tmpdir, "drip_state.json")
    st = {"target": "+15555550100", "pending_row": None, "question_unix": 0,
          "sent": 0, "answered": 0, "complete": False, "asks": 1}
    st.update(state_extra)
    with open(state, "w") as f:
        _json.dump(st, f)
    return tmpdir, sheet, state


def _noon():
    import time as _t
    lt = list(_t.localtime())
    lt[3], lt[4], lt[5] = 12, 0, 0   # 12:00 local — inside send hours
    return _t.mktime(tuple(lt))


def _with_drip(tmpdir, sheet, state, **patches):
    orig = {k: getattr(rd, k) for k in ("SHEET", "STATE", "SHEET_DIR", *patches)}
    rd.SHEET, rd.STATE, rd.SHEET_DIR = sheet, state, tmpdir
    for k, v in patches.items():
        setattr(rd, k, v)
    return orig


def _restore(orig):
    for k, v in orig.items():
        setattr(rd, k, v)


def test_tick_sends_one_message_for_a_whole_batch():
    import json as _json
    tmpdir, sheet, state = _batch_fixture(7, {})
    sent = []
    orig = _with_drip(tmpdir, sheet, state, BATCH_SIZE=5,
                      send_question=lambda t, q, dry_run=False: sent.append(q) or True,
                      harvest_answer=lambda *a, **k: None)
    try:
        rd.tick(now=_noon())
        st = _json.load(open(state))
        assert len(sent) == 1, "one message for the whole batch, not five"
        assert st["pending_rows"] == ["r1", "r2", "r3", "r4", "r5"]
        # n>1 clears the legacy field so rating_ingest.py's single-row harvest
        # (guarded on pending_row) leaves the batch to the drip.
        assert st["pending_row"] is None
        assert "reply with 5 letters" in sent[0]
    finally:
        _restore(orig)


def test_tick_ingests_a_batch_reply_onto_the_right_rows():
    import json as _json
    tmpdir, sheet, state = _batch_fixture(
        5, {"pending_rows": ["r1", "r2", "r3", "r4", "r5"], "question_unix": 1000})
    reply = [("A", 3), ("B", 3), ("B", 3), ("A", 3), ("B", 3)]
    orig = _with_drip(tmpdir, sheet, state, BATCH_SIZE=5,
                      send_question=lambda *a, **k: True,
                      harvest_answer=lambda *a, **k: reply,
                      run_score=lambda: True)
    try:
        rd.tick(now=_noon())
        got = {r["id"]: r["choice"] for r in csv.DictReader(open(sheet))}
        # Positional: the k-th letter lands on the k-th pending row.
        assert got == {"r1": "A", "r2": "B", "r3": "B", "r4": "A", "r5": "B"}
        st = _json.load(open(state))
        assert st["answered"] == 5 and st["pending_rows"] == []
    finally:
        _restore(orig)


def test_tick_harvests_with_the_pending_count():
    # The harvester must be told HOW MANY answers to expect; that count is
    # what makes a 4-letter or 6-letter reply refusable.
    tmpdir, sheet, state = _batch_fixture(
        3, {"pending_rows": ["r1", "r2", "r3"], "question_unix": 1000})
    seen = {}
    def fake_harvest(target, since, n=1, **k):
        seen["n"] = n
        return None
    orig = _with_drip(tmpdir, sheet, state, BATCH_SIZE=5,
                      send_question=lambda *a, **k: True, harvest_answer=fake_harvest)
    try:
        rd.tick(now=_noon())
        assert seen["n"] == 3
    finally:
        _restore(orig)


def test_tick_still_ingests_the_legacy_single_pending_row():
    # The live state file carried `pending_row: orpo1-33` when this shipped.
    tmpdir, sheet, state = _batch_fixture(3, {"pending_row": "r2", "question_unix": 1000})
    orig = _with_drip(tmpdir, sheet, state, BATCH_SIZE=5,
                      send_question=lambda *a, **k: True,
                      harvest_answer=lambda *a, **k: [("B", 4)])
    try:
        rd.tick(now=_noon())
        got = {r["id"]: (r["choice"], r["confidence"]) for r in csv.DictReader(open(sheet))}
        assert got["r2"] == ("B", "4")
        assert got["r1"] == ("", "") and got["r3"] == ("", "")
    finally:
        _restore(orig)
