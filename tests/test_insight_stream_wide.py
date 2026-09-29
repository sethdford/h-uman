"""--population wide: human-only evidence, curator_wide source, counts-only manifest."""
import datetime as dt
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
import insight_stream as ins  # noqa: E402

NOW = dt.datetime(2026, 9, 28, 12, 0, tzinfo=dt.timezone.utc)
H = "+15550000042"


def timeline():
    out = []
    for i in range(12):
        out.append({"rowid": 100 + i, "guid": f"them{i}", "from_me": False,
                    "text": "priya's surgery is tuesday" if i == 0 else f"them {i}",
                    "t": NOW - dt.timedelta(hours=40 - i), "atype": 0})
    for i in range(6):
        out.append({"rowid": 200 + i, "guid": f"me{i}", "from_me": True, "text": f"me {i}",
                    "t": NOW - dt.timedelta(hours=20 - i), "atype": 0})
    out.append({"rowid": 300, "guid": "bot", "from_me": True, "text": "Marcus says hi",
                "t": NOW - dt.timedelta(hours=1), "atype": 0})
    return out


class A:
    consistency_k = 1; turns = 80; min_turns = 10; max_notes = 8
    url = "http://127.0.0.1:1/x"; model = "m"
    never_path = "/nonexistent/curator_never.json"  # hermetic: never read the real file


import pytest  # noqa: E402


@pytest.fixture(autouse=True)
def _hermetic_memory_db(tmp_path, monkeypatch):
    """wide_pass reads suppressions from MEMORY_DB; never touch the real ~/.human.

    load_suppressed fails closed (see curator_population.load_suppressed):
    only a missing TABLE means "nobody opted out" -- a missing FILE raises
    sqlite3.OperationalError("unable to open database file"), since the
    read-only URI connect refuses to create it. So the hermetic redirect
    must point at a real, existing (if empty) sqlite file, not a bare path
    in an empty tmp_path -- a bare path here would make wide_pass refuse
    every run with the "unreadable DB" error instead of "no suppressions".
    """
    mem_db = tmp_path / "suppressions.db"
    sqlite3.connect(mem_db).close()
    monkeypatch.setattr(ins, "MEMORY_DB", str(mem_db))
    # run_wide also reads ~/.human/config.json (loopback handle) and writes
    # ~/.human/curator_state.json: redirect both so no test touches them.
    monkeypatch.setattr(ins, "HUMAN_CONFIG", str(tmp_path / "absent-config.json"), raising=False)
    monkeypatch.setattr(ins, "CURATOR_STATE", str(tmp_path / "curator_state.json"))


def test_wide_pass_writes_only_supported_named_notes(tmp_path, monkeypatch):
    db = sqlite3.connect(tmp_path / "m.db"); db.executescript(ins.SCHEMA); ins.migrate(db)
    att = {"timelines": {H: timeline()},
           "labels": {**{f"me{i}": "seth" for i in range(6)}, "bot": "huuman"}}
    # the model proposes: one good note, one citing the daemon row, one with an invented name
    raw = json.dumps([
        {"note": "Priya surgery tuesday", "kind": "plan", "confidence": 0.9,
         "evidence": ["t0"], "names": [{"name": "Priya", "type": "person"}]},
        {"note": "Marcus says hi", "kind": "fact", "confidence": 0.9,
         "evidence": ["d0"], "names": [{"name": "Marcus", "type": "person"}]},
        {"note": "Dana visiting", "kind": "fact", "confidence": 0.9,
         "evidence": ["t1"], "names": [{"name": "Dana", "type": "person"}]}])
    monkeypatch.setattr(ins, "call_model", lambda *a, **k: raw)
    monkeypatch.setattr(ins, "verify_claims", lambda a, s, u, claims, k: [1] * len(claims))
    man = ins.wide_pass(db, A(), "id", set(), att, NOW, write=True)
    rows = db.execute("SELECT insight, source, evidence_ids FROM contact_insights").fetchall()
    assert rows == [("Priya surgery tuesday", "curator_wide:" + ins.source_tag(1, 1),
                     json.dumps(["chat:100"]))]
    assert man["eligible"] == 1 and man["notes_written"] == 1
    assert man["rejected_daemon_evidence"] == 1 and man["rejected_name_not_said"] == 1
    assert "Priya" not in json.dumps(man)  # counts only, never text
    assert H not in json.dumps(man)  # ...and never a handle


