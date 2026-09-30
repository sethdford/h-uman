"""Hermetic tests for scripts/prospective_shadow_report.py (spec 2026-09-30 §3,
controller rulings F5/F6/F15).

A synthetic service log and a synthetic memory.db in tmp_path. Nothing reads
~/.human, no model, no network, no ports.
"""
import json
import re
import sqlite3
import stat
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

import pytest  # noqa: E402

import prospective_shadow_report as psr  # noqa: E402

PROSPECTIVE_POLICY_C = (
    Path(__file__).resolve().parent.parent / "src" / "memory" / "prospective_policy.c")


def gate_banner_literals():
    """Read the six hu_prospective_gate_banner() return-string literals
    straight out of the C source (src/memory/prospective_policy.c), rather
    than hardcoding a copy here, so a future banner edit is covered by this
    test too. Each `return "..." "...";` may span several adjacent C string
    literals (compiler-concatenated); this joins them the same way the
    compiler would."""
    src = PROSPECTIVE_POLICY_C.read_text()
    m = re.search(r"hu_prospective_gate_banner\([^\n]*\)\s*\{\n(?:.*\n)*?^\}\n", src, re.M)
    assert m, "hu_prospective_gate_banner() not found in prospective_policy.c"
    body = m.group(0)
    banners = []
    for ret in re.findall(r'return\s+((?:"(?:[^"\\]|\\.)*"\s*)+);', body):
        banners.append("".join(re.findall(r'"((?:[^"\\]|\\.)*)"', ret)))
    assert len(banners) == 6, f"expected 6 gate-banner literals, got {len(banners)}: {banners}"
    return banners

C1 = "+15550000001"
SINCE = "2026-10-01"
UNTIL = "2026-10-03"
SINCE_EPOCH = int(datetime(2026, 10, 1, tzinfo=timezone.utc).timestamp())

SCHEMA_MEM = (
    "CREATE TABLE prospective_memories(id INTEGER PRIMARY KEY, trigger_type TEXT, "
    "trigger_value TEXT, action TEXT, contact_id TEXT, expires_at INTEGER, fired "
    "INTEGER DEFAULT 0, created_at INTEGER, cue_kind TEXT DEFAULT 'keyword', due_at "
    "INTEGER, status TEXT DEFAULT 'pending', surfaced_at INTEGER, attempts INTEGER "
    "DEFAULT 0, outcome TEXT, source TEXT DEFAULT 'extractor')")
SCHEMA_MSG = ("CREATE TABLE messages(id INTEGER PRIMARY KEY, session_id TEXT, role TEXT, "
              "content TEXT, created_at TEXT)")


@pytest.fixture(autouse=True)
def utc(monkeypatch):
    """Service-log stamps are LOCAL time; pin the zone so the window is exact."""
    monkeypatch.setenv("TZ", "UTC")
    time.tzset()
    yield
    monkeypatch.undo()
    time.tzset()


def new_db(tmp_path, name="memory.db"):
    p = tmp_path / name
    con = sqlite3.connect(p)
    con.execute(SCHEMA_MEM)
    con.execute(SCHEMA_MSG)
    return con, str(p)


def add_memory(con, id_, action, cue=None, contact=C1, created_at=SINCE_EPOCH - 86400,
               cue_kind="keyword", due_at=0, status="pending", trigger_type="keyword"):
    con.execute(
        "INSERT INTO prospective_memories(id,trigger_type,trigger_value,action,contact_id,"
        "expires_at,created_at,cue_kind,due_at,status) VALUES(?,?,?,?,?,0,?,?,?,?)",
        (id_, trigger_type, cue, action, contact, created_at, cue_kind, due_at, status))


def add_message(con, text, ts_text, contact=C1):
    con.execute("INSERT INTO messages(session_id,role,content,created_at) VALUES(?, 'user', ?, ?)",
                (contact, text, ts_text))


