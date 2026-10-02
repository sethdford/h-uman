"""Hermetic tests for replay_driver.py: arm parsing, the snapshot (source
never written, cloud keys dropped, consistent SQLite copy), the per-arm env
(no inherited HU_* leaks between arms, isolation vars set, network fenced),
and a full run against a fake `human` binary — no model, no chat.db, no
~/.human.
"""
import hashlib
import json
import os
import sqlite3
import stat
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import replay_driver as rd  # noqa: E402


def test_parse_arm():
    assert rd.parse_arm("off:") == ("off", {})
    assert rd.parse_arm("live:HU_THREAD_CONTEXT=live,HU_LENGTH_POLICY=live") == (
        "live", {"HU_THREAD_CONTEXT": "live", "HU_LENGTH_POLICY": "live"})
    for bad in ("nocolon", ":HU_X=1", "a:PATH=/bin", "a:HU_X", "a b:HU_X=1"):
        with pytest.raises(ValueError):
            rd.parse_arm(bad)


def test_is_loopback():
    assert rd.is_loopback("http://127.0.0.1:8741/v1")
    assert rd.is_loopback("http://localhost:8743")
    assert rd.is_loopback("http://[::1]:8741")
    for bad in ("https://api.openai.com/v1", "http://localhost.evil.com/",
                "http://u@127.0.0.1:8741", "ftp://127.0.0.1", "127.0.0.1:8741"):
        assert not rd.is_loopback(bad)


def sha_tree(root):
    h = hashlib.sha256()
    for dirpath, _, files in sorted(os.walk(root)):
        for fn in sorted(files):
            p = os.path.join(dirpath, fn)
            h.update(p.encode())
            with open(p, "rb") as f:
                h.update(f.read())
    return h.hexdigest()


@pytest.fixture
def state_src(tmp_path):
    src = tmp_path / "dot_human"
    (src / "personas").mkdir(parents=True)
    (src / "personas" / "seth.json").write_text('{"name":"seth"}')
    (src / "config.json").write_text(json.dumps({
        "default_model": "local-model",
        "providers": [
            {"name": "gemini", "base_url": "https://aiplatform.googleapis.com/v1", "api_key": "CLOUD"},
            {"name": "mlx_local", "base_url": "http://127.0.0.1:8741/v1", "api_key": "LOCAL"},
        ]}))
    (src / "small.json").write_text("{}")
    (src / "huge.json").write_text("x" * (rd.SMALL_JSON_MAX + 1))
    (src / ".env").write_text("SECRET=1")
    con = sqlite3.connect(str(src / "memory.db"))
    con.execute("PRAGMA journal_mode=WAL")
    con.execute("CREATE TABLE memories (k TEXT)")
    con.execute("INSERT INTO memories VALUES ('fact')")
    con.commit()
    con.close()
    return src


def test_snapshot_copies_without_writing_the_source(state_src, tmp_path):
    before = sha_tree(state_src)
    base = tmp_path / "run" / "state" / "base"
    copied = rd.snapshot(str(state_src), str(base))
    assert sha_tree(state_src) == before
    assert "memory.db" in copied and "personas/" in copied and "small.json" in copied
    assert not (base / "huge.json").exists() and not (base / ".env").exists()
    con = sqlite3.connect(str(base / "memory.db"))
    assert con.execute("SELECT k FROM memories").fetchall() == [("fact",)]
    con.close()
    cfg = json.loads((base / "config.json").read_text())
    keys = {p["name"]: p.get("api_key") for p in cfg["providers"]}
    assert keys == {"gemini": None, "mlx_local": "LOCAL"}
    for p in (base / "config.json", base / "memory.db", base / "personas" / "seth.json"):
        assert stat.S_IMODE(os.stat(p).st_mode) == 0o600


def test_snapshot_refuses_without_memory_db(tmp_path):
    src = tmp_path / "empty"
    src.mkdir()
    with pytest.raises(RuntimeError):
        rd.snapshot(str(src), str(tmp_path / "base"))


def test_arm_env_isolates_and_fences():
    parent = {"PATH": "/bin", "HU_STALE": "1", "HU_IS_TEST": "1", "HOME": "/h",
              "HU_CHATDB": "/x/chat.db"}
    env = rd.arm_env(parent, {"HU_BASE": "live", "HU_X": "off"}, {"HU_X": "live"}, "/r/state/a")
    assert "HU_STALE" not in env and "HU_IS_TEST" not in env and "HU_CHATDB" not in env
    assert env["HOME"] == "/r/state/a/home" and env["TMPDIR"] == "/r/state/a/tmp"
    assert env["HU_BASE"] == "live" and env["HU_X"] == "live"
    assert env["HU_STATE_DIR"] == "/r/state/a"
    assert env["HU_MEMORY_SQLITE_PATH"] == "/r/state/a/memory.db"
    assert env["HTTPS_PROXY"] == rd.DEAD_PROXY and "127.0.0.1" in env["NO_PROXY"]
    assert env["PATH"] == "/bin"


