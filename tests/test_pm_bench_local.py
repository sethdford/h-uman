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