def run(tmp_path, log_text, db_path, since=SINCE, until=UNTIL):
    log = tmp_path / "service.log"
    log.write_text(log_text)
    out = tmp_path / "logs"
    rc = psr.main(["--log", str(log), "--memory-db", db_path, "--since", since,
                   "--until", until, "--out-dir", str(out)])
    return rc, out


def read_payload(out):
    [f] = list(out.iterdir())
    assert stat.S_IMODE(f.stat().st_mode) == 0o600
    return json.loads(f.read_text()), f


# ---------------------------------------------------------------------------
# Happy path + dedupe-across-days (an intention that would-fires via two
# different keyword rows on two different days must count as ONE fired
# intention, not two, and the guitar intention that never reaches the judge
# must be the one and only miss).
# ---------------------------------------------------------------------------

LOG_HAPPY = """\
2026-10-01T12:00:05 INFO  [prospective] prospective shadow: candidates=1 fire=1 resolved=0 cancel=0 not_now=0 parse_fail=0 judge_err=0 expired=0 capped=0 write_err=0
2026-10-01T12:00:05 INFO  [prospective] prospective shadow item: id=1 verdict=fire
2026-10-01T12:00:09 INFO  [prospective] prospective shadow uptake: would_fire=1 used=1
2026-10-02T12:00:05 INFO  [prospective] prospective shadow: candidates=2 fire=1 resolved=1 cancel=0 not_now=0 parse_fail=0 judge_err=0 expired=0 capped=0 write_err=0
2026-10-02T12:00:05 INFO  [prospective] prospective shadow item: id=2 verdict=fire
2026-10-02T12:00:05 INFO  [prospective] prospective shadow item: id=3 verdict=already_resolved
2026-10-02T12:00:09 INFO  [prospective] prospective shadow uptake: would_fire=1 used=0
2026-10-02T12:00:10 INFO  [human] unrelated line
"""


def make_happy_db(tmp_path):
    con, path = new_db(tmp_path)
    add_memory(con, 1, "ask about tacos", cue="taco place")
    add_memory(con, 2, "ask about tacos", cue="tacos")  # same intention, 2nd trigger row
    add_memory(con, 3, "send the recipe", cue="lasagna")
    add_memory(con, 4, "send the number", cue="guitar")  # cued, never reaches the judge
    add_message(con, "the taco place!", "2026-10-01 12:00:00")
    add_message(con, "lasagna tonight", "2026-10-02 12:00:00")
    add_message(con, "guitar time", "2026-10-02 13:00:00")
    con.commit()
    con.close()
    return path


def test_report_counts_and_rates_dedupe_across_days(tmp_path):
    rc, out = run(tmp_path, LOG_HAPPY, make_happy_db(tmp_path))
    assert rc == 0
    body, f = read_payload(out)
    c, r = body["counts"], body["rates"]

    assert c["candidates"] == 3 and c["fire"] == 2 and c["resolved"] == 1
    assert r["would_fire_per_day"] == 1.0            # 2 raw fire events over 2 days
    assert r["resolved_before_cue_rate"] == 1 / 3

    assert c["cued_intentions"] == 3                 # tacos, recipe, number all cued
    # DEDUPE ACROSS DAYS: id=1 fired day1, id=2 fired day2, but both are the
    # SAME intention ("ask about tacos") -- must collapse to 1, not 2.
    assert c["fire_intentions"] == 1
    assert r["cue_to_fire_rate"] == 1 / 3
    assert r["uptake"] == 0.5

    assert c["pending_at_start_intentions"] == 3      # tacos/recipe/number, id1+id2 collapsed
    assert c["missed_intentions"] == 1                # only "send the number" (guitar) never judged
    assert r["miss_rate"] == 1 / 3

    text = f.read_text()
    assert "ask about tacos" not in text and "send the number" not in text
    assert C1 not in text and "taco place" not in text and "guitar" not in text