def test_wide_pass_dry_run_writes_nothing(tmp_path, monkeypatch):
    db = sqlite3.connect(tmp_path / "m.db"); db.executescript(ins.SCHEMA); ins.migrate(db)
    att = {"timelines": {H: timeline()}, "labels": {f"me{i}": "seth" for i in range(6)}}
    monkeypatch.setattr(ins, "call_model", lambda *a, **k: "[]")
    monkeypatch.setattr(ins, "verify_claims", lambda a, s, u, claims, k: [])
    ins.wide_pass(db, A(), "id", set(), att, NOW, write=False)
    assert db.execute("SELECT COUNT(*) FROM contact_insights").fetchone()[0] == 0


def test_suppressed_contact_rows_are_retired_whatever_the_source(tmp_path, monkeypatch):
    db = sqlite3.connect(tmp_path / "m.db"); db.executescript(ins.SCHEMA); ins.migrate(db)
    db.execute("INSERT INTO contact_insights (contact_id, kind, insight, confidence, as_of_ms,"
               " source, created_at_ms) VALUES (?, 'fact', 'old note', 0.9, 1, 'extractor:v2', 1)",
               (H,))
    db.commit()
    n = ins.retire_suppressed(db, {H}, now_ms=5, write=True)
    assert n == 1
    assert db.execute("SELECT retired_at_ms FROM contact_insights").fetchone()[0] == 5


def test_deadline_stops_before_the_next_contact_and_order_prefers_stale(tmp_path, monkeypatch):
    state = {"+1a": 300, "+1b": 100, "+1c": 200}
    assert ins.order_by_last_run(["+1a", "+1b", "+1c", "+1d"], state) == ["+1d", "+1b", "+1c", "+1a"]
    db = sqlite3.connect(tmp_path / "m.db"); db.executescript(ins.SCHEMA); ins.migrate(db)
    att = {"timelines": {H: timeline()}, "labels": {f"me{i}": "seth" for i in range(6)}}
    monkeypatch.setattr(ins, "call_model", lambda *a, **k: (_ for _ in ()).throw(AssertionError))
    man = ins.wide_pass(db, A(), "id", set(), att, NOW, write=True,
                        deadline=NOW - dt.timedelta(minutes=1), state={})
    assert man["curated"] == 0 and man["stopped_at_deadline"] == 1


def test_manifest_refuses_when_nothing_is_eligible(tmp_path):
    # a dedicated subdir: tmp_path itself already holds the autouse fixture's
    # hermetic suppressions.db, which would make the "wrote nothing" assertion
    # below see a stray file that has nothing to do with write_manifest.
    manifest_dir = tmp_path / "manifest"
    assert ins.write_manifest(str(manifest_dir), NOW, {"eligible": 0}) == 2
    assert not manifest_dir.exists()
    assert ins.write_manifest(str(manifest_dir), NOW, {"eligible": 3, "notes_written": 1}) == 0
    assert json.loads((manifest_dir / "curator-manifest-20260928.json").read_text())["eligible"] == 3


def test_wide_pass_uses_the_same_window_for_eligibility_and_evidence(tmp_path, monkeypatch):
    """R9a: cp.eligible_handles must see the same window_days used for the
    evidence cutoff -- pin it by making the two windows disagree and
    confirming the CLI's --window-days value (not the eligible_handles
    default) is what decides eligibility."""
    db = sqlite3.connect(tmp_path / "m.db"); db.executescript(ins.SCHEMA); ins.migrate(db)
    seen = {}
    real_eligible = ins.cp.eligible_handles

    def spy(timelines, persona_ids, now, **kw):
        seen["window_days"] = kw.get("window_days")
        return real_eligible(timelines, persona_ids, now, **kw)
    monkeypatch.setattr(ins.cp, "eligible_handles", spy)
    att = {"timelines": {H: timeline()}, "labels": {f"me{i}": "seth" for i in range(6)}}
    monkeypatch.setattr(ins, "call_model", lambda *a, **k: (_ for _ in ()).throw(AssertionError))
    a = A(); a.window_days = 7
    ins.wide_pass(db, a, "id", set(), att, NOW, write=False,
                  deadline=NOW - dt.timedelta(minutes=1), state={})
    assert seen["window_days"] == 7


