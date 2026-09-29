"""graph_retype_entities.py (spec 2026-09-29 §4.5): backup before any write, refuse on a
failed backup or a locked graph, retype-only lines through the C importer.
Hermetic: temp sqlite, fake model, fake `human` binary; never ~/.human. One test runs
the real built `build/human` against a temp graph (HOME pointed at tmp_path) and is
skipped when that binary is absent."""
import hashlib
import json
import os
import sqlite3
import stat
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import graph_retype_entities as rt  # noqa: E402

C = "+15550000042"
REAL_BIN = ROOT / "build" / "human"

FAKE_BIN = """#!{py}
import json, os, sys
lines = open(sys.argv[3]).read().splitlines()
with open(os.environ["FAKE_LOG"], "a") as f:
    f.write(json.dumps({{"argv": sys.argv[1:], "graph": os.environ.get("HU_GRAPH_DB"),
                        "lines": lines}}) + "\\n")
print(json.dumps({{"imported": 0, "entities": len(lines), "skipped": 0, "graph": "x"}}))
sys.exit(int(os.environ.get("FAKE_RC", "0")))
"""


def make_graph(path, wal=False):
    con = sqlite3.connect(path)
    if wal:
        con.execute("PRAGMA journal_mode=WAL")
    con.execute("CREATE TABLE entities (id INTEGER PRIMARY KEY AUTOINCREMENT, contact_id TEXT"
                " NOT NULL, name TEXT NOT NULL, type INTEGER NOT NULL DEFAULT 6, provenance TEXT)")
    con.executemany("INSERT INTO entities (contact_id, name, type) VALUES (?, ?, ?)", [
        (C, "Salim", 6), (C, "the lake house", 6), (C, "+15551234567", 6), (C, C, 6),
        (C, "Tampa", 1), ("self", "Vanguard", 6)])
    con.commit()
    con.close()