# ---------------------------------------------------------------------------
# F5: duplicate_rate / time_duplicate_rate are ALWAYS null in a SHADOW
# report, even in a scenario ("ask about tacos" would-fires on two distinct
# days) that would have scored duplicate_rate == 1.0 under the naive
# (pre-ruling) computation. No number may survive into `rates`, and the
# reason must be machine-readable in `notes`.
# ---------------------------------------------------------------------------

def test_f5_duplicate_rate_is_null_not_measurable(tmp_path):
    rc, out = run(tmp_path, LOG_HAPPY, make_happy_db(tmp_path))
    assert rc == 0
    body, _ = read_payload(out)
    assert body["rates"]["duplicate_rate"] is None
    assert body["rates"]["time_duplicate_rate"] is None
    assert "dup_intentions" not in body["counts"]
    assert "time_dup_intentions" not in body["counts"]
    assert body["notes"]["duplicate_rate"] == "not_measurable_in_shadow"
    assert body["notes"]["time_duplicate_rate"] == "not_measurable_in_shadow"


# ---------------------------------------------------------------------------
# F6: miss_rate's denominator is intentions PENDING AT THE WINDOW START, not
# "cued during the window". Intention A is pending before the window and
# never cued by any inbound text and never judged -- under the superseded
# cued-based denominator it would be invisible (0 misses / 1 cued = 0.0);
# under F6 it is the one genuine miss (1 missed / 2 pending-at-start = 0.5).
# ---------------------------------------------------------------------------

LOG_F6 = """\
2026-10-01T12:00:05 INFO  [prospective] prospective shadow: candidates=1 fire=1 resolved=0 cancel=0 not_now=0 parse_fail=0 judge_err=0 expired=0 capped=0 write_err=0
2026-10-01T12:00:05 INFO  [prospective] prospective shadow item: id=2 verdict=fire
"""


def test_f6_miss_rate_denominator_is_pending_at_start(tmp_path):
    con, path = new_db(tmp_path)
    add_memory(con, 1, "A", cue="wordone")   # pending at start, never cued, never judged
    add_memory(con, 2, "B", cue="wordtwo")   # pending at start, cued, judged (fire)
    add_message(con, "wordtwo appears here", "2026-10-01 11:00:00")
    con.commit()
    con.close()

    rc, out = run(tmp_path, LOG_F6, path)
    assert rc == 0
    body, _ = read_payload(out)
    c, r = body["counts"], body["rates"]

    assert c["cued_intentions"] == 1               # only B was cued
    assert c["pending_at_start_intentions"] == 2    # A and B were both pending at window start
    assert c["missed_intentions"] == 1              # A: pending, never reached the judge
    assert r["miss_rate"] == 0.5                    # 1/2, NOT 0/1 (the cued-only denominator)
    assert body["notes"]["miss_rate_denominator"].startswith("pending_at_window_start")


# ---------------------------------------------------------------------------
# write_err must never inflate judge_failure_rate; it is reported as its own
# count instead.
# ---------------------------------------------------------------------------

LOG_WRITE_ERR = """\
2026-10-01T12:00:05 INFO  [prospective] prospective shadow: candidates=2 fire=0 resolved=0 cancel=0 not_now=0 parse_fail=0 judge_err=1 expired=0 capped=0 write_err=5
"""


def test_write_err_excluded_from_judge_failure_rate(tmp_path):
    con, path = new_db(tmp_path)
    con.commit()
    con.close()

    rc, out = run(tmp_path, LOG_WRITE_ERR, path)
    assert rc == 0
    body, _ = read_payload(out)
    c, r = body["counts"], body["rates"]

    assert c["judge_err"] == 1
    assert c["write_err"] == 5
    # 1/2, not (1+5)/2 -- write_err must not leak into the judge-failure numerator.
    assert r["judge_failure_rate"] == 0.5
    assert body["notes"]["judge_failure_rate_excludes"] == "write_err (reported separately as a count)"


