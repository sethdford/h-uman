"""Verifier audit (spec §4.1): only cited evidence, never guessed verdicts."""
import datetime as dt
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import audit, stats, store  # noqa: E402


def mem_db():
    m = sqlite3.connect(":memory:")
    m.executescript("""
      CREATE TABLE messages (id INTEGER PRIMARY KEY, session_id TEXT, role TEXT, content TEXT,
                             created_at TEXT);
      CREATE TABLE contact_insights (id INTEGER PRIMARY KEY AUTOINCREMENT, contact_id TEXT,
        kind TEXT, insight TEXT, confidence REAL, as_of_ms INTEGER, source TEXT,
        created_at_ms INTEGER, retired_at_ms INTEGER DEFAULT 0, evidence_ids TEXT,
        superseded_by_id INTEGER DEFAULT 0);
      INSERT INTO messages VALUES (1, '+1a', 'user', 'priya surgery is tuesday', '');
      INSERT INTO contact_insights (contact_id, insight, source, created_at_ms, evidence_ids)
        VALUES ('+1a', 'Priya surgery tuesday', 'extractor:v2:k3:a3', 3, '[1]'),
               ('+1b', 'Dana moving to Austin', 'curator_wide:extractor:v2:k3:a3', 2, '["chat:7"]'),
               ('+1c', 'no evidence note', 'extractor:v1', 1, NULL),
               ('+1d', 'dangling', 'extractor:v1', 0, '[999]'),
               ('+1e', 'retired', 'extractor:v1', 5, '[1]');
      UPDATE contact_insights SET retired_at_ms = 9 WHERE insight = 'retired';
    """)
    return m


def chat_db():
    c = sqlite3.connect(":memory:")
    c.executescript("CREATE TABLE message (ROWID INTEGER PRIMARY KEY, text TEXT,"
                    " attributedBody BLOB);"
                    "INSERT INTO message VALUES (7, 'dana is moving to austin in may', NULL);")
    return c


class Fake:
    name = "fake@local"

    def __init__(self, outputs):
        self.outputs = list(outputs)
        self.calls = 0

    def generate(self, system, user, max_tokens=400):
        self.calls += 1
        out = self.outputs.pop(0)
        if isinstance(out, Exception):
            raise out
        return out


def test_resolve_evidence_persona_wide_and_unresolvable():
    m, c = mem_db(), chat_db()
    assert audit.resolve_evidence("[1]", m, c) == ["priya surgery is tuesday"]
    assert audit.resolve_evidence('["chat:7"]', m, c) == ["dana is moving to austin in may"]
    for bad in (None, "", "not json", '{"a": 1}', "[999]", '["chat:x"]', '["chat:8"]'):
        assert audit.resolve_evidence(bad, m, c) == [], bad
    assert audit.resolve_evidence('["chat:7"]', m, None) == []


def test_parse_verdict_never_guesses():
    assert audit.parse_verdict("Supported.\nthey say so") == ("supported", "they say so", False)
    assert audit.parse_verdict("**unsupported**") == ("unsupported", "", False)
    assert audit.parse_verdict("Sure! Here is my analysis: supported")[0] == "unclear"
    assert audit.parse_verdict("Sure! Here is my analysis")[2] is True
    assert audit.parse_verdict("") == ("unclear", "", True)


def test_audit_pass_counts_skips_and_dedupes():
    m, c, s = mem_db(), chat_db(), store.open_store(":memory:")
    b = Fake(["supported\nok", "unsupported\nno mention of Austin"])
    counts = audit.audit_pass(s, b, m, c, limit=10)
    assert counts["sampled"] == 4 and counts["skipped_no_evidence"] == 2
    assert counts["audited"] == 2 and counts["supported"] == 1 and counts["unsupported"] == 1
    assert b.calls == 2
    rows = dict(s.execute("SELECT source, verdict FROM audits WHERE verdict != 'no_evidence'")
                .fetchall())
    assert rows == {"persona": "supported", "wide": "unsupported"}
    again = audit.audit_pass(s, Fake([]), m, c, limit=10)  # second night: nothing new to judge
    assert again["sampled"] == 0 and again["audited"] == 0  # unresolvable ones recorded once
    assert audit.audit_report(s)["all"]["disagreement"]["n"] == 2  # no_evidence is not a verdict


def test_audit_pass_counts_errors_and_unparseable():
    m, c, s = mem_db(), chat_db(), store.open_store(":memory:")
    counts = audit.audit_pass(s, Fake([TimeoutError("slow"), "I think maybe?"]), m, c, limit=10)
    assert counts["errors"] == 1 and counts["unparseable"] == 1 and counts["unclear"] == 1
    assert counts["attempted"] == 2


def test_audit_pass_stops_at_deadline():
    m, c, s = mem_db(), chat_db(), store.open_store(":memory:")
    now = dt.datetime(2026, 9, 29, 8, 0, tzinfo=dt.timezone.utc)
    counts = audit.audit_pass(s, Fake([]), m, c, limit=10, deadline=now, now=lambda: now)
    assert counts["stopped_at_deadline"] == 1 and counts["audited"] == 0


def test_audit_report_disagreement_by_source_and_not_measured():
    s = store.open_store(":memory:")
    assert audit.audit_report(s)["all"]["disagreement"] == stats.NOT_MEASURED
    for i, (src, v) in enumerate([("wide", "unsupported"), ("wide", "supported"),
                                  ("persona", "supported"), ("persona", "unclear")]):
        store.add_audit(s, i, src, v, "", 0, "g", "audit-v1", 10)
    rep = audit.audit_report(s)
    assert rep["wide"]["disagreement"]["rate"] == 0.5
    assert rep["persona"]["disagreement"]["rate"] == 0.0 and rep["persona"]["unclear"] == 1
    assert rep["all"]["disagreement"]["n"] == 3


def test_a_broken_database_read_is_unresolvable_not_a_crash():
    m = mem_db()
    m.execute("DROP TABLE messages")          # schema drift / corrupt table mid-pass
    assert audit.resolve_evidence("[1]", m, chat_db()) == []
    s = store.open_store(":memory:")
    counts = audit.audit_pass(s, Fake(["unsupported\nx"]), m, chat_db(), limit=10)
    assert counts["skipped_no_evidence"] == 3 and counts["audited"] == 1


def test_audit_report_filters_to_one_backend_and_names_its_provenance():
    # I2: a Vertex night must never mix into the Gemma number.
    s = store.open_store(":memory:")
    store.add_audit(s, 1, "wide", "unsupported", "", 0, "gemma@local", "audit-v1", 10)
    store.add_audit(s, 2, "wide", "supported", "", 0, "gemma@local", "audit-v1", 10)
    store.add_audit(s, 1, "wide", "supported", "", 0, "gem@vertex", "audit-v2", 10)
    store.add_audit(s, 3, "wide", "supported", "", 0, "gem@vertex", "audit-v2", 10)
    pooled = audit.audit_report(s)
    assert pooled["wide"]["disagreement"]["rate"] == 0.25
    assert pooled["backends"] == ["gem@vertex", "gemma@local"]
    local = audit.audit_report(s, backend="gemma@local")
    assert local["wide"]["disagreement"]["rate"] == 0.5
    assert local["backends"] == ["gemma@local"] and local["prompt_versions"] == ["audit-v1"]
    nobody = audit.audit_report(s, backend="nobody")
    assert nobody["all"]["disagreement"] == stats.NOT_MEASURED
    # An empty filtered report still says what it was filtered to.
    assert nobody["backends"] == [] and nobody["backend_filter"] == "nobody"
    assert pooled["backend_filter"] is None
