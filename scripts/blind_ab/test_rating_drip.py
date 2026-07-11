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