def test_run_wide_refuses_before_writing_when_memory_db_unreadable(tmp_path, monkeypatch):
    """R5: fail closed. A sqlite3.Error reading memory.db (here, the
    suppressions read inside run_wide) must refuse with exit 2 and write no
    manifest and no state file -- before wide_pass or write_manifest ever
    run. chat.db/model/never-file checks are monkeypatched so only the
    memory.db failure is under test."""
    db = sqlite3.connect(tmp_path / "m.db"); db.executescript(ins.SCHEMA); ins.migrate(db)
    chat_db = tmp_path / "chat.db"
    c = sqlite3.connect(chat_db)
    c.execute("CREATE TABLE message (rowid INTEGER)")
    c.commit(); c.close()
    monkeypatch.setattr(ins.urllib.request, "urlopen", lambda *a, **k: None)
    import eval_conversation_quality as cq
    monkeypatch.setattr(cq, "attribute", lambda *a, **k: {
        "timelines": {H: timeline()}, "labels": {f"me{i}": "seth" for i in range(6)}})
    monkeypatch.setattr(ins.cp, "load_suppressed",
                        lambda *a, **k: (_ for _ in ()).throw(sqlite3.OperationalError("locked")))
    monkeypatch.setattr(ins, "call_model", lambda *a, **k: (_ for _ in ()).throw(AssertionError))
    state_path = tmp_path / "curator_state.json"
    monkeypatch.setattr(ins, "CURATOR_STATE", str(state_path))
    manifest_dir = tmp_path / "manifests"
    a = A()
    a.chat_db = str(chat_db); a.window_days = 30; a.deadline = None
    a.manifest_dir = str(manifest_dir); a.write = False
    rc = ins.run_wide(a, db, "id", set(), 123)
    assert rc == 2
    assert not manifest_dir.exists()
    assert not state_path.exists()


def test_run_wide_excludes_suppressed_and_never_without_calling_model(tmp_path, monkeypatch):
    """R9b end-to-end: one eligible contact opted out via contact_suppressions,
    one eligible contact opted out via the --never-path file -> the manifest
    reports both exclusions, curated == 0, and call_model is never invoked."""
    db = sqlite3.connect(tmp_path / "m.db"); db.executescript(ins.SCHEMA); ins.migrate(db)
    chat_db = tmp_path / "chat.db"
    c = sqlite3.connect(chat_db)
    c.execute("CREATE TABLE message (rowid INTEGER)")
    c.commit(); c.close()
    monkeypatch.setattr(ins.urllib.request, "urlopen", lambda *a, **k: None)
    H2 = "+15550000099"
    import eval_conversation_quality as cq
    monkeypatch.setattr(cq, "attribute", lambda *a, **k: {
        "timelines": {H: timeline(), H2: timeline()},
        "labels": {f"me{i}": "seth" for i in range(6)}})
    mem = sqlite3.connect(ins.MEMORY_DB)  # the hermetic tmp file from the autouse fixture
    mem.execute("CREATE TABLE contact_suppressions (contact TEXT)")
    mem.execute("INSERT INTO contact_suppressions VALUES (?)", (H,))
    mem.commit(); mem.close()
    never_path = tmp_path / "never.json"
    never_path.write_text(json.dumps([H2]))
    monkeypatch.setattr(ins, "call_model", lambda *a, **k: (_ for _ in ()).throw(AssertionError))
    state_path = tmp_path / "curator_state.json"
    monkeypatch.setattr(ins, "CURATOR_STATE", str(state_path))
    manifest_dir = tmp_path / "manifests"
    a = A()
    a.chat_db = str(chat_db); a.window_days = 30; a.deadline = None
    a.manifest_dir = str(manifest_dir); a.write = False; a.never_path = str(never_path)
    rc = ins.run_wide(a, db, "id", set(), 123)
    assert rc == 0
    # run_wide uses the real wall clock for "now" (not the fixed NOW/H fixtures
    # above), so find today's manifest rather than assuming its date stamp.
    manifest_files = list(manifest_dir.glob("curator-manifest-*.json"))
    assert len(manifest_files) == 1
    man = json.loads(manifest_files[0].read_text())
    assert man["excluded_suppressed"] == 1
    assert man["excluded_never"] == 1
    assert man["curated"] == 0