FAKE_HUMAN = r'''#!/usr/bin/env python3
import json, os, sqlite3, sys
a = sys.argv[1:]
arg = lambda k: a[a.index(k) + 1]
assert a[0] == "replay"
rows = [json.loads(l) for l in open(arg("--in")) if l.strip()]
assert len(rows) == 1  # one turn per process
r = rows[0]
escape = os.environ.get("HU_FAKE_ESCAPE")
escaped = False
if escape:
    try:
        with open(escape, "w") as f:
            f.write("x")
        escaped = True
    except OSError:
        pass
mem = sqlite3.connect(os.environ["HU_MEMORY_SQLITE_PATH"])
seen = [k for (k,) in mem.execute("SELECT k FROM memories ORDER BY k")]
mem.execute("INSERT INTO memories VALUES ('written-by-' || ?, '2000-01-01 00:00:00')", (r["id"],))
mem.commit()
with open(arg("--out"), "w") as f:
    if os.environ.get("HU_FAKE_MALFORMED") == r["id"]:
        f.write("{not json\n")
    else:
        f.write(json.dumps({"id": r["id"], "arm": arg("--arm"), "action": "text",
                            "bubbles": [os.environ.get("HU_GATE", "unset")], "bubble_count": 1,
                            "reply_fp": "%016x" % len(os.environ.get("HU_GATE", "")),
                            "home": os.environ["HOME"], "state": os.environ["HU_STATE_DIR"],
                            "seen": seen, "escaped": escaped}) + "\n")
'''


def make_run(tmp_path, n_turns=3):
    root = tmp_path / "runs"
    run = root / "r1"
    base = run / "state" / "base"
    base.mkdir(parents=True)
    con = sqlite3.connect(str(base / "memory.db"))
    con.execute("CREATE TABLE memories (k TEXT, created_at TEXT)")
    # one memory per turn, written 30 s after that turn
    for i in range(n_turns):
        con.execute("INSERT INTO memories VALUES (?, datetime(?, 'unixepoch'))",
                    (f"m{i}", 1_790_000_000 + 100 * i + 30))
    con.commit()
    con.close()
    with open(run / "turns.jsonl", "w") as f:
        for i in range(n_turns):
            f.write(json.dumps({"id": f"t{i}", "contact_id": "+1", "ts": 1_790_000_000 + 100 * i,
                                "inbound_bubbles": ["hi"], "history": [],
                                "seth_reply_bubbles": ["yo"]}) + "\n")
    fake = tmp_path / "fake_human"
    fake.write_text(FAKE_HUMAN)
    fake.chmod(0o755)
    return root, run, fake


def run_driver(root, fake, *extra):
    return rd.main(["run", "--name", "r1", "--run-root", str(root), "--human", str(fake),
                    "--delay-ms", "0", *extra])


def rows_of(run, arm):
    return [json.loads(l) for l in open(run / "out" / f"{arm}.jsonl")]


def test_run_gives_each_arm_its_env_and_a_fresh_state(tmp_path, monkeypatch, capsys):
    root, run, fake = make_run(tmp_path)
    monkeypatch.setenv("HU_GATE", "leaked-from-parent")
    assert run_driver(root, fake, "--arm", "off:", "--arm", "on:HU_GATE=live") == 0
    off, on = rows_of(run, "off"), rows_of(run, "on")
    assert {r["bubbles"][0] for r in off} == {"unset"}  # parent HU_* never leaks
    assert {r["bubbles"][0] for r in on} == {"live"}
    for r in off + on:
        assert r["home"].startswith(r["state"])  # HOME is inside the scratch state
    manifest = json.loads((run / "manifest.json").read_text())
    assert [a["complete"] for a in manifest["arms"]] == [True, True]
    assert manifest["turn_ids"] == ["t0", "t1", "t2"]


def test_each_turn_sees_only_its_past_and_no_other_turns_writes(tmp_path, capsys):
    root, run, fake = make_run(tmp_path)
    assert run_driver(root, fake, "--arm", "a:", "--arm", "b:") == 0
    for arm in ("a", "b"):
        seen = {r["id"]: r["seen"] for r in rows_of(run, arm)}
        # turn i sees memories m0..m(i-1): later ones are cut, and nothing a
        # previous turn (or the other arm) wrote survives into it.
        assert seen == {"t0": [], "t1": ["m0"], "t2": ["m0", "m1"]}
    manifest = json.loads((run / "manifest.json").read_text())
    assert manifest["time_filter"]["rows_deleted"] == 3 + 2 + 1
    assert manifest["time_filter"]["errors"] == []


