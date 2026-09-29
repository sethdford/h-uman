"""Runner contract (spec §5, §6): refuse and write nothing; counts-only manifest."""
import contextlib
import datetime as dt
import fcntl
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import backend as be, run_nightly  # noqa: E402

LOCAL_MORNING = dt.datetime(2026, 9, 29, 7, 45).astimezone()   # a Tuesday


def mk_mem(p):
    m = sqlite3.connect(p)
    m.executescript("""
      CREATE TABLE messages (id INTEGER PRIMARY KEY, session_id TEXT, role TEXT, content TEXT,
                             created_at TEXT);
      CREATE TABLE contact_insights (id INTEGER PRIMARY KEY AUTOINCREMENT, contact_id TEXT,
        kind TEXT, insight TEXT, confidence REAL, as_of_ms INTEGER, source TEXT,
        created_at_ms INTEGER, retired_at_ms INTEGER DEFAULT 0, evidence_ids TEXT,
        superseded_by_id INTEGER DEFAULT 0);
      INSERT INTO messages VALUES (1, '+1a', 'user', 'priya surgery is tuesday', '');
      INSERT INTO contact_insights (contact_id, insight, confidence, source, created_at_ms,
        evidence_ids) VALUES ('+1a', 'SECRET-NOTE priya surgery', 0.9, 'extractor:v1', 1, '[1]');
    """)
    m.commit()
    m.close()


def mk_chat(p):
    c = sqlite3.connect(p)
    c.executescript("CREATE TABLE message (ROWID INTEGER PRIMARY KEY, text TEXT,"
                    " attributedBody BLOB);")
    c.commit()
    c.close()


class Fake:
    name = "fake@local"
    base_url = "http://127.0.0.1:8743"
    model = "m"

    def __init__(self, outputs):
        self.outputs = list(outputs)

    def generate(self, system, user, max_tokens=400):
        out = self.outputs.pop(0)
        if isinstance(out, Exception):
            raise out
        return out


def serve_with(backend_obj):
    @contextlib.contextmanager
    def serve(**kw):
        yield backend_obj
    return serve


def args(tmp_path, *extra):
    return ["--mem-db", str(tmp_path / "mem.db"), "--chat-db", str(tmp_path / "chat.db"),
            "--store", str(tmp_path / "so.db"), "--manifest-dir", str(tmp_path / "logs"),
            "--reports-dir", str(tmp_path / "reports"), "--lock", str(tmp_path / "lock"),
            "--blind-ab-root", str(tmp_path / "ab"), "--jobs", "audit", *extra]


def setup(tmp_path):
    mk_mem(str(tmp_path / "mem.db"))
    mk_chat(str(tmp_path / "chat.db"))


def no_att(*a, **k):
    return {"timelines": {}, "labeled": {}}


def test_happy_path_writes_counts_only_manifest(tmp_path):
    setup(tmp_path)
    rc = run_nightly.main(args(tmp_path), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake(["supported\nok"])), attribute=no_att)
    assert rc == 0
    man_path = tmp_path / "logs" / "second-opinion-20260929.json"
    text = man_path.read_text()
    man = json.loads(text)
    assert man["audit"]["audited"] == 1 and man["backend"] == "fake@local"
    assert "SECRET-NOTE" not in text and "+1a" not in text and "priya" not in text
    assert sqlite3.connect(tmp_path / "so.db").execute("SELECT COUNT(*) FROM runs").fetchone()[0] == 1


def test_unreadable_db_refuses_and_writes_nothing(tmp_path):
    mk_chat(str(tmp_path / "chat.db"))                     # no memory.db
    rc = run_nightly.main(args(tmp_path), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake([])), attribute=no_att)
    assert rc == 2
    assert not (tmp_path / "logs").exists() and not (tmp_path / "so.db").exists()


def test_server_that_never_comes_up_refuses(tmp_path):
    setup(tmp_path)

    @contextlib.contextmanager
    def broken(**kw):
        raise be.BackendError("not healthy")
        yield

    assert run_nightly.main(args(tmp_path), now_local=LOCAL_MORNING, serve=broken,
                            attribute=no_att) == 2
    assert not (tmp_path / "logs").exists()


def test_window_closed_writes_nothing(tmp_path):
    setup(tmp_path)
    late = dt.datetime(2026, 9, 29, 9, 30).astimezone()
    rc = run_nightly.main(args(tmp_path, "--deadline", "09:00"), now_local=late,
                          serve=serve_with(Fake([])), attribute=no_att)
    assert rc == 0 and not (tmp_path / "logs").exists() and not (tmp_path / "so.db").exists()


def test_second_concurrent_run_does_nothing(tmp_path):
    setup(tmp_path)
    with open(tmp_path / "lock", "w") as held:
        fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
        rc = run_nightly.main(args(tmp_path), now_local=LOCAL_MORNING,
                              serve=serve_with(Fake([])), attribute=no_att)
    assert rc == 0 and not (tmp_path / "logs").exists()


def test_every_item_failing_exits_3_and_keeps_the_manifest(tmp_path):
    setup(tmp_path)
    rc = run_nightly.main(args(tmp_path), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake([TimeoutError()])), attribute=no_att)
    assert rc == 3
    man = json.loads((tmp_path / "logs" / "second-opinion-20260929.json").read_text())
    assert man["audit"]["errors"] == 1 and man["exit_reason"] == "every item failed"


def test_dry_run_writes_no_rows_and_suffixes_manifest(tmp_path):
    setup(tmp_path)
    rc = run_nightly.main(args(tmp_path, "--dry-run"), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake(["supported"])), attribute=no_att)
    assert rc == 0 and not (tmp_path / "so.db").exists()
    assert (tmp_path / "logs" / "second-opinion-20260929-dryrun.json").exists()


def test_auto_jobs_add_judge_and_report_on_sunday():
    assert run_nightly.resolve_jobs("auto", dt.date(2026, 9, 29)) == ["audit", "gold"]
    assert run_nightly.resolve_jobs("auto", dt.date(2026, 10, 4)) == ["audit", "gold", "judge",
                                                                       "report"]
    assert run_nightly.resolve_jobs("audit,report", dt.date(2026, 9, 29)) == ["audit", "report"]