# ---------------------------------------------------------------------------
# A malformed shadow-shaped line refuses the ENTIRE report -- exit 2, no
# file, even though a well-formed line exists earlier in the same window.
# ---------------------------------------------------------------------------

LOG_MALFORMED = """\
2026-10-01T12:00:05 INFO  [prospective] prospective shadow: candidates=1 fire=1 resolved=0 cancel=0 not_now=0 parse_fail=0 judge_err=0 expired=0 capped=0 write_err=0
2026-10-01T12:00:06 INFO  [prospective] prospective shadow item: id=1
"""


def test_malformed_line_refuses_and_writes_nothing(tmp_path):
    con, path = new_db(tmp_path)
    con.commit()
    con.close()

    rc, out = run(tmp_path, LOG_MALFORMED, path)
    assert rc == 2
    assert not out.exists()


# ---------------------------------------------------------------------------
# Fix round I1: a daemon-restart gate banner (hu_prospective_gate_banner,
# src/memory/prospective_policy.c) must NOT be treated as a shadow-shaped
# candidate line. All four of the SHADOW/OFF banners name "shadow" somewhere
# in their text -- before the fix, PREFIX's loose ".*shadow.*" matched them,
# they then failed every COUNTS/ITEM/UPTAKE shape check, and the "shadow-
# shaped but not one of the three known lines" branch counted each one
# MALFORMED -- refusing the WHOLE report (exit 2) on any window that
# happened to contain a daemon restart. Read the six literal banner strings
# straight out of the C source (gate_banner_literals(), above) so a future
# banner edit is covered by this test too, not just today's wording.
# ---------------------------------------------------------------------------

def test_gate_banners_are_not_shadow_candidates_and_do_not_refuse(tmp_path):
    for i, banner in enumerate(gate_banner_literals()):
        case_dir = tmp_path / f"case{i}"
        case_dir.mkdir()
        log = LOG_HAPPY + f"2026-10-02T12:00:11 INFO  [prospective] {banner}\n"
        rc, out = run(case_dir, log, make_happy_db(case_dir))
        assert rc == 0, f"banner line refused the report: {banner!r}"
        body, _ = read_payload(out)
        # Counts/rates unchanged from the happy path (test_report_counts_and_
        # rates_dedupe_across_days) -- the banner line must be inert, not
        # silently folded into any count.
        c = body["counts"]
        assert c["candidates"] == 3 and c["fire"] == 2 and c["resolved"] == 1


# ---------------------------------------------------------------------------
# No shadow lines in the window at all -- exit 2, nothing written.
# ---------------------------------------------------------------------------

def test_empty_window_refuses_and_writes_nothing(tmp_path):
    con, path = new_db(tmp_path)
    con.commit()
    con.close()

    rc, out = run(tmp_path, "2026-10-01T12:00:00 INFO  [human] nothing here\n", path)
    assert rc == 2
    assert not out.exists()


# ---------------------------------------------------------------------------
# An unmigrated database (missing the v2 columns) refuses too.
# ---------------------------------------------------------------------------

def test_unmigrated_db_refuses(tmp_path):
    p = tmp_path / "memory.db"
    con = sqlite3.connect(p)
    con.execute("CREATE TABLE prospective_memories(id INTEGER PRIMARY KEY, trigger_type TEXT, "
                "trigger_value TEXT, action TEXT, contact_id TEXT, expires_at INTEGER, fired "
                "INTEGER DEFAULT 0, created_at INTEGER)")
    con.execute(SCHEMA_MSG)
    con.commit()
    con.close()

    rc, out = run(tmp_path, LOG_HAPPY, str(p))
    assert rc == 2
    assert not out.exists()


# ---------------------------------------------------------------------------
# rates_of() is a pure function: every rate with a zero denominator is null,
# never a ZeroDivisionError or a fabricated 0/0.
# ---------------------------------------------------------------------------

