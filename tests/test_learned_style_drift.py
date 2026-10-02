"""Tests for scripts/learned_style_drift.py and scripts/learned_style_nightly.sh.

Hermetic: synthetic chat.db / memory.db (eval_conversation_quality Fixture),
a temp persona dir and log dir, HOME pointed at the temp dir, and a fake
python interpreter for the wrapper. No real ~/.human, chat.db, network or
daemon ports.
"""
import datetime as dt
import json
import os
import stat
import subprocess
import sys
from pathlib import Path

import pytest

SCRIPTS = Path(__file__).parent.parent / "scripts"
sys.path.insert(0, str(SCRIPTS))
import learned_style_drift as lsd  # noqa: E402
from test_eval_conversation_quality import Fixture, MIN, T0  # noqa: E402

NOW = T0 + dt.timedelta(days=10)
NOW_ISO = NOW.strftime("%Y-%m-%dT%H:%M:%SZ")
SECRET = "okapi axolotl drift marker"


@pytest.fixture(autouse=True)
def _isolate_home(tmp_path, monkeypatch):
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    monkeypatch.delenv("HU_PERSONA_DIR", raising=False)
    monkeypatch.delenv("HU_STATE_DIR", raising=False)


# ── KS statistic ───────────────────────────────────────────────────────────

def test_ks_identical_samples_is_zero():
    assert lsd.ks_2samp([1, 2, 3, 4], [1, 2, 3, 4]) == 0.0


def test_ks_disjoint_samples_is_one():
    assert lsd.ks_2samp([1, 2, 3], [10, 11, 12]) == 1.0


def test_ks_known_value():
    # ECDFs differ by 0.5 at x in [4, 5): F_a = 1.0, F_b = 0.5.
    assert lsd.ks_2samp([1, 2, 3, 4], [3, 4, 5, 6]) == pytest.approx(0.5)
    # Ties are handled across both samples at once.
    assert lsd.ks_2samp([1, 1, 2], [1, 2, 2]) == pytest.approx(1 / 3, abs=1e-4)


def test_ks_symmetric_and_bounded_on_synthetic_data():
    a = [10 + (i * 7) % 30 for i in range(50)]
    b = [25 + (i * 11) % 40 for i in range(60)]
    d = lsd.ks_2samp(a, b)
    assert 0.0 < d < 1.0
    assert d == pytest.approx(lsd.ks_2samp(b, a))


# ── report building ─────────────────────────────────────────────────────────

def test_report_flags_only_well_sampled_drift():
    seth = {"+a": [20] * 30, "+b": [20] * 30, "+c": [20] * 5}
    hu = {"+a": [20] * 30,            # identical: KS 0
          "+b": [80] * 25,            # drifted, n >= 20 on both sides
          "+c": [90] * 40}            # drifted but Seth n=5: never flagged
    rep, flagged = lsd.build_report(seth, hu, min_n=20, threshold=0.35)
    assert flagged is True
    by = {r["ordinal"]: r for r in rep["contacts"]}
    assert [r["flagged"] for r in rep["contacts"]] == [False, True, False]
    assert by["c2"]["ks"] == 1.0 and by["c2"]["median_ratio"] == pytest.approx(4.0)
    assert by["c3"]["eligible"] is False
    assert rep["flagged_n"] == 1 and rep["eligible_n"] == 2


def test_report_not_flagged_when_close():
    seth = {"+a": list(range(10, 40))}
    hu = {"+a": list(range(11, 41))}
    rep, flagged = lsd.build_report(seth, hu, min_n=20, threshold=0.35)
    assert flagged is False
    assert rep["contacts"][0]["ks"] < 0.35


# ── h-uman lengths from outbound_sends (lengths only) ───────────────────────

def test_huuman_turn_lengths_groups_bubbles_and_skips_media(tmp_path):
    fx = Fixture(str(tmp_path))
    c = "+15550000001"
    fx.outbound(c, 0, "abc", 0)
    fx.outbound(c, 60, "defgh", 0)               # 60 s later: same turn
    fx.outbound(c, 30 * MIN, "xy", 0)            # new turn
    fx.outbound(c, 40 * MIN, None, 0, kind="media")
    fx.outbound(c, 50 * MIN, "zz", 0, raw_ms=5000)   # uptime stamp: no time, skipped
    fx.outbound("+other", 0, "q" * 9, 0)
    fx.close()
    since = T0 - dt.timedelta(days=1)
    got = lsd.huuman_turn_lengths(fx.mem_path, since)
    assert got[c] == [8, 2]
    assert got["+other"] == [9]


