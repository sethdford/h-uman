"""Runner contract (spec §5, §6): refuse and write nothing; counts-only manifest.

Fix round 1 additions (task-8-review.md I1-I4 + minors): Vertex credential
preflight (I1), per-job isolation with finish_run always running (I2), the
judge job honouring --deadline (I3), and mutant-killing assertions for M1,
M2, M3, M4, M7, M9 (I4)."""
import contextlib
import csv
import datetime as dt
import os
import fcntl
import json
import sqlite3
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
import pytest  # noqa: E402

from second_opinion import backend as be, run_nightly  # noqa: E402

LOCAL_MORNING = dt.datetime(2026, 9, 29, 7, 45).astimezone()   # a Tuesday


@pytest.fixture(autouse=True)
def _restore_umask():
    """run_nightly.main sets os.umask(0o077) for the whole process (M1); keep
    that from leaking into other test modules."""
    old = os.umask(0o022)
    os.umask(old)
    yield
    os.umask(old)


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


def mk_mem_no_contact_insights(p):
    """A memory.db missing the contact_insights table entirely (review probe P5):
    audit_pass's SELECT raises sqlite3.OperationalError."""
    m = sqlite3.connect(p)
    m.executescript("""
      CREATE TABLE messages (id INTEGER PRIMARY KEY, session_id TEXT, role TEXT, content TEXT,
                             created_at TEXT);
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


def make_ab_run_dir(tmp_path, with_triples=True):
    """A blind-A/B run dir judge.latest_run_dir will accept (detection-mode key,
    rating_sheet.csv + answer_key.json present). Without triples.json,
    gold.weak_items raises FileNotFoundError (review probe P6)."""
    run_dir = tmp_path / "ab" / "run1"
    run_dir.mkdir(parents=True)
    (run_dir / "rating_sheet.csv").write_text("id,choice\n1,A\n")
    (run_dir / "answer_key.json").write_text(json.dumps({"1": "A"}))
    if with_triples:
        (run_dir / "triples.json").write_text(json.dumps(
            {"1": {"context": "hi", "seth_reply": "yo", "huuman_reply": "hey"}}))
    return run_dir


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
    # M1: open_store/start_run must not run before the server-health refusal.
    assert not (tmp_path / "so.db").exists()


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
    # M1: a second run must never open the store either.
    assert not (tmp_path / "so.db").exists()


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


# ---------------------------------------------------------------------------
# Fix round 1: I1 — Vertex credential preflight
# ---------------------------------------------------------------------------

def test_vertex_missing_credentials_refuses_and_writes_nothing(tmp_path):
    setup(tmp_path)

    def bad_token():
        raise be.BackendError("no ADC credentials")

    rc = run_nightly.main(args(tmp_path, "--backend", "vertex"), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake([])), attribute=no_att, vertex_token=bad_token)
    assert rc == 2
    assert not (tmp_path / "logs").exists() and not (tmp_path / "so.db").exists()


# ---------------------------------------------------------------------------
# Fix round 1: I2 — every job isolated, finish_run always runs
# ---------------------------------------------------------------------------

def test_audit_job_isolated_other_jobs_still_run(tmp_path):
    mk_mem_no_contact_insights(str(tmp_path / "mem.db"))
    mk_chat(str(tmp_path / "chat.db"))
    rc = run_nightly.main(args(tmp_path, "--jobs", "audit,gold"), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake([])), attribute=no_att)
    assert rc == 0
    man_path = tmp_path / "logs" / "second-opinion-20260929.json"
    text = man_path.read_text()
    man = json.loads(text)
    assert man["audit"] == {"error": "OperationalError"}
    assert "no such table" not in text
    assert "gold" in man and "error" not in man["gold"]
    row = sqlite3.connect(tmp_path / "so.db").execute(
        "SELECT finished_at_ms FROM runs").fetchone()
    assert row[0] is not None


def test_gold_job_isolated_other_jobs_still_run(tmp_path, monkeypatch):
    setup(tmp_path)

    def boom(*a, **k):
        raise FileNotFoundError("a path that must never leak")

    monkeypatch.setattr(run_nightly.gold, "gold_pass", boom)
    rc = run_nightly.main(args(tmp_path, "--jobs", "audit,gold"), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake(["supported\nok"])), attribute=no_att)
    assert rc == 0
    man_path = tmp_path / "logs" / "second-opinion-20260929.json"
    text = man_path.read_text()
    man = json.loads(text)
    assert man["gold"] == {"error": "FileNotFoundError"}
    assert man["audit"]["audited"] == 1
    row = sqlite3.connect(tmp_path / "so.db").execute(
        "SELECT finished_at_ms FROM runs").fetchone()
    assert row[0] is not None


def test_gold_job_survives_attribution_failure(tmp_path):
    """M2: the attribution try/except stays in place even now that the whole
    gold job is also wrapped."""
    setup(tmp_path)

    def bad_attr(*a, **k):
        raise RuntimeError("boom")

    rc = run_nightly.main(args(tmp_path, "--jobs", "gold"), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake([])), attribute=bad_attr)
    assert rc == 0
    man = json.loads((tmp_path / "logs" / "second-opinion-20260929.json").read_text())
    assert man["gold_attribution_error"] == 1
    assert "gold" in man and "error" not in man["gold"]


def test_judge_job_failure_is_isolated_and_type_only(tmp_path, monkeypatch):
    """M3/M4: an exception from judge.judge_pass is caught, recorded as the
    exception TYPE NAME only (never the message), and the run still exits
    with a written manifest."""
    setup(tmp_path)
    make_ab_run_dir(tmp_path)

    def boom(*a, **k):
        raise RuntimeError("a secret message that must never leak into the manifest")

    monkeypatch.setattr(run_nightly.judge, "judge_pass", boom)
    rc = run_nightly.main(args(tmp_path, "--jobs", "judge"), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake([])), attribute=no_att)
    assert rc == 3  # the only attempted job raised
    text = (tmp_path / "logs" / "second-opinion-20260929.json").read_text()
    man = json.loads(text)
    assert man["judge"] == {"error": "RuntimeError"}
    assert "secret message" not in text


def test_runs_row_records_exit_code_and_finish_time(tmp_path):
    """M9: finish_run must always run — the runs row is never left dangling."""
    setup(tmp_path)
    rc = run_nightly.main(args(tmp_path), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake(["supported\nok"])), attribute=no_att)
    assert rc == 0
    row = sqlite3.connect(tmp_path / "so.db").execute(
        "SELECT exit_code, finished_at_ms FROM runs").fetchone()
    assert row[0] == 0 and row[1] is not None


def test_all_jobs_together(tmp_path):
    setup(tmp_path)
    rc = run_nightly.main(args(tmp_path, "--jobs", "audit,gold,judge"), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake(["supported\nok"])), attribute=no_att)
    assert rc == 0
    man = json.loads((tmp_path / "logs" / "second-opinion-20260929.json").read_text())
    assert "audit" in man and "gold" in man and "judge" in man
    assert man["judge"] == {"skipped": "no rating sheet"}


# ---------------------------------------------------------------------------
# Fix round 1: I3 — the judge job honours --deadline
# ---------------------------------------------------------------------------

def test_judge_skips_when_deadline_already_passed(tmp_path):
    setup(tmp_path)
    make_ab_run_dir(tmp_path)  # otherwise it skips as "no rating sheet" regardless
    deadline = run_nightly.resolve_deadline("23:59", LOCAL_MORNING)
    past_now = deadline + dt.timedelta(hours=1)
    rc = run_nightly.main(args(tmp_path, "--jobs", "judge", "--deadline", "23:59"),
                          now_local=LOCAL_MORNING, serve=serve_with(Fake([])), attribute=no_att,
                          utcnow=lambda: past_now)
    assert rc == 0
    man = json.loads((tmp_path / "logs" / "second-opinion-20260929.json").read_text())
    assert man["judge"] == {"skipped": "deadline_skipped"}


def test_judge_timeout_expired_is_isolated_and_type_only(tmp_path, monkeypatch):
    setup(tmp_path)
    make_ab_run_dir(tmp_path)
    captured = {}

    def boom(*a, **k):
        captured["timeout"] = k.get("timeout")
        raise subprocess.TimeoutExpired(cmd=["synthetic_judge.py"], timeout=k.get("timeout"))

    monkeypatch.setattr(run_nightly.judge, "judge_pass", boom)
    rc = run_nightly.main(args(tmp_path, "--jobs", "judge", "--deadline", "23:59"),
                          now_local=LOCAL_MORNING, serve=serve_with(Fake([])), attribute=no_att)
    assert rc == 3
    text = (tmp_path / "logs" / "second-opinion-20260929.json").read_text()
    man = json.loads(text)
    assert man["judge"] == {"error": "TimeoutExpired"}
    assert "synthetic_judge.py" not in text
    # the seconds remaining before --deadline were forwarded as the timeout
    assert captured["timeout"] is not None and captured["timeout"] > 0


def test_deadline_is_forwarded_to_audit_pass(tmp_path, monkeypatch):
    """M7: the deadline must reach audit_pass, not be dropped."""
    setup(tmp_path)
    captured = {}

    def fake_audit_pass(con, backend, mem, chat, limit, deadline):
        captured["deadline"] = deadline
        return {"sampled": 0, "attempted": 0, "audited": 0, "supported": 0, "unsupported": 0,
                "unclear": 0, "unparseable": 0, "skipped_no_evidence": 0, "errors": 0,
                "stopped_at_deadline": 0}

    monkeypatch.setattr(run_nightly.audit, "audit_pass", fake_audit_pass)
    rc = run_nightly.main(args(tmp_path, "--deadline", "23:59"), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake([])), attribute=no_att)
    assert rc == 0
    assert captured["deadline"] is not None


# ---------------------------------------------------------------------------
# Fix round 1: Minors — lock (no truncate/symlink follow), --jobs/--deadline
# validation exits 2 and writes nothing, --dry-run skips the judge job.
# ---------------------------------------------------------------------------

def test_unknown_job_refuses_and_writes_nothing(tmp_path):
    setup(tmp_path)
    rc = run_nightly.main(args(tmp_path, "--jobs", "bogus"), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake([])), attribute=no_att)
    assert rc == 2
    assert not (tmp_path / "logs").exists() and not (tmp_path / "so.db").exists()


def test_malformed_deadline_refuses_and_writes_nothing(tmp_path):
    setup(tmp_path)
    rc = run_nightly.main(args(tmp_path, "--deadline", "25:00"), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake([])), attribute=no_att)
    assert rc == 2
    assert not (tmp_path / "logs").exists() and not (tmp_path / "so.db").exists()


def test_dry_run_skips_judge_job_entirely(tmp_path):
    setup(tmp_path)
    make_ab_run_dir(tmp_path)  # a real rating sheet a non-dry-run would pick up
    rc = run_nightly.main(args(tmp_path, "--jobs", "judge", "--dry-run"),
                          now_local=LOCAL_MORNING, serve=serve_with(Fake([])), attribute=no_att)
    assert rc == 0
    man = json.loads((tmp_path / "logs" / "second-opinion-20260929-dryrun.json").read_text())
    assert man["judge"] == {"skipped": "dry_run"}
    # nothing judge-shaped was written under reports-dir
    assert not (tmp_path / "reports").exists()


def test_lock_file_is_not_truncated(tmp_path):
    """Minor 1: opening the lock must never zero out an existing file."""
    setup(tmp_path)
    lock_path = tmp_path / "lock"
    lock_path.write_text("not-empty-marker")
    run_nightly.main(args(tmp_path), now_local=LOCAL_MORNING,
                     serve=serve_with(Fake(["supported\nok"])), attribute=no_att)
    assert lock_path.read_text() == "not-empty-marker"


# ---------------------------------------------------------------------------
# Final-review fix round: I1 (lane judged sheet reaches gold), I2/M11 (weekly
# reports carry the backend), I4 (no triples.json keeps reference replies),
# M1 (umask), M10 (read-only openers).
# ---------------------------------------------------------------------------

def daemon_reply_att(*a, **k):
    t0 = dt.datetime(2026, 9, 28, 12, 0, tzinfo=dt.timezone.utc)
    them = {"rowid": 1, "t": t0, "from_me": False, "text": "dinner?"}
    bot = {"rowid": 2, "t": t0 + dt.timedelta(minutes=1), "from_me": True, "text": "sure"}
    return {"timelines": {"+1a": [them, bot]}, "labeled": {"+1a": [(bot, "huuman")]}}


def test_gold_without_triples_still_writes_reference_replies(tmp_path):
    setup(tmp_path)
    make_ab_run_dir(tmp_path, with_triples=False)
    rc = run_nightly.main(args(tmp_path, "--jobs", "gold"), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake(["7pm works"])), attribute=daemon_reply_att)
    assert rc == 0
    man = json.loads((tmp_path / "logs" / "second-opinion-20260929.json").read_text())
    assert "error" not in man["gold"]
    assert man["gold"]["critiques_skipped_no_triples"] == 1 and man["gold"]["references"] == 1
    assert man["gold"]["critiques_skipped_no_run_dir"] == 0
    n = sqlite3.connect(tmp_path / "so.db").execute(
        "SELECT COUNT(*) FROM reference_replies").fetchone()[0]
    assert n == 1


def test_gold_reads_synthetic_moments_only_from_the_lanes_judged_sheet(tmp_path):
    setup(tmp_path)
    run_dir = make_ab_run_dir(tmp_path, with_triples=False)
    (run_dir / "triples.json").write_text(json.dumps(
        [{"id": "1", "context": "hi", "seth_reply": "yo", "huuman_reply": "hey"}]))
    cols = ["id", "choice", "judge_api", "judge_model"]
    # a judged sheet inside the blind-A/B run dir (unknown judge) is ignored ...
    with open(run_dir / "judged_prod.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        w.writerow({"id": "1", "choice": "A", "judge_api": "openai", "judge_model": "m"})
    rc = run_nightly.main(args(tmp_path, "--jobs", "gold"), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake([])), attribute=no_att)
    assert rc == 0
    man = json.loads((tmp_path / "logs" / "second-opinion-20260929.json").read_text())
    assert man["gold"]["attempted"] == 0
    # ... while the same row in the lane's own judged.csv is used, labelled by model.
    lane = tmp_path / "reports" / "judge-20260927"
    lane.mkdir(parents=True)
    (run_dir / "judged_prod.csv").rename(lane / "judged.csv")
    (lane / "source.json").write_text(json.dumps(run_nightly.judge.run_stamp(str(run_dir))))
    ok = '{"gaps":["tone"],"missing":"m","severity":1}'
    rc = run_nightly.main(args(tmp_path, "--jobs", "gold"), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake([ok])), attribute=no_att)
    assert rc == 0
    row = sqlite3.connect(tmp_path / "so.db").execute(
        "SELECT item_id, weak_source FROM critiques").fetchone()
    assert row == ("run1/1", "synthetic:m")


def test_weekly_reports_are_scoped_to_the_runs_backend(tmp_path):
    setup(tmp_path)
    from second_opinion import store
    s = store.open_store(str(tmp_path / "so.db"))
    now = store.now_ms()
    store.add_audit(s, 9, "wide", "unsupported", "", 0, "other@vertex", "audit-v2", now)
    store.add_critique(s, "r/x", "human", ["tone"], "", 1, False, "other@vertex",
                       "critique-v1", now)
    s.close()
    rc = run_nightly.main(args(tmp_path, "--jobs", "audit,report"), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake(["supported\nok"])), attribute=no_att)
    assert rc == 0
    rep = json.loads((tmp_path / "reports" / "audit-20260929.json").read_text())
    assert rep["backends"] == ["fake@local"] and rep["wide"]["unsupported"] == 0
    assert rep["all"]["supported"] == 1
    gold_rep = json.loads((tmp_path / "reports" / "gold-20260929.json").read_text())
    assert gold_rep["critiques"] == 0 and gold_rep["backends"] == []


def test_files_created_during_a_run_are_owner_only(tmp_path):
    # M1: plain open() inside main's flow (as a child process's judged.csv
    # would be) gets 0600 because main sets umask 077.
    setup(tmp_path)
    probe = tmp_path / "probe.txt"

    @contextlib.contextmanager
    def serve(**kw):
        with open(probe, "w") as f:
            f.write("x")
        yield Fake(["supported\nok"])

    os.umask(0o022)
    assert run_nightly.main(args(tmp_path), now_local=LOCAL_MORNING, serve=serve,
                            attribute=no_att) == 0
    assert (probe.stat().st_mode & 0o777) == 0o600


@pytest.mark.parametrize("name,maker", [("memory.db", mk_mem), ("chat.db", mk_chat)])
def test_lane_opener_is_read_only(tmp_path, name, maker):
    # M10: nothing the lane opens through _ro can be written.
    p = str(tmp_path / name)
    maker(p)
    con = run_nightly._ro(p)
    with pytest.raises(sqlite3.OperationalError):
        con.execute("CREATE TABLE x (a INTEGER)")
    with pytest.raises(sqlite3.OperationalError):
        con.execute("INSERT INTO message (text) VALUES ('x')" if name == "chat.db"
                    else "INSERT INTO messages (content) VALUES ('x')")


def test_judge_runs_before_gold_and_its_failure_does_not_stop_gold(tmp_path, monkeypatch):
    setup(tmp_path)
    make_ab_run_dir(tmp_path)
    order = []

    def fake_judge(*a, **k):
        order.append("judge")
        raise RuntimeError("judge broke")

    def fake_gold(*a, **k):
        order.append("gold")
        return {"critiques": 0, "references": 0, "unparseable": 0, "errors": 0,
                "attempted": 0, "stopped_at_deadline": 0}

    monkeypatch.setattr(run_nightly.judge, "judge_pass", fake_judge)
    monkeypatch.setattr(run_nightly.gold, "gold_pass", fake_gold)
    rc = run_nightly.main(args(tmp_path, "--jobs", "gold,judge"), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake([])), attribute=no_att)
    assert order == ["judge", "gold"]
    man = json.loads((tmp_path / "logs" / "second-opinion-20260929.json").read_text())
    assert man["judge"] == {"error": "RuntimeError"} and "error" not in man["gold"]
    assert rc == 0
