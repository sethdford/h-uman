"""Human-only evidence (spec §4): daemon/ambiguous sends are context, never citable."""
import datetime as dt
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
import curator_evidence as ce  # noqa: E402

T0 = dt.datetime(2026, 9, 27, 12, 0, tzinfo=dt.timezone.utc)


def m(rowid, guid, from_me, text, minutes=0):
    return {"rowid": rowid, "guid": guid, "from_me": from_me, "text": text,
            "t": T0 + dt.timedelta(minutes=minutes), "atype": 0}


def test_authorship_maps_to_who_and_ambiguous_is_daemon():
    msgs = [m(1, "a", False, "hi"), m(2, "b", True, "hey", 1), m(3, "c", True, "sup", 2),
            m(4, "d", True, "yo", 3)]
    labels = {"b": "seth", "c": "huuman", "d": "ambiguous"}
    rows = ce.chat_turn_rows(msgs, labels, 80, T0 - dt.timedelta(days=1))
    assert [r[2] for r in rows] == ["them", "me", "daemon", "daemon"]


def test_attachment_only_turn_is_a_placeholder_and_last_n_kept():
    msgs = [m(i, f"g{i}", False, "" if i == 5 else f"t{i}", i) for i in range(10)]
    rows = ce.chat_turn_rows(msgs, {}, 4, T0 - dt.timedelta(days=1))
    assert [r[0] for r in rows] == [6, 7, 8, 9]
    rows = ce.chat_turn_rows(msgs, {}, 80, T0 - dt.timedelta(days=1))
    assert rows[5][3] == "[attachment]"


def test_number_rows_daemon_rows_are_d_and_not_in_cite_map():
    rows = [(1, 0, "them", "hi"), (2, 0, "daemon", "hey"), (3, 0, "me", "ok")]
    lines, cite = ce.number_rows(rows)
    assert lines == ["[t0] them: hi", "[d0] me: hey", "[t1] me: ok"]
    assert set(cite) == {0, 1} and cite[1][0] == 3


def test_parse_evidence_flags_daemon_citations():
    assert ce.parse_evidence(["t0", "1", "[t2]"]) == ([0, 1, 2], False)
    assert ce.parse_evidence(["t0", "d1"]) == ([0], True)
    assert ce.parse_evidence([]) == ([], False)


def test_name_said_word_bounded_and_possessive():
    assert ce.name_said("Priya", ["priya's surgery is tuesday"])
    assert not ce.name_said("Al", ["Also, see you then"])
    assert ce.name_said("St Petersburg", ["moving to st petersburg soon"])
    assert not ce.name_said("Priya", ["[attachment]"])


def _cite():
    return {0: (11, 1, "them", "priya's surgery is tuesday"), 1: (12, 2, "me", "hope it goes well")}


def test_validate_note_accepts_supported_named_note():
    note = {"note": "Priya's surgery tuesday", "kind": "plan", "confidence": 0.9,
            "evidence_tokens": ["t0"], "names": [{"name": "Priya", "type": "person"}]}
    out, why = ce.validate_note(note, _cite())
    assert why == "ok" and out["evidence_rows"] == [_cite()[0]]


def test_validate_note_rejects_daemon_empty_and_invented_names():
    base = {"note": "x", "kind": "fact", "confidence": 0.9, "names": []}
    assert ce.validate_note({**base, "evidence_tokens": ["d0"]}, _cite())[1] == "daemon_evidence"
    assert ce.validate_note({**base, "evidence_tokens": ["t9"]}, _cite())[1] == "no_evidence"
    inv = {**base, "evidence_tokens": ["t0"], "names": [{"name": "Marcus", "type": "person"}]}
    assert ce.validate_note(inv, _cite()) == (None, "name_not_said")
