"""Hermetic tests for scripts/pm_bench_local.py (spec 2026-09-30 §3, §4.5).

The scenario set and the scoring math are tested directly. The end-to-end runs
use a fake `human` binary in tmp_path that plays the probe contract. Its
"always remind" policy is exactly the TriggerBench failure the harness exists
to catch, and a run with it must FAIL. Nothing touches ~/.human.
"""
import json
import stat
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

import pm_bench_local as pb  # noqa: E402

FAKE = r'''#!/usr/bin/env python3
import os, sqlite3, sys
a = sys.argv[2:]          # argv[1] == "prospective"
mode = os.environ.get("FAKE_MODE", "always")
def arg(k):
    return a[a.index(k) + 1] if k in a else None
db = arg("--db")
if a[0] == "init":
    con = sqlite3.connect(db)
    con.execute("CREATE TABLE IF NOT EXISTS prospective_memories(id INTEGER PRIMARY KEY "
                "AUTOINCREMENT, trigger_type TEXT, trigger_value TEXT, action TEXT, contact_id "
                "TEXT, expires_at INTEGER, fired INTEGER DEFAULT 0, created_at INTEGER, cue_kind "
                "TEXT DEFAULT 'keyword', due_at INTEGER, status TEXT DEFAULT 'pending', "
                "surfaced_at INTEGER, attempts INTEGER DEFAULT 0, outcome TEXT, source TEXT "
                "DEFAULT 'extractor')")
    con.commit()
    print("ok")
    sys.exit(0)
if mode == "crash":
    sys.exit(1)
if "--deliver" in a:
    print("surfaced=0 used=0 ignored=0 expired=0")
    sys.exit(0)
con = sqlite3.connect(db)
now = int(arg("--now"))
if "--tick" in a:
    ids = [r[0] for r in con.execute("SELECT id FROM prospective_memories WHERE cue_kind='time' "
                                     "AND status='pending' AND due_at<=?", (now,))]
else:
    text = arg("--inbound").lower()
    ids = [i for i, cue in con.execute("SELECT id, trigger_value FROM prospective_memories "
                                       "WHERE cue_kind='keyword' AND status='pending'")
           if cue.lower() in text]
ids = ids[:3]
verdict = "parse_fail" if mode == "garbled" else "fire"
fire = len(ids) if verdict == "fire" else 0
pf = len(ids) - fire
print(f"candidates={len(ids)} fire={fire} resolved=0 cancel=0 not_now=0 parse_fail={pf} "
      f"judge_err=0 expired=0 capped=0 bytes=0")
for i in ids:
    print(f"item id={i} verdict={verdict}")
'''


def fake_bin(tmp_path, monkeypatch, mode):
    b = tmp_path / "human"
    b.write_text(FAKE)
    b.chmod(0o755)
    monkeypatch.setenv("FAKE_MODE", mode)
    return str(b)


# Fix round 1: a step whose header reports write_err>0 did not complete its
# measurement (the surfaced write failed), so it must be excluded from
# scoring and counted toward the same failure budget as parse_fail/judge_err.
# This fixture always verdicts "fire" (mode irrelevant here) and additionally
# reports write_err=1 on exactly the 1-based full-header call indices named
# in FAKE_WRITE_ERR_AT, tracked via a counter file so behavior is
# deterministic across the whole 56-scenario/112-step run.
FAKE_WRITE_ERR = r'''#!/usr/bin/env python3
import os, sqlite3, sys
a = sys.argv[2:]          # argv[1] == "prospective"
def arg(k):
    return a[a.index(k) + 1] if k in a else None
db = arg("--db")
if a[0] == "init":
    con = sqlite3.connect(db)
    con.execute("CREATE TABLE IF NOT EXISTS prospective_memories(id INTEGER PRIMARY KEY "
                "AUTOINCREMENT, trigger_type TEXT, trigger_value TEXT, action TEXT, contact_id "
                "TEXT, expires_at INTEGER, fired INTEGER DEFAULT 0, created_at INTEGER, cue_kind "
                "TEXT DEFAULT 'keyword', due_at INTEGER, status TEXT DEFAULT 'pending', "
                "surfaced_at INTEGER, attempts INTEGER DEFAULT 0, outcome TEXT, source TEXT "
                "DEFAULT 'extractor')")
    con.commit()
    print("ok")
    sys.exit(0)
if "--deliver" in a:
    print("surfaced=0 used=0 ignored=0 expired=0")
    sys.exit(0)
con = sqlite3.connect(db)
now = int(arg("--now"))
if "--tick" in a:
    ids = [r[0] for r in con.execute("SELECT id FROM prospective_memories WHERE cue_kind='time' "
                                     "AND status='pending' AND due_at<=?", (now,))]
else:
    text = arg("--inbound").lower()
    ids = [i for i, cue in con.execute("SELECT id, trigger_value FROM prospective_memories "
                                       "WHERE cue_kind='keyword' AND status='pending'")
           if cue.lower() in text]
ids = ids[:3]
counter_path = os.environ["FAKE_WRITE_ERR_COUNTER"]
trigger_at = {int(x) for x in os.environ["FAKE_WRITE_ERR_AT"].split(",") if x}
try:
    with open(counter_path) as f:
        n = int(f.read().strip() or "0")
except FileNotFoundError:
    n = 0
n += 1
with open(counter_path, "w") as f:
    f.write(str(n))
write_err = 1 if n in trigger_at else 0
print(f"candidates={len(ids)} fire={len(ids)} resolved=0 cancel=0 not_now=0 parse_fail=0 "
      f"judge_err=0 expired=0 capped=0 bytes=0 write_err={write_err}")
for i in ids:
    print(f"item id={i} verdict=fire")
'''


