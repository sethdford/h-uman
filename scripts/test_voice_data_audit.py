#!/usr/bin/env python3
"""Tests for voice_data_audit.py (W7-1)."""

import sqlite3
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import voice_data_audit as vda


def test_audit_counts_and_pairs():
    rows = [
        ("s1", "user", "you around?"),
        ("s1", "assistant", "yeah for a bit whats up"),
        ("s1", "user", "wanna grab food"),
        ("s1", "assistant", "sure gimme 20"),
        ("s2", "assistant", "no preceding user — not a pair"),
    ]
    st = vda.audit_rows(rows)
    assert st["total_messages"] == 5
    assert st["user_messages"] == 2
    assert st["assistant_messages"] == 3
    assert st["sessions"] == 2
    assert st["candidate_pairs"] == 2, st  # two user→assistant adjacencies in s1


def test_pairs_do_not_cross_sessions():
    rows = [
        ("s1", "user", "hey"),
        ("s2", "assistant", "should NOT pair with s1's user"),
    ]
    st = vda.audit_rows(rows)
    assert st["candidate_pairs"] == 0, st


def test_type_token_ratio_repetitive_is_low():
    rows = [("s1", "assistant", "ok ok ok ok ok ok ok ok ok ok") for _ in range(3)]
    st = vda.audit_rows(rows)
    assert st["type_token_ratio"] < vda.LOW_DIVERSITY_TTR, st


def test_verdict_starved():
    st = vda.audit_rows([("s1", "user", "hi"), ("s1", "assistant", "hey there friend")])
    verdict, reasons = vda.data_viability_verdict(st)
    assert verdict == "starved", (verdict, st)
    assert any("assistant messages" in r for r in reasons)


def test_verdict_viable_needs_volume_and_sessions():
    rows = []
    # 320 assistant msgs across 8 sessions, diverse-ish tokens
    for i in range(320):
        s = f"s{i % 8}"
        rows.append((s, "user", f"question number {i} about plans and life"))
        rows.append((s, "assistant", f"honestly i think option {i} sounds better tbh, lets do that"))
    st = vda.audit_rows(rows)
    verdict, reasons = vda.data_viability_verdict(st)
    assert verdict == "viable", (verdict, st, reasons)


def test_verdict_thin_when_few_sessions_even_with_volume():
    rows = []
    for i in range(320):
        rows.append(("only-session", "user", f"q {i}"))
        rows.append(("only-session", "assistant", f"reply variant {i} with some words here"))
    st = vda.audit_rows(rows)
    verdict, reasons = vda.data_viability_verdict(st)
    assert verdict == "thin", (verdict, st)
    assert any("distinct sessions" in r for r in reasons)


def test_read_messages_roundtrip(tmp_path=None):
    d = Path(tempfile.mkdtemp())
    db = d / "memory.db"
    conn = sqlite3.connect(db)
    conn.execute("CREATE TABLE messages (id INTEGER PRIMARY KEY, session_id TEXT, role TEXT, content TEXT)")
    conn.executemany(
        "INSERT INTO messages (session_id, role, content) VALUES (?,?,?)",
        [("s1", "user", "hi"), ("s1", "assistant", "hey whats up")],
    )
    conn.commit()
    conn.close()
    rows = vda.read_messages(str(db))
    assert rows is not None and len(rows) == 2
    st = vda.audit_rows(rows)
    assert st["candidate_pairs"] == 1


def test_read_messages_missing_table_is_graceful():
    d = Path(tempfile.mkdtemp())
    db = d / "empty.db"
    conn = sqlite3.connect(db)
    conn.execute("CREATE TABLE other (x INTEGER)")
    conn.commit()
    conn.close()
    assert vda.read_messages(str(db)) is None


def main():
    tests = [
        test_audit_counts_and_pairs,
        test_pairs_do_not_cross_sessions,
        test_type_token_ratio_repetitive_is_low,
        test_verdict_starved,
        test_verdict_viable_needs_volume_and_sessions,
        test_verdict_thin_when_few_sessions_even_with_volume,
        test_read_messages_roundtrip,
        test_read_messages_missing_table_is_graceful,
    ]
    print("Testing voice_data_audit.py")
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
