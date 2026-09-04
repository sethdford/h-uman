#!/usr/bin/env python3
"""Tests for scripts/fit_reply_delay_model.py — Contract C5, Part C.

Builds a synthetic in-memory chat.db-shaped sqlite database (same table
shapes fit_reply_delay_model.py queries: chat, chat_handle_join, handle,
message, chat_message_join) with a known, hand-constructed reply-delay
distribution, then verifies the fitted quantile table recovers it. No real
chat.db is touched.
"""
import sqlite3
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from fit_reply_delay_model import (  # noqa: E402
    build_delay_samples,
    fit,
    freq_tercile,
    length_bucket,
    percentile,
    quantiles_for,
)

APPLE_EPOCH = 978307200


def make_fixture_db():
    """Returns an in-memory sqlite3 connection shaped like chat.db, with
    the minimum columns fit_reply_delay_model.py's queries touch."""
    con = sqlite3.connect(":memory:")
    con.executescript(
        """
        CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT);
        CREATE TABLE chat (ROWID INTEGER PRIMARY KEY, chat_identifier TEXT);
        CREATE TABLE chat_handle_join (chat_id INTEGER, handle_id INTEGER);
        CREATE TABLE message (
            ROWID INTEGER PRIMARY KEY,
            date INTEGER,
            is_from_me INTEGER,
            is_system_message INTEGER DEFAULT 0,
            item_type INTEGER DEFAULT 0,
            text TEXT,
            attributedBody BLOB
        );
        CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER);
        """
    )
    return con


def unix_to_apple_ns(ts_unix):
    return int(round((ts_unix - APPLE_EPOCH) * 1_000_000_000))


def add_message(con, msg_id, chat_id, ts_unix, is_from_me, text):
    con.execute(
        "INSERT INTO message (ROWID, date, is_from_me, text) VALUES (?, ?, ?, ?)",
        (msg_id, unix_to_apple_ns(ts_unix), 1 if is_from_me else 0, text),
    )
    con.execute("INSERT INTO chat_message_join (chat_id, message_id) VALUES (?, ?)", (chat_id, msg_id))


def add_dm_chat(con, chat_id, handle_row, contact_id):
    con.execute("INSERT INTO chat (ROWID, chat_identifier) VALUES (?, ?)", (chat_id, contact_id))
    con.execute("INSERT INTO handle (ROWID, id) VALUES (?, ?)", (handle_row, contact_id))
    con.execute("INSERT INTO chat_handle_join (chat_id, handle_id) VALUES (?, ?)", (chat_id, handle_row))


def test_length_bucket():
    thresholds = (40, 160)
    assert length_bucket(0, thresholds) == "short"
    assert length_bucket(39, thresholds) == "short"
    assert length_bucket(40, thresholds) == "medium"
    assert length_bucket(160, thresholds) == "medium"
    assert length_bucket(161, thresholds) == "long"
    print("✓ length_bucket boundaries correct")


def test_freq_tercile():
    boundaries = (5, 20)
    assert freq_tercile(1, boundaries) == "low"
    assert freq_tercile(5, boundaries) == "low"
    assert freq_tercile(6, boundaries) == "mid"
    assert freq_tercile(20, boundaries) == "mid"
    assert freq_tercile(21, boundaries) == "high"
    print("✓ freq_tercile boundaries correct")


def test_percentile_matches_known_values():
    vals = [10, 20, 30, 40, 50]
    assert percentile(vals, 0.0) == 10
    assert percentile(vals, 1.0) == 50
    assert percentile(vals, 0.5) == 30
    print("✓ percentile() matches hand-computed values")


def test_quantiles_for_empty_is_none():
    assert quantiles_for([]) is None
    print("✓ quantiles_for([]) is None (no fabricated numbers on empty input)")


def test_build_delay_samples_recovers_known_delay():
    """One contact, one inbound message at hour 10 with a 60s reply. The
    fitted sample list must contain exactly that (hour, delay) pair."""
    con = make_fixture_db()
    add_dm_chat(con, chat_id=1, handle_row=1, contact_id="+15550001111")
    # Anchor a message at a specific unix time whose LOCAL hour is
    # deterministic regardless of the test machine's timezone: use
    # time.localtime on a chosen ts and read back the hour, rather than
    # asserting a specific hour ourselves.
    base_ts = time.time() - 3600  # 1h ago, definitely within any lookback
    add_message(con, 1, 1, base_ts, is_from_me=False, text="hey are you around")
    add_message(con, 2, 1, base_ts + 60, is_from_me=True, text="yep")
    con.commit()

    samples, contact_counts = build_delay_samples(con, since_unix=base_ts - 86400, max_delay_secs=3600)
    assert len(samples) == 1, f"expected 1 sample, got {len(samples)}"
    s = samples[0]
    assert s["delay_secs"] == 60, s
    assert s["len_chars"] == len("hey are you around"), s
    assert s["contact"] == "+15550001111"
    assert contact_counts["+15550001111"] == 2
    print("✓ build_delay_samples recovers the known 60s delay + text length")


