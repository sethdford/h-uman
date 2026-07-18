#!/usr/bin/env python3
"""Tests for mine_phrase_banks.py — Seth's-own-voice phrase-bank miner.

Builds a synthetic chat.db fixture (same shape as the real Messages schema
subset the miner queries) and checks extraction, the min-frequency floor,
the PII scrub (digits + proper-noun heuristic), and the on-disk JSON shape.
"""

import json
import sqlite3
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import mine_phrase_banks as mpb

APPLE_EPOCH_NS = 10**9  # helper base; we use small second offsets * 1e9


def _make_db(rows):
    """Create a fixture chat.db with the columns the miner reads.

    rows: iterable of (handle_id, is_from_me, date_seconds, text).
    Returns path to the sqlite file.
    """
    tmp = tempfile.NamedTemporaryFile(suffix=".db", delete=False)
    tmp.close()
    con = sqlite3.connect(tmp.name)
    con.execute(
        "CREATE TABLE message ("
        " ROWID INTEGER PRIMARY KEY,"
        " text TEXT,"
        " attributedBody BLOB,"
        " handle_id INTEGER,"
        " is_from_me INTEGER,"
        " date INTEGER)"
    )
    for handle_id, is_from_me, sec, text in rows:
        con.execute(
            "INSERT INTO message (text, attributedBody, handle_id, is_from_me, date)"
            " VALUES (?, NULL, ?, ?, ?)",
            (text, handle_id, is_from_me, int(sec * 1_000_000_000)),
        )
    con.commit()
    con.close()
    return Path(tmp.name)


def _sent(handle, sec, text):
    return (handle, 1, sec, text)


def _recv(handle, sec, text):
    return (handle, 0, sec, text)


def _bank_texts(bank, key):
    return [e["text"] for e in bank.get(key, [])]


# ── message-initial fillers/starters ──────────────────────────────────────


def test_fillers_message_initial_with_floor():
    rows = []
    # "haha" starts 6 sent messages (>= floor of 5)
    for i in range(6):
        rows.append(_sent(1, 100 + i * 60, f"haha that meeting was rough {i}"))
    # ...and appears standalone twice (filler evidence: works on its own)
    rows.append(_sent(1, 900, "haha"))
    rows.append(_sent(1, 930, "haha"))
    # "ngl" starts only 2 (below floor)
    for i in range(2):
        rows.append(_sent(1, 1000 + i * 60, "ngl that was fine"))
    db = _make_db(rows)
    banks = mpb.mine(db, min_freq=5, gap_hours=6)
    fillers = _bank_texts(banks["imessage"], "fillers")
    assert "haha " in fillers, f"expected 'haha ' in {fillers}"
    assert "ngl " not in fillers, "below-floor starter must be dropped"
    # freq is recorded
    entry = [e for e in banks["imessage"]["fillers"] if e["text"] == "haha "][0]
    assert entry["freq"] == 6
    # starters mirror the message-initial distribution
    assert "haha " in _bank_texts(banks["imessage"], "starters")


def test_fillers_require_standalone_evidence_starters_do_not():
    """"you"/"just"/"and" are frequent first WORDS but not fillers — injecting
    them as a prefix produces garbage ("you ok that works"). A filler must
    occur as a standalone one-word message at least twice (a single "you?"
    reply is not filler evidence); a starter need not."""
    rows = [_sent(1, 100 + i * 60, "you good for tonight?") for i in range(6)]
    rows.append(_sent(1, 900, "you?"))  # one standalone occurrence: not enough
    db = _make_db(rows)
    banks = mpb.mine(db, min_freq=5, gap_hours=6)
    assert "you " not in _bank_texts(banks["imessage"], "fillers")
    assert "you " in _bank_texts(banks["imessage"], "starters")


def test_incoming_messages_do_not_contribute():
    rows = [_recv(1, 100 + i * 60, "haha whatever dude") for i in range(10)]
    db = _make_db(rows)
    banks = mpb.mine(db, min_freq=5, gap_hours=6)
    assert _bank_texts(banks["imessage"], "fillers") == []


def test_tapback_echoes_ignored():
    rows = [_sent(1, 100 + i * 60, "Liked “ok sounds good”") for i in range(6)]
    db = _make_db(rows)
    banks = mpb.mine(db, min_freq=5, gap_hours=6)
    assert _bank_texts(banks["imessage"], "fillers") == []


# ── backchannel one-worders ───────────────────────────────────────────────


def test_backchannels_full_message_one_worders():
    rows = []
    for i in range(5):
        rows.append(_sent(1, 100 + i * 60, "yeah"))
    for i in range(5):
        rows.append(_sent(1, 700 + i * 60, "fr!"))  # trailing punct ok
    rows.append(_sent(1, 2000, "yeah that works"))  # multi-word: not a backchannel
    db = _make_db(rows)
    banks = mpb.mine(db, min_freq=5, gap_hours=6)
    bc = _bank_texts(banks["imessage"], "backchannels")
    assert "yeah" in bc
    assert "fr" in bc, f"punct-stripped one-worder expected in {bc}"