def fake_bin_write_err(tmp_path, monkeypatch, trigger_at):
    """trigger_at: iterable of 1-based full-header call indices (in the fixed
    scenario order build_scenarios() produces) that should report write_err=1."""
    b = tmp_path / "human"
    b.write_text(FAKE_WRITE_ERR)
    b.chmod(0o755)
    monkeypatch.setenv("FAKE_WRITE_ERR_AT", ",".join(str(x) for x in trigger_at))
    monkeypatch.setenv("FAKE_WRITE_ERR_COUNTER", str(tmp_path / "write-err-counter"))
    return str(b)


def test_committed_scenarios_meet_minimums_and_never_cross_cue():
    scenarios = pb.build_scenarios()
    counts = pb.validate(scenarios)
    assert len(scenarios) == 56
    assert counts["positive_expectations"] == 40
    assert counts["silent_steps"] == 48
    assert counts["cross_day_expectations"] == 16


def test_validate_refuses_a_step_that_cues_an_unexpected_intention():
    s = pb.build_scenarios()[1]  # overloaded-taco
    bad = dict(s)
    bad["steps"] = [dict(s["steps"][0], text=s["steps"][0]["text"] + " and the interview")]
    try:
        pb.validate([bad])
    except ValueError:
        return
    raise AssertionError("validate accepted a step cueing a distractor")


def test_score_math_on_a_hand_built_run():
    results = [
        {"expect": ["a"], "pred": ["a"], "tags": []},                    # TP
        {"expect": ["b"], "pred": [], "tags": ["cross_day"]},            # FN, cross-day miss
        {"expect": [], "pred": ["c"], "tags": ["silent"]},               # FP, silent false alarm
        {"expect": [], "pred": [], "tags": ["silent"]},                  # clean silent step
        {"expect": ["d"], "pred": ["d", "e"], "tags": ["update"]},       # TP + FP, update error
    ]
    counts, rates = pb.score(results)
    assert (counts["tp"], counts["fp"], counts["fn"]) == (2, 2, 1)
    assert rates["set_f1"] == 4 / 7  # 2*tp / (2*tp + fp + fn)
    assert rates["silent_negative_false_alarm"] == 0.5
    assert rates["cross_day_miss"] == 1.0
    assert rates["update_miss"] == 1.0
    assert rates["false_alarms_per_step"] == 2 / 5
    empty_counts, empty_rates = pb.score([])
    assert empty_rates["set_f1"] is None and empty_counts["steps"] == 0


def test_always_remind_fails_on_silent_negatives(tmp_path, monkeypatch):
    out = tmp_path / "logs"
    rc = pb.main(["--human-bin", fake_bin(tmp_path, monkeypatch, "always"), "--judge", "fire",
                  "--out-dir", str(out)])
    assert rc == 1
    [report] = list(out.iterdir())
    assert stat.S_IMODE(report.stat().st_mode) == 0o600
    body = json.loads(report.read_text())
    assert body["verdict"] == "FAIL"
    assert body["rates"]["silent_negative_false_alarm"] == 1.0
    text = report.read_text()
    for s in pb.SITUATIONS:  # counts only: no action, cue or conversation text
        assert s["action"] not in text and s["cue_text"] not in text


def test_probe_failure_refuses_and_writes_nothing(tmp_path, monkeypatch):
    out = tmp_path / "logs"
    rc = pb.main(["--human-bin", fake_bin(tmp_path, monkeypatch, "crash"), "--out-dir", str(out)])
    assert rc == 2
    assert not out.exists()


def test_judge_failures_make_the_run_inconclusive(tmp_path, monkeypatch):
    out = tmp_path / "logs"
    rc = pb.main(["--human-bin", fake_bin(tmp_path, monkeypatch, "garbled"), "--out-dir",
                  str(out)])
    assert rc == 3
    assert not out.exists()


def test_missing_binary_refuses(tmp_path):
    out = tmp_path / "logs"
    assert pb.main(["--human-bin", str(tmp_path / "nope"), "--out-dir", str(out)]) == 2
    assert not out.exists()


# Fix round 1: write_err joins the judge failure budget. The committed scenario
# set makes exactly 112 full-header probe calls (every "inbound"/"tick" step
# across 56 scenarios); --judge fire reports candidates=fire on every one of
# them, so judge["candidates"] totals 112 regardless of write_err.