# ── main(): end to end on fixtures ──────────────────────────────────────────

def _env(tmp_path, hu_len):
    pdir = tmp_path / "personas"
    pdir.mkdir()
    contacts = {"+15550000001": {"name": "A"}}
    (pdir / "seth.json").write_text(json.dumps({"contacts": contacts}))
    fx = Fixture(str(tmp_path))
    c = "+15550000001"
    for i in range(25):
        t = i * 20 * MIN
        fx.msg(c, t, SECRET, False)
        fx.msg(c, t + 30, "r" * 20, True)
    for i in range(25):
        t = 200 * 3600 + i * 20 * MIN
        prior = fx.max_rowid()
        text = SECRET[:5] + "h" * (hu_len - 5)
        fx.outbound(c, t + 30, text, prior)
        fx.msg(c, t, "hey", False)
        fx.msg(c, t + 30, text, True)
    fx.close()
    logs = tmp_path / "logs"
    args = ["--persona", "seth", "--persona-dir", str(pdir), "--chat-db", fx.chat_path,
            "--memory-db", fx.mem_path, "--log-dir", str(logs), "--now", NOW_ISO]
    return args, logs


def _report(logs):
    files = list(logs.glob("learned-style-drift-*.json"))
    assert len(files) == 1
    assert stat.S_IMODE(os.stat(files[0]).st_mode) == 0o600
    return files[0]


def test_main_exit_1_on_drift_and_report_is_aggregate_only(tmp_path, capsys):
    args, logs = _env(tmp_path, hu_len=90)
    assert lsd.main(args) == 1
    path = _report(logs)
    raw = path.read_text()
    rep = json.loads(raw)
    assert rep["flagged_n"] == 1
    entry = rep["contacts"][0]
    assert entry["n_seth"] == 25 and entry["n_huuman"] == 25
    assert entry["ks"] == 1.0
    assert "+1555" not in raw and SECRET[:6] not in raw and "hhhhhh" not in raw
    out = capsys.readouterr()
    assert "+1555" not in out.out + out.err and SECRET[:6] not in out.out + out.err


def test_main_exit_0_when_lengths_match(tmp_path):
    args, logs = _env(tmp_path, hu_len=20)
    assert lsd.main(args) == 0
    rep = json.loads(_report(logs).read_text())
    assert rep["flagged_n"] == 0


def test_drift_defaults_honour_hu_state_dir(tmp_path, monkeypatch):
    monkeypatch.setenv("HU_STATE_DIR", str(tmp_path / "state"))
    a = lsd.parse_args([])
    assert a.memory_db == str(tmp_path / "state" / "memory.db")
    assert a.log_dir == str(tmp_path / "state" / "logs")
    assert a.persona_dir == str(tmp_path / "state" / "personas")


def test_main_exit_2_when_memory_db_unreadable(tmp_path):
    args, logs = _env(tmp_path, hu_len=20)
    i = args.index("--memory-db")
    args[i + 1] = str(tmp_path / "nope.db")
    assert lsd.main(args) == 2
    assert not logs.exists()


# ── nightly wrapper ─────────────────────────────────────────────────────────

def _fake_python(tmp_path):
    rec = tmp_path / "calls.txt"
    fake = tmp_path / "fakepy"
    fake.write_text(f"#!/bin/bash\necho \"$*\" >> {rec}\nexit 0\n")
    fake.chmod(0o755)
    return fake, rec


def _run_wrapper(tmp_path, **env):
    fake, rec = _fake_python(tmp_path)
    full = {"PATH": "/usr/bin:/bin", "HOME": str(tmp_path / "home"), "HU_PYTHON": str(fake)}
    full.update(env)
    r = subprocess.run(["/bin/bash", str(SCRIPTS / "learned_style_nightly.sh")],
                       env=full, capture_output=True, text=True, timeout=30)
    return r, rec


def test_wrapper_is_noop_when_gate_is_zero(tmp_path):
    r, rec = _run_wrapper(tmp_path, HU_LEARNED_STYLE_LEARN="0")
    assert r.returncode == 0
    assert not rec.exists()
    assert "skipped" in r.stdout


def test_wrapper_runs_learner_then_drift_by_default(tmp_path):
    r, rec = _run_wrapper(tmp_path)
    assert r.returncode == 0, r.stderr
    calls = rec.read_text().splitlines()
    assert len(calls) == 2
    assert "learned_style_profile.py --persona seth" in calls[0]
    assert "learned_style_drift.py --persona seth" in calls[1]