# ── farewells: last own message before a >gap_hours silence ───────────────


def test_farewell_before_long_gap():
    rows = []
    hour = 3600
    for i in range(5):
        base = i * 100 * hour
        rows.append(_recv(1, base + 0, "ok talk tomorrow"))
        rows.append(_sent(1, base + 60, "gn"))
        # next activity in the chat comes 7h later (> 6h gap)
        rows.append(_recv(1, base + 60 + 7 * hour, "morning"))
    # control: own message followed 10 minutes later is NOT a farewell,
    # even if it repeats often
    for i in range(6):
        rows.append(_sent(2, 1000 + i * hour, "brb"))
        rows.append(_recv(2, 1000 + i * hour + 600, "ok"))
    db = _make_db(rows)
    banks = mpb.mine(db, min_freq=5, gap_hours=6)
    fw = _bank_texts(banks["imessage"], "farewells")
    assert "gn" in fw, f"expected 'gn' in {fw}"
    assert "brb" not in fw, "message followed within the gap window is not a farewell"


def test_farewell_last_message_of_chat_counts():
    rows = []
    for h in range(5):  # 5 separate chats, each ending with the same sign-off
        rows.append(_recv(10 + h, 100, "later man"))
        rows.append(_sent(10 + h, 200, "peace out"))
    db = _make_db(rows)
    banks = mpb.mine(db, min_freq=5, gap_hours=6)
    assert "peace out" in _bank_texts(banks["imessage"], "farewells")


# ── PII scrub ─────────────────────────────────────────────────────────────


def test_digits_are_scrubbed():
    rows = [_sent(1, 100 + i * 60, "911") for i in range(6)]
    rows += [_sent(1, 1000 + i * 60, "4pm works for me") for i in range(6)]
    db = _make_db(rows)
    banks = mpb.mine(db, min_freq=5, gap_hours=6)
    assert _bank_texts(banks["imessage"], "backchannels") == []
    assert "4pm " not in _bank_texts(banks["imessage"], "fillers")


def test_proper_nouns_are_scrubbed():
    rows = []
    # "jordan" starts 6 sent messages (autocapitalized like real texting)
    for i in range(6):
        rows.append(_sent(1, 100 + i * 60, "Jordan said we should go"))
    # mid-sentence occurrences are capitalized -> proper-noun signal
    for i in range(3):
        rows.append(_sent(1, 1000 + i * 60, f"i was with Jordan earlier {i}"))
    # "haha" also autocapitalized message-initially, but lowercase mid-sentence
    for i in range(6):
        rows.append(_sent(1, 2000 + i * 60, "Haha no way man"))
    rows.append(_sent(1, 2500, "haha"))  # standalone evidence for the filler
    rows.append(_sent(1, 2530, "haha"))
    for i in range(3):
        rows.append(_sent(1, 3000 + i * 60, f"ok haha sure thing {i}"))
    db = _make_db(rows)
    banks = mpb.mine(db, min_freq=5, gap_hours=6)
    fillers = _bank_texts(banks["imessage"], "fillers")
    assert "jordan " not in fillers, f"proper noun leaked into {fillers}"
    assert "haha " in fillers


# ── end-to-end: JSON file shape ───────────────────────────────────────────


def test_written_json_shape():
    rows = [_sent(1, 100 + i * 60, "haha good stuff") for i in range(6)]
    db = _make_db(rows)
    out = Path(tempfile.mkdtemp()) / "phrase_banks.json"
    rc = mpb.main(["--db", str(db), "--out", str(out), "--min-freq", "5"])
    assert rc == 0
    data = json.loads(out.read_text())
    assert "imessage" in data
    ch = data["imessage"]
    for key in ("fillers", "starters", "backchannels", "farewells"):
        assert key in ch, f"missing key {key}"
        for entry in ch[key]:
            assert set(entry) == {"text", "freq"}
            assert isinstance(entry["text"], str)
            assert isinstance(entry["freq"], int)


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    print("=" * 60)
    print("mine_phrase_banks tests")
    print("=" * 60)
    p = f = 0
    for t in tests:
        try:
            t()
            print(f"✓ {t.__name__}")
            p += 1
        except AssertionError as e:
            print(f"✗ {t.__name__}: {e}")
            f += 1
        except Exception as e:  # noqa: BLE001
            print(f"✗ {t.__name__}: {type(e).__name__}: {e}")
            f += 1
    print("=" * 60)
    print(f"Results: {p} passed, {f} failed")
    return 0 if f == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