def test_write_err_over_budget_is_inconclusive_and_writes_nothing(tmp_path, monkeypatch):
    # 16 of 112 calls (multiples of 7) report write_err=1: 16/112 ~= 0.143 > the
    # 10% judge-failure threshold, so the run must be inconclusive, same as a
    # run whose judge mostly parse_fail'd or judge_err'd.
    out = tmp_path / "logs"
    trigger_at = range(7, 113, 7)
    rc = pb.main(["--human-bin", fake_bin_write_err(tmp_path, monkeypatch, trigger_at),
                  "--judge", "fire", "--out-dir", str(out)])
    assert rc == 3
    assert not out.exists()


def test_write_err_under_budget_excludes_steps_from_scoring_denominators(tmp_path, monkeypatch):
    # Only call #1 (clean-taco's first inbound step: tags=[], expect=["taco"]) reports
    # write_err=1: 1/112 ~= 0.0089, well under the 10% budget, so the run proceeds to
    # scoring with that one step excluded. It carries no silent/cross_day/update tag,
    # so those three denominators must be exactly unchanged; only the total step count
    # and the write_err total move.
    out = tmp_path / "logs"
    rc = pb.main(["--human-bin", fake_bin_write_err(tmp_path, monkeypatch, [1]),
                  "--judge", "fire", "--out-dir", str(out)])
    assert rc in (0, 1)
    [report] = list(out.iterdir())
    body = json.loads(report.read_text())
    counts = body["counts"]
    assert counts["write_err"] == 1
    assert counts["steps"] == 111          # 112 judged steps minus the 1 excluded
    assert counts["silent_steps"] == 48    # unaffected: excluded step isn't tagged silent
    assert counts["cross_day_expectations"] == 16   # unaffected: not tagged cross_day
    assert counts["update_steps"] == 16              # unaffected: not tagged update


# --dump-steps: the per-step expected-vs-judged diagnostic (off by default).


def test_dump_steps_is_off_by_default(tmp_path, monkeypatch):
    out = tmp_path / "logs"
    rc = pb.main(["--human-bin", fake_bin(tmp_path, monkeypatch, "always"), "--judge", "fire",
                  "--out-dir", str(out)])
    assert rc == 1
    assert [p.name for p in out.iterdir()][0].startswith("pm-bench-local-")
    assert len(list(out.iterdir())) == 1
    assert not list(tmp_path.glob("*.jsonl"))


def test_dump_steps_writes_one_private_line_per_judged_step(tmp_path, monkeypatch):
    out = tmp_path / "logs"
    dump = tmp_path / "diag" / "steps.jsonl"
    rc = pb.main(["--human-bin", fake_bin(tmp_path, monkeypatch, "always"), "--judge", "fire",
                  "--out-dir", str(out), "--dump-steps", str(dump)])
    assert rc == 1
    assert stat.S_IMODE(dump.stat().st_mode) == 0o600
    rows = [json.loads(line) for line in dump.read_text().splitlines()]
    report = json.loads(next(out.iterdir()).read_text())
    assert len(rows) == report["steps"] == 112
    classes = {r["class"] for r in rows}
    assert classes == {"clean", "overloaded", "silent_negative", "cancellation", "reschedule",
                       "cross_day_keyword", "cross_day_time"}
    # overloaded-taco: the cued intention is expected and judged fire; the four
    # distractors were never cued, so the filter never sent them to the judge.
    [ov] = [r for r in rows if r["scenario"] == "overloaded-taco"]
    by_key = {i["key"]: i for i in ov["intentions"]}
    assert by_key["taco"] == {"key": "taco", "expected": True, "verdict": "fire"}
    assert len(by_key) == 5
    assert all(i == {"key": k, "expected": False, "verdict": "not_candidate"}
               for k, i in by_key.items() if k != "taco")
    # a silent-negative step: nothing expected, the always-fire fake fired anyway.
    sil = [r for r in rows if r["class"] == "silent_negative"]
    assert len(sil) == 24 and all(r["tags"] == ["silent"] for r in sil)
    assert all(r["intentions"][0]["expected"] is False and r["intentions"][0]["verdict"] == "fire"
               for r in sil)
    # counts agree with the report: every expected+fire pair is a TP.
    tp = sum(1 for r in rows for i in r["intentions"] if i["expected"] and i["verdict"] == "fire")
    assert tp == report["counts"]["tp"]
    text = dump.read_text()  # keys and verdicts only: no action, cue or conversation text
    for s in pb.SITUATIONS:
        assert s["action"] not in text and s["cue_text"] not in text and s["reply"] not in text


def test_dump_steps_is_not_written_on_a_refusal(tmp_path, monkeypatch):
    dump = tmp_path / "steps.jsonl"
    rc = pb.main(["--human-bin", fake_bin(tmp_path, monkeypatch, "crash"), "--out-dir",
                  str(tmp_path / "logs"), "--dump-steps", str(dump)])
    assert rc == 2
    assert not dump.exists()