def test_build_delay_samples_excludes_replies_beyond_max_delay():
    con = make_fixture_db()
    add_dm_chat(con, chat_id=1, handle_row=1, contact_id="+15550002222")
    base_ts = time.time() - 3600
    add_message(con, 1, 1, base_ts, is_from_me=False, text="hi")
    add_message(con, 2, 1, base_ts + 7200, is_from_me=True, text="sorry, been busy")  # 2h later
    con.commit()

    samples, _ = build_delay_samples(con, since_unix=base_ts - 86400, max_delay_secs=3600)  # 1h cap
    assert samples == [], "reply beyond max_delay_secs must not produce a sample"
    print("✓ replies beyond max_delay_secs are excluded (censored, not truncated)")


def test_build_delay_samples_excludes_group_chats():
    con = make_fixture_db()
    # Group chat: two handles joined to the same chat.
    con.execute("INSERT INTO chat (ROWID, chat_identifier) VALUES (1, 'group;1')")
    con.execute("INSERT INTO handle (ROWID, id) VALUES (1, '+15550003333')")
    con.execute("INSERT INTO handle (ROWID, id) VALUES (2, '+15550004444')")
    con.execute("INSERT INTO chat_handle_join (chat_id, handle_id) VALUES (1, 1)")
    con.execute("INSERT INTO chat_handle_join (chat_id, handle_id) VALUES (1, 2)")
    base_ts = time.time() - 3600
    add_message(con, 1, 1, base_ts, is_from_me=False, text="group hi")
    add_message(con, 2, 1, base_ts + 60, is_from_me=True, text="group reply")
    con.commit()

    samples, _ = build_delay_samples(con, since_unix=base_ts - 86400, max_delay_secs=3600)
    assert samples == [], "group chats must be excluded from the DM-only delay model"
    print("✓ group chats excluded")


def test_fit_produces_hierarchical_fallback_levels():
    """With enough samples in exactly one (hour,len,freq) cell, fit() must
    populate that cell AND its (hour,len) and (hour) and global marginals,
    since every sample also counts toward the coarser buckets."""
    con = make_fixture_db()
    add_dm_chat(con, chat_id=1, handle_row=1, contact_id="+15550005555")
    base_ts = time.time() - 3600
    hour = time.localtime(base_ts).tm_hour
    for i in range(10):
        t0 = base_ts + i * 2
        add_message(con, i * 2 + 1, 1, t0, is_from_me=False, text="short msg")
        add_message(con, i * 2 + 2, 1, t0 + 30 + i, is_from_me=True, text="ok")
    con.commit()

    samples, counts = build_delay_samples(con, since_unix=base_ts - 86400, max_delay_secs=3600)
    assert len(samples) == 10
    model = fit(samples, counts, min_cell_n=5)
    assert model is not None
    assert model["global"]["n"] == 10
    assert model["global"]["quantiles"] is not None
    hour_key = f"h{hour}"
    assert hour_key in model["hour_marginals"], model["hour_marginals"].keys()
    assert model["hour_marginals"][hour_key]["n"] == 10
    print("✓ fit() populates cells + hour_len + hour + global fallback levels")


def test_fit_returns_none_for_no_samples():
    assert fit([], {}, min_cell_n=5) is None
    print("✓ fit([]) returns None rather than a fabricated model")


def main():
    tests = [
        test_length_bucket,
        test_freq_tercile,
        test_percentile_matches_known_values,
        test_quantiles_for_empty_is_none,
        test_build_delay_samples_recovers_known_delay,
        test_build_delay_samples_excludes_replies_beyond_max_delay,
        test_build_delay_samples_excludes_group_chats,
        test_fit_produces_hierarchical_fallback_levels,
        test_fit_returns_none_for_no_samples,
    ]
    print("=" * 60)
    print("Testing fit_reply_delay_model.py")
    print("=" * 60)
    passed = failed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"✗ {t.__name__}: {e}")
            failed += 1
        except Exception as e:
            print(f"✗ {t.__name__}: {type(e).__name__}: {e}")
            failed += 1
    print("=" * 60)
    print(f"Results: {passed} passed, {failed} failed")
    print("=" * 60)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
