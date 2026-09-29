"""second_opinion.db: provenance on every row, dedupe per backend, private file."""
import json
import os
import stat
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import store  # noqa: E402


def test_store_file_is_private_and_schema_exists(tmp_path):
    p = tmp_path / "so.db"
    con = store.open_store(str(p))
    assert stat.S_IMODE(os.stat(p).st_mode) == 0o600
    tables = {r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    assert {"audits", "critiques", "reference_replies", "runs"} <= tables


def test_audits_dedupe_per_backend_and_carry_provenance():
    con = store.open_store(":memory:")
    assert store.add_audit(con, 7, "wide", "supported", "ok", 0, "g@local", "audit-v1", 1) == 1
    assert store.add_audit(con, 7, "wide", "unsupported", "x", 0, "g@local", "audit-v1", 2) == 0
    assert store.add_audit(con, 7, "wide", "unsupported", "x", 0, "v@vertex", "audit-v1", 3) == 1
    assert store.audited_ids(con, "g@local") == {7}
    row = con.execute("SELECT backend, prompt_version, created_at_ms FROM audits "
                      "WHERE backend='g@local'").fetchone()
    assert row == ("g@local", "audit-v1", 1)


def test_critiques_and_references_dedupe():
    con = store.open_store(":memory:")
    assert store.add_critique(con, "d0928_001", "human", ["tone"], "m", 2, 0, "g", "c1", 1) == 1
    assert store.add_critique(con, "d0928_001", "human", ["length"], "m", 2, 0, "g", "c1", 2) == 0
    assert json.loads(con.execute("SELECT gaps FROM critiques").fetchone()[0]) == ["tone"]
    assert store.critiqued_items(con, "g") == {"d0928_001"}
    assert store.add_reference(con, "+15550000001", 42, "ctx", "reply", "g", "r1", 1) == 1
    assert store.add_reference(con, "+15550000001", 42, "ctx", "reply2", "g", "r1", 2) == 0
    assert store.referenced_rowids(con, "g") == {42}
    assert con.execute("SELECT rated FROM reference_replies").fetchone()[0] is None


def test_runs_are_recorded():
    con = store.open_store(":memory:")
    rid = store.start_run(con, "g", "audit,gold", 10)
    store.finish_run(con, rid, 0, '{"audit": {}}', 20)
    assert con.execute("SELECT exit_code, finished_at_ms FROM runs WHERE id=?", (rid,)).fetchone() == (0, 20)