def test_run_wide_refuses_before_any_write_when_no_eligible_contacts(tmp_path, monkeypatch):
    """Fix round 1 (a): with --write and 0 eligible contacts, run_wide must
    refuse BEFORE retire_suppressed, wide_pass, the state write, or the
    manifest -- not after. Set up a suppressed contact with a live
    contact_insights row and empty chat.db timelines (0 eligible); assert
    the row is untouched, no state file, no manifest."""
    db = sqlite3.connect(tmp_path / "m.db"); db.executescript(ins.SCHEMA); ins.migrate(db)
    db.execute("INSERT INTO contact_insights (contact_id, kind, insight, confidence, as_of_ms,"
               " source, created_at_ms) VALUES (?, 'fact', 'old note', 0.9, 1, 'extractor:v2', 1)",
               (H,))
    db.commit()
    chat_db = tmp_path / "chat.db"
    c = sqlite3.connect(chat_db)
    c.execute("CREATE TABLE message (rowid INTEGER)")
    c.commit(); c.close()
    monkeypatch.setattr(ins.urllib.request, "urlopen", lambda *a, **k: None)
    import eval_conversation_quality as cq
    monkeypatch.setattr(cq, "attribute", lambda *a, **k: {"timelines": {}, "labels": {}})
    mem = sqlite3.connect(ins.MEMORY_DB)  # the hermetic tmp file from the autouse fixture
    mem.execute("CREATE TABLE contact_suppressions (contact TEXT)")
    mem.execute("INSERT INTO contact_suppressions VALUES (?)", (H,))
    mem.commit(); mem.close()
    monkeypatch.setattr(ins, "call_model", lambda *a, **k: (_ for _ in ()).throw(AssertionError))
    state_path = tmp_path / "curator_state.json"
    monkeypatch.setattr(ins, "CURATOR_STATE", str(state_path))
    manifest_dir = tmp_path / "manifests"
    a = A()
    a.chat_db = str(chat_db); a.window_days = 30; a.deadline = None
    a.manifest_dir = str(manifest_dir); a.write = True
    rc = ins.run_wide(a, db, "id", set(), 999)
    assert rc == 2
    assert db.execute("SELECT retired_at_ms FROM contact_insights").fetchone()[0] == 0
    assert not state_path.exists()
    assert not manifest_dir.exists()


def test_resolve_deadline_today_closed_or_tomorrow():
    """R12 I5 (overrides R10b): today's HH:MM while it is still ahead; if it
    passed less than 12h ago the window is CLOSED (None) -- launchd runs a
    missed 05:10 job on wake, and a 22h run would load :8741 all day; only
    once it passed 12h+ ago does the run target tomorrow's HH:MM."""
    tz = dt.timezone(dt.timedelta(hours=-4))
    morning = dt.datetime(2026, 9, 28, 5, 10, tzinfo=tz)
    assert ins.resolve_deadline("07:30", morning) == (
        dt.datetime(2026, 9, 28, 7, 30, tzinfo=tz).astimezone(dt.timezone.utc))
    assert ins.resolve_deadline("07:30", dt.datetime(2026, 9, 28, 9, 0, tzinfo=tz)) is None
    assert ins.resolve_deadline("07:30", dt.datetime(2026, 9, 28, 19, 29, tzinfo=tz)) is None
    tomorrow = dt.datetime(2026, 9, 29, 7, 30, tzinfo=tz).astimezone(dt.timezone.utc)
    assert ins.resolve_deadline("07:30", dt.datetime(2026, 9, 28, 19, 30, tzinfo=tz)) == tomorrow
    assert ins.resolve_deadline("07:30", dt.datetime(2026, 9, 28, 23, 0, tzinfo=tz)) == tomorrow


def test_run_wide_recovers_from_a_truncated_curator_state_file(tmp_path, monkeypatch):
    """Fix round 1 (c): a truncated/corrupt curator_state.json must not crash
    the run -- load_curator_state warns and starts from {}, and the atomic
    save (tmp + os.replace) leaves the file valid JSON afterward."""
    db = sqlite3.connect(tmp_path / "m.db"); db.executescript(ins.SCHEMA); ins.migrate(db)
    chat_db = tmp_path / "chat.db"
    c = sqlite3.connect(chat_db)
    c.execute("CREATE TABLE message (rowid INTEGER)")
    c.commit(); c.close()
    monkeypatch.setattr(ins.urllib.request, "urlopen", lambda *a, **k: None)
    import eval_conversation_quality as cq
    monkeypatch.setattr(cq, "attribute", lambda *a, **k: {
        "timelines": {H: timeline()}, "labels": {f"me{i}": "seth" for i in range(6)}})
    monkeypatch.setattr(ins, "call_model", lambda *a, **k: "[]")
    monkeypatch.setattr(ins, "verify_claims", lambda a, s, u, claims, k: [])
    state_path = tmp_path / "curator_state.json"
    state_path.write_text('{"+1a": 3')  # truncated -- not valid JSON
    monkeypatch.setattr(ins, "CURATOR_STATE", str(state_path))
    manifest_dir = tmp_path / "manifests"
    a = A()
    a.chat_db = str(chat_db); a.window_days = 30; a.deadline = None
    a.manifest_dir = str(manifest_dir); a.write = True
    a.never_path = "/nonexistent/curator_never.json"
    rc = ins.run_wide(a, db, "id", set(), 123)  # must not raise
    assert rc == 0
    state = json.loads(state_path.read_text())  # must be valid JSON afterward
    assert isinstance(state, dict) and H in state and isinstance(state[H], int)
