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


def test_wide_pass_dry_run_writes_nothing(tmp_path, monkeypatch):
    db = sqlite3.connect(tmp_path / "m.db"); db.executescript(ins.SCHEMA); ins.migrate(db)
    att = {"timelines": {H: timeline()}, "labels": {f"me{i}": "seth" for i in range(6)}}
    monkeypatch.setattr(ins, "call_model", lambda *a, **k: "[]")
    monkeypatch.setattr(ins, "verify_claims", lambda a, s, u, claims, k: [])
    ins.wide_pass(db, A(), "id", set(), att, NOW, write=False)
    assert db.execute("SELECT COUNT(*) FROM contact_insights").fetchone()[0] == 0