def test_run_marks_a_malformed_row_incomplete_without_raising(tmp_path, capsys):
    root, run, fake = make_run(tmp_path)
    rc = run_driver(root, fake, "--arm", "bad:HU_FAKE_MALFORMED=t1")
    assert rc == 1
    out = capsys.readouterr().out
    assert "rows 2/3" in out and "malformed 1" in out and "INCOMPLETE" in out
    assert [r["id"] for r in rows_of(run, "bad")] == ["t0", "t2"]


def test_run_refuses_a_cloud_endpoint(tmp_path, capsys):
    root, run, fake = make_run(tmp_path)
    rc = run_driver(root, fake, "--arm", "off:", "--endpoint", "https://api.openai.com/v1")
    assert rc == 2
    assert not (run / "out").exists()


@pytest.mark.skipif(not os.path.exists(rd.SANDBOX_EXEC), reason="macOS sandbox-exec only")
def test_sandbox_blocks_writes_outside_the_turn_dir(tmp_path, capsys):
    root, run, fake = make_run(tmp_path, n_turns=1)
    target = tmp_path / "live_dot_human" / "training-data.jsonl"
    target.parent.mkdir()
    assert run_driver(root, fake, "--sandbox", "on",
                      "--arm", f"x:HU_FAKE_ESCAPE={target}") == 0
    assert rows_of(run, "x")[0]["escaped"] is False
    assert not target.exists()


def test_without_sandbox_the_same_write_would_land(tmp_path, capsys):
    # The control for the test above: the escape attempt is real.
    root, run, fake = make_run(tmp_path, n_turns=1)
    target = tmp_path / "elsewhere.jsonl"
    assert run_driver(root, fake, "--sandbox", "off",
                      "--arm", f"x:HU_FAKE_ESCAPE={target}") == 0
    assert rows_of(run, "x")[0]["escaped"] is True and target.exists()


def test_sandbox_profile_confines_writes_and_network(tmp_path):
    prof = rd.sandbox_profile(str(tmp_path))
    assert f'(subpath "{os.path.realpath(tmp_path)}")' in prof
    assert "deny file-write*" in prof and "deny network-outbound" in prof
    assert 'remote ip "localhost:*"' in prof


def test_snapshot_reads_uncheckpointed_wal_without_touching_the_source(tmp_path):
    src = tmp_path / "live"
    src.mkdir()
    con = sqlite3.connect(str(src / "memory.db"))
    con.execute("PRAGMA journal_mode=WAL")
    con.execute("PRAGMA wal_autocheckpoint=0")
    con.execute("CREATE TABLE m (k TEXT)")
    con.execute("INSERT INTO m VALUES ('only-in-wal')")
    con.commit()  # the daemon keeps its connection open: nothing is checkpointed
    before = sha_tree(src)
    rd.snapshot(str(src), str(tmp_path / "base"))
    assert sha_tree(src) == before
    copy = sqlite3.connect(str(tmp_path / "base" / "memory.db"))
    assert copy.execute("SELECT k FROM m").fetchall() == [("only-in-wal",)]
    copy.close()
    con.close()


def test_time_filter_keeps_uptime_stamped_ms_rows_and_reports_errors(tmp_path):
    con = sqlite3.connect(str(tmp_path / "memory.db"))
    con.execute("CREATE TABLE contact_insights (created_at_ms INTEGER)")
    con.executemany("INSERT INTO contact_insights VALUES (?)",
                    [(1_790_000_000_000 - 1,), (1_790_000_000_000 + 1,), (5_000_000,)])
    con.execute("CREATE TABLE episodes (created_at INTEGER)")
    con.executemany("INSERT INTO episodes VALUES (?)", [(1_789_999_999,), (1_790_000_001,)])
    con.commit()
    con.close()
    rep = rd.filter_to_cutoff(str(tmp_path), 1_790_000_000)
    con = sqlite3.connect(str(tmp_path / "memory.db"))
    assert sorted(r[0] for r in con.execute("SELECT * FROM contact_insights")) == [
        5_000_000, 1_790_000_000_000 - 1]
    assert [r[0] for r in con.execute("SELECT * FROM episodes")] == [1_789_999_999]
    assert rep["deleted"] == 2 and rep["errors"] == []
    assert "memory.db:memories" in rep["absent"] and "graph.db:entities" in rep["absent"]