def test_rates_with_empty_denominators_are_null():
    counts = {"days": 2, "fire": 0, "candidates": 0, "resolved": 0, "fire_intentions": 0,
              "cued_intentions": 0, "uptake_used": 0, "uptake_would_fire": 0,
              "missed_intentions": 0, "pending_at_start_intentions": 0, "judge_err": 0,
              "time_fire": 0, "time_resolved": 0, "time_candidates": 0,
              "time_missed_intentions": 0, "time_due_intentions": 0, "time_judge_err": 0}
    r = psr.rates_of(counts)
    assert r["would_fire_per_day"] == 0.0            # nonzero denominator (days), zero numerator
    assert r["resolved_before_cue_rate"] is None
    assert r["cue_to_fire_rate"] is None
    assert r["uptake"] is None
    assert r["miss_rate"] is None
    assert r["duplicate_rate"] is None
    assert r["time_duplicate_rate"] is None
    assert r["judge_failure_rate"] is None
    assert r["time_resolved_rate"] is None
    assert r["time_miss_rate"] is None
    assert r["time_judge_failure_rate"] is None


# ---------------------------------------------------------------------------
# Fix round 1, (1): time_miss_rate's denominator is time-cue intentions
# PENDING AT THE WINDOW START (created before --since, still pending) whose
# due_at < --until -- due within the window OR already overdue before it
# opened. id=10 is overdue two days before the window and never produces a
# time item line: under the superseded due-in-window-only denominator it
# would be invisible (0 misses / 1 due = 0.0); under the fix it is the one
# genuine miss (1 missed / 2 pending-at-start-and-due-before-until = 0.5).
# ---------------------------------------------------------------------------

LOG_TIME_OVERDUE = """\
2026-10-01T12:00:05 INFO  [prospective] prospective time shadow: candidates=1 fire=1 resolved=0 cancel=0 not_now=0 parse_fail=0 judge_err=0 expired=0 capped=0 write_err=0
2026-10-01T12:00:05 INFO  [prospective] prospective time shadow item: id=11 verdict=fire
"""


def test_time_miss_rate_counts_overdue_before_window(tmp_path):
    con, path = new_db(tmp_path)
    # Overdue before the window even opened, still pending, never judged.
    add_memory(con, 10, "call the vet", cue_kind="time",
               created_at=SINCE_EPOCH - 200000, due_at=SINCE_EPOCH - 172800)
    # Due inside the window, judged (fire).
    add_memory(con, 11, "confirm reservation", cue_kind="time",
               created_at=SINCE_EPOCH - 86400, due_at=SINCE_EPOCH + 3600)
    con.commit()
    con.close()

    rc, out = run(tmp_path, LOG_TIME_OVERDUE, path)
    assert rc == 0
    body, _ = read_payload(out)
    c, r = body["counts"], body["rates"]

    assert c["time_due_intentions"] == 2             # both pending-at-start AND due<until
    assert c["time_missed_intentions"] == 1          # only "call the vet" (id=10) never judged
    assert r["time_miss_rate"] == 0.5                # 1/2, NOT 0/1 (due-in-window-only denominator)


# ---------------------------------------------------------------------------
# Fix round 1, (2): read_log's malformed-line gate validates VALUES too, not
# just field presence. "candidates=1x2" must refuse (exit 2, nothing
# written), never reach an uncaught ValueError in build()'s int(kv[f]).
# ---------------------------------------------------------------------------

LOG_BAD_VALUE = """\
2026-10-01T12:00:05 INFO  [prospective] prospective shadow: candidates=1x2 fire=1 resolved=0 cancel=0 not_now=0 parse_fail=0 judge_err=0 expired=0 capped=0 write_err=0
"""


def test_malformed_count_value_refuses_and_writes_nothing(tmp_path):
    con, path = new_db(tmp_path)
    con.commit()
    con.close()

    rc, out = run(tmp_path, LOG_BAD_VALUE, path)
    assert rc == 2
    assert not out.exists()