def digest(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def fake_calls(tmp_path):
    p = tmp_path / "fake.json"
    return [json.loads(ln) for ln in p.read_text().splitlines()] if p.exists() else []


@pytest.fixture
def env(tmp_path, monkeypatch):
    g = tmp_path / "graph.db"
    make_graph(g)
    fake = tmp_path / "human"
    fake.write_text(FAKE_BIN.format(py=sys.executable))
    fake.chmod(0o755)
    monkeypatch.setenv("FAKE_LOG", str(tmp_path / "fake.json"))
    monkeypatch.setattr(rt.urllib.request, "urlopen", lambda *a, **k: None)
    argv = ["--graph-db", str(g), "--backup-dir", str(tmp_path / "backups"),
            "--work-dir", str(tmp_path / "work"), "--human-bin", str(fake)]
    return tmp_path, g, argv


TYPED = {"Salim": "person", "the lake house": "topic", "Vanguard": "org"}


def answer(url, model, system, user, **kw):
    names = user.split("\n")
    return json.dumps([{"name": n, "type": TYPED[n]} for n in names if n in TYPED]
                      + [{"name": "Invented", "type": "person"}])


def last_json(capsys):
    return json.loads(capsys.readouterr().out.strip().splitlines()[-1])


# ── pure pieces ──────────────────────────────────────────────────────────────

def test_unknown_entities_skips_placeholders_and_typed_rows(env):
    _, g, _ = env
    con = sqlite3.connect(g)
    assert rt.unknown_entities(con) == {C: ["Salim", "the lake house"], "self": ["Vanguard"]}


def test_unknown_entities_skips_names_with_line_breaks(tmp_path):
    g = tmp_path / "g.db"
    make_graph(g)
    con = sqlite3.connect(g)
    con.execute("INSERT INTO entities (contact_id, name, type) VALUES (?, ?, 6)",
                (C, "two\nlines"))
    assert "two\nlines" not in rt.unknown_entities(con)[C]


def test_parse_types_accepts_only_exact_batch_names_and_known_types():
    """Retype-only keys on contact_id + the stored spelling, so a case variant of a
    stored name is ignored (and counted), never mapped."""
    raw = ('[{"name":"salim","type":"person"},{"name":"Nobody","type":"person"},'
           '{"name":"the lake house","type":"planet"},{"name":" Salim ","type":"Person"}]')
    stats = {}
    assert rt.parse_types(raw, ["Salim", "the lake house"], stats) == {"Salim": "person"}
    assert stats == {"ignored": 3}


def test_parse_types_keeps_first_answer_for_a_name_and_counts_the_rest():
    stats = {}
    raw = '[{"name":"Salim","type":"person"},{"name":"Salim","type":"place"}]'
    assert rt.parse_types(raw, ["Salim"], stats) == {"Salim": "person"}
    assert stats == {"ignored": 1}


def test_parse_types_unparseable_is_counted():
    stats = {}
    assert rt.parse_types("nope", ["Salim"], stats) == {}
    assert stats == {"parse_failed": 1}
    assert rt.parse_types("sure! [] none", ["Salim"], stats) == {}
    assert stats == {"parse_failed": 1}  # an empty array is an answer, not a failure


def test_batches_are_40():
    assert [len(b) for b in rt.batches([str(i) for i in range(85)])] == [40, 40, 5]


# ── backup ───────────────────────────────────────────────────────────────────

def test_backup_is_private_verified_and_standalone_from_a_live_wal_graph(tmp_path):
    g = tmp_path / "graph.db"
    make_graph(g, wal=True)
    daemon = sqlite3.connect(g)  # the daemon holds the graph open (not mid-write)
    daemon.execute("SELECT COUNT(*) FROM entities").fetchone()
    try:
        import datetime as dt
        bk = rt.backup(str(g), str(tmp_path / "backups"), dt.datetime(2026, 9, 29, 12, 0, 0))
    finally:
        daemon.close()
    assert os.path.basename(bk) == "graph.db.bak-retype-20260929-120000"
    assert stat.S_IMODE(os.stat(bk).st_mode) == 0o600
    assert stat.S_IMODE(os.stat(tmp_path / "backups").st_mode) == 0o700
    ro = sqlite3.connect(f"file:{bk}?mode=ro", uri=True)
    assert ro.execute("PRAGMA journal_mode").fetchone()[0] == "delete"
    assert ro.execute("SELECT COUNT(*) FROM entities").fetchone()[0] == 6
    assert sorted(os.listdir(tmp_path / "backups")) == [os.path.basename(bk)]


def test_backup_never_overwrites_and_removes_a_failed_copy(tmp_path):
    import datetime as dt
    now = dt.datetime(2026, 9, 29, 12, 0, 0)
    g = tmp_path / "graph.db"
    make_graph(g)
    rt.backup(str(g), str(tmp_path / "b"), now)
    with pytest.raises(FileExistsError):
        rt.backup(str(g), str(tmp_path / "b"), now)  # same second: O_EXCL
    bad = tmp_path / "not-a-graph.db"
    sqlite3.connect(bad).execute("CREATE TABLE x (y)").connection.commit()
    with pytest.raises(sqlite3.Error):
        rt.backup(str(bad), str(tmp_path / "c"), now)  # no entities table
    assert os.listdir(tmp_path / "c") == []


def test_backup_refuses_a_symlinked_backup_dir(tmp_path):
    import datetime as dt
    g = tmp_path / "graph.db"
    make_graph(g)
    (tmp_path / "elsewhere").mkdir()
    (tmp_path / "b").symlink_to(tmp_path / "elsewhere")
    with pytest.raises(OSError):
        rt.backup(str(g), str(tmp_path / "b"), dt.datetime(2026, 9, 29))
    assert os.listdir(tmp_path / "elsewhere") == []


# ── main ─────────────────────────────────────────────────────────────────────

def test_modes_are_required_and_exclusive(env):
    _, _, argv = env
    for extra in ([], ["--dry-run", "--write"]):
        with pytest.raises(SystemExit) as e:
            rt.main(argv + extra)
        assert e.value.code == 2


def test_dry_run_counts_only(env, monkeypatch, capsys):
    tmp_path, g, argv = env
    monkeypatch.setattr(rt, "call_model", lambda *a, **k: pytest.fail("model called"))
    before = digest(g)
    assert rt.main(argv + ["--dry-run"]) == 0
    out = json.loads(capsys.readouterr().out)
    assert out == {"unknown_entities": 3, "contacts": 2, "batches": 2}
    assert not (tmp_path / "backups").exists()
    assert fake_calls(tmp_path) == [] and digest(g) == before


def test_write_backs_up_first_then_imports_retype_only_lines(env, monkeypatch, capsys):
    tmp_path, g, argv = env
    order = []
    real_backup = rt.backup
    monkeypatch.setattr(rt, "backup", lambda *a: order.append("backup") or real_backup(*a))
    monkeypatch.setattr(rt, "call_model", lambda *a, **k: order.append("model") or answer(*a, **k))
    before = digest(g)
    assert rt.main(argv + ["--write"]) == 0
    assert order[0] == "backup" and "model" in order
    assert digest(g) == before  # Python never writes graph.db
    bk = list((tmp_path / "backups").glob("graph.db.bak-retype-*"))
    assert len(bk) == 1 and stat.S_IMODE(os.stat(bk[0]).st_mode) == 0o600
    ro = sqlite3.connect(f"file:{bk[0]}?mode=ro", uri=True)
    assert ro.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
    (log,) = fake_calls(tmp_path)
    assert log["argv"][:2] == ["memory", "import-facts"] and log["graph"] == str(g)
    jsonl = log["argv"][2]
    assert stat.S_IMODE(os.stat(jsonl).st_mode) == 0o600
    lines = [json.loads(ln) for ln in log["lines"]]
    assert {(ln["contact"], ln["name"], ln["type"]) for ln in lines} == {
        (C, "Salim", "person"), (C, "the lake house", "topic"), ("self", "Vanguard", "org")}
    assert all(ln["retype_only"] is True and ln["source"] == "names:migrate"
               and ln["kind"] == "entity" and ln["confidence"] == 0.6 for ln in lines)
    out = capsys.readouterr().out
    assert "Salim" not in out and "Vanguard" not in out and C not in out  # counts only
    res = json.loads(out.strip().splitlines()[-1])
    assert res["applied"] == 3 and res["answered"] == 3 and res["unanswered"] == 0
    assert res["ignored"] == 2 and res["parse_failed"] == 0 and res["model_errors"] == 0
    assert res["by_type"] == {"person": 1, "place": 0, "org": 1, "event": 0, "topic": 1}


def test_rerun_with_nothing_unknown_touches_nothing(env, monkeypatch, capsys):
    """Idempotence: once every row is typed, a re-run calls neither the model, the
    backup nor the importer."""
    tmp_path, g, argv = env
    con = sqlite3.connect(g)
    con.executemany("UPDATE entities SET type = ? WHERE name = ?",
                    [(0, "Salim"), (4, "the lake house"), (2, "Vanguard")])
    con.commit()
    con.close()
    monkeypatch.setattr(rt, "call_model", lambda *a, **k: pytest.fail("model called"))
    before = digest(g)
    assert rt.main(argv + ["--write"]) == 0
    assert last_json(capsys)["applied"] == 0
    assert fake_calls(tmp_path) == [] and not (tmp_path / "backups").exists()
    assert digest(g) == before


def test_unparseable_batch_is_counted_and_the_rest_continue(env, monkeypatch, capsys):
    tmp_path, _, argv = env
    monkeypatch.setattr(rt, "call_model", lambda url, m, s, user, **k:
                        "I cannot" if "Vanguard" in user else answer(url, m, s, user))
    assert rt.main(argv + ["--write"]) == 0
    res = last_json(capsys)
    assert res["parse_failed"] == 1 and res["answered"] == 2 and res["unanswered"] == 1
    (log,) = fake_calls(tmp_path)
    assert len(log["lines"]) == 2


def test_every_batch_unparseable_exits_3(env, monkeypatch):
    tmp_path, _, argv = env
    monkeypatch.setattr(rt, "call_model", lambda *a, **k: "no idea")
    assert rt.main(argv + ["--write"]) == 3
    assert fake_calls(tmp_path) == []


def test_backup_failure_refuses_before_any_model_call(env, monkeypatch):
    tmp_path, g, argv = env
    def broken(*a):
        raise OSError("disk full")
    monkeypatch.setattr(rt, "backup", broken)
    monkeypatch.setattr(rt, "call_model", lambda *a, **k: pytest.fail("model called"))
    before = digest(g)
    assert rt.main(argv + ["--write"]) == 2
    assert fake_calls(tmp_path) == [] and digest(g) == before


def test_locked_graph_refuses(env, monkeypatch):
    tmp_path, g, argv = env
    holder = sqlite3.connect(g)
    holder.execute("BEGIN IMMEDIATE")  # the daemon mid-write
    monkeypatch.setattr(rt, "call_model", lambda *a, **k: pytest.fail("model called"))
    try:
        assert rt.main(argv + ["--write"]) == 2
    finally:
        holder.rollback()
        holder.close()
    assert not (tmp_path / "backups").exists()  # refused before the backup


def test_missing_graph_refuses_and_is_not_created(env):
    tmp_path, _, argv = env
    missing = tmp_path / "nope.db"
    assert rt.main(argv + ["--write", "--graph-db", str(missing)]) == 2
    assert not missing.exists()


def test_every_batch_errored_exits_3_and_imports_nothing(env, monkeypatch):
    tmp_path, _, argv = env
    def boom(*a, **k):
        raise TimeoutError("down")
    monkeypatch.setattr(rt, "call_model", boom)
    assert rt.main(argv + ["--write"]) == 3
    assert fake_calls(tmp_path) == []


def test_import_failure_exits_2(env, monkeypatch):
    _, _, argv = env
    monkeypatch.setenv("FAKE_RC", "1")
    monkeypatch.setattr(rt, "call_model", answer)
    assert rt.main(argv + ["--write"]) == 2


@pytest.mark.parametrize("url", ["https://example.com/v1/chat/completions",
                                 "http://127.0.0.1.evil.com/v1/chat/completions",
                                 "http://127.0.0.1@evil.com/v1/chat/completions"])
def test_refuses_a_remote_model(env, url):
    tmp_path, _, argv = env
    assert rt.main(argv + ["--write", "--url", url]) == 2
    assert not (tmp_path / "backups").exists()


# ── the real importer ────────────────────────────────────────────────────────

def _rows(g):
    con = sqlite3.connect(f"file:{g}?mode=ro", uri=True)
    try:
        return {r[0]: r[1:] for r in con.execute(
            "SELECT name, contact_id, type, last_seen, mention_count, provenance "
            "FROM entities ORDER BY id")}
    finally:
        con.close()


@pytest.mark.skipif(not os.access(REAL_BIN, os.X_OK), reason="build/human not built")
def test_real_importer_retypes_without_bumping_creating_or_downgrading(tmp_path, monkeypatch,
                                                                       capsys):
    monkeypatch.setenv("HOME", str(tmp_path / "home"))  # the binary never sees ~/.human
    (tmp_path / "home").mkdir()
    g = tmp_path / "graph.db"
    seed = tmp_path / "seed.jsonl"
    seed.write_text(json.dumps({"kind": "entity", "contact": C, "name": "Seed", "type": "topic",
                                "source": "test", "confidence": 0.5}) + "\n")
    r = subprocess.run([str(REAL_BIN), "memory", "import-facts", str(seed)],
                       env={**os.environ, "HU_GRAPH_DB": str(g)}, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr  # the C schema, created by the C code
    con = sqlite3.connect(g)
    con.executemany(
        "INSERT INTO entities (contact_id, name, type, first_seen, last_seen, mention_count)"
        " VALUES (?, ?, ?, 1, 2, 3)",
        [(C, "Salim", 6), (C, "the lake house", 6), (C, "Mystery", 6), (C, "Tampa", 1),
         ("self", "Vanguard", 6)])
    con.commit()
    con.close()
    monkeypatch.setattr(rt.urllib.request, "urlopen", lambda *a, **k: None)
    argv = ["--write", "--graph-db", str(g), "--backup-dir", str(tmp_path / "backups"),
            "--work-dir", str(tmp_path / "work"), "--human-bin", str(REAL_BIN)]

    # The daemon types "Salim" as a place after the backup, before the import: the
    # migration's "person" must not downgrade it.
    real_backup = rt.backup
    def backup_then_daemon_types(*a):
        out = real_backup(*a)
        c2 = sqlite3.connect(g)
        c2.execute("UPDATE entities SET type = 1 WHERE name = 'Salim'")
        c2.commit()
        c2.close()
        return out
    monkeypatch.setattr(rt, "backup", backup_then_daemon_types)
    monkeypatch.setattr(rt, "call_model", answer)
    before = _rows(g)
    assert rt.main(argv) == 0
    res = last_json(capsys)
    after = _rows(g)

    assert set(after) == set(before)  # nothing created ("Invented"), nothing deleted
    assert after["the lake house"][1] == 4 and after["Vanguard"][1] == 2  # TOPIC, ORG
    assert after["the lake house"][4] == after["Vanguard"][4] == "names:migrate"
    assert after["Salim"][1] == 1 and after["Salim"][4] is None  # not downgraded, no stamp
    assert after["Mystery"] == before["Mystery"]  # unanswered stays UNKNOWN, untouched
    assert after["Tampa"] == before["Tampa"]
    for n in ("Salim", "the lake house", "Vanguard", "Mystery"):
        assert after[n][2:4] == (2, 3)  # last_seen, mention_count never bumped
    assert res["unknown_after"] == 1
    bk = next((tmp_path / "backups").glob("graph.db.bak-retype-*"))
    assert _rows(bk)["Salim"][1] == 6  # the backup is the pre-migration graph

    # Re-run: only "Mystery" is still UNKNOWN; typed rows stay exactly as they are.
    monkeypatch.setattr(rt, "backup", real_backup)
    monkeypatch.setattr(rt, "call_model", lambda url, m, s, user, **k: json.dumps(
        [{"name": "Mystery", "type": "event"}, {"name": "Salim", "type": "person"}]))
    import time
    time.sleep(1.1)  # a distinct backup timestamp
    assert rt.main(argv) == 0
    again = _rows(g)
    assert again["Mystery"][1] == 3 and again["Mystery"][2:4] == (2, 3)
    assert {n: v for n, v in again.items() if n != "Mystery"} == \
        {n: v for n, v in after.items() if n != "Mystery"}
    assert last_json(capsys)["ignored"] == 1  # "Salim" was not in the batch
