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
