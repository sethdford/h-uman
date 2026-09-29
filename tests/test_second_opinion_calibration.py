"""Turn Gemma's disagreement rate into a real wrong-accept estimate (spec §4.1)."""
import csv
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import audit_score, audit_sheet, stats, store  # noqa: E402


def dbs(n):
    m = sqlite3.connect(":memory:")
    m.executescript("CREATE TABLE messages (id INTEGER PRIMARY KEY, session_id TEXT, role TEXT,"
                    " content TEXT, created_at TEXT);"
                    "CREATE TABLE contact_insights (id INTEGER PRIMARY KEY, contact_id TEXT,"
                    " insight TEXT, evidence_ids TEXT, retired_at_ms INTEGER DEFAULT 0,"
                    " created_at_ms INTEGER DEFAULT 0, source TEXT);")
    for i in range(n):
        m.execute("INSERT INTO messages VALUES (?, 'c', 'user', ?, '')", (i, f"msg {i}"))
        m.execute("INSERT INTO contact_insights (id, contact_id, insight, evidence_ids)"
                  " VALUES (?, 'c', ?, ?)", (i, f"note {i}", json.dumps([i])))
    return m


def test_sheet_refuses_until_enough_audits_then_hides_the_verdict(tmp_path):
    m, s = dbs(40), store.open_store(":memory:")
    out, key = tmp_path / "check.csv", tmp_path / "key.json"
    assert audit_sheet.write_check_sheet(s, m, None, str(out), str(key)) == 2
    assert not out.exists() and not key.exists()
    for i in range(25):
        store.add_audit(s, i, "persona", "unsupported", "", 0, "g", "audit-v1", 1)
    for i in range(25, 40):
        store.add_audit(s, i, "persona", "supported", "", 0, "g", "audit-v1", 1)
    assert audit_sheet.write_check_sheet(s, m, None, str(out), str(key)) == 0
    rows = list(csv.DictReader(open(out)))
    assert len(rows) == 30 and set(rows[0]) == {"row", "note", "cited_messages", "supported"}
    assert all(r["supported"] == "" for r in rows)
    assert "unsupported" not in out.read_text()
    k = json.loads(key.read_text())
    assert sum(1 for v in k.values() if v["gemma"] == "unsupported") == 20


def write_labeled(tmp_path, labels):
    """labels: list of (gemma_verdict, human_says_supported 'y'|'n'|'')."""
    sheet, key = tmp_path / "c.csv", tmp_path / "k.json"
    with open(sheet, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["row", "note", "cited_messages", "supported"])
        w.writeheader()
        for i, (_, lab) in enumerate(labels):
            w.writerow({"row": str(i), "note": "n", "cited_messages": "m", "supported": lab})
    key.write_text(json.dumps({str(i): {"insight_id": i, "gemma": g}
                               for i, (g, _) in enumerate(labels)}))
    return str(sheet), str(key)


def test_score_combines_disagreement_with_precision(tmp_path):
    labels = ([("unsupported", "n")] * 15 + [("unsupported", "y")] * 5 +
              [("supported", "y")] * 9 + [("supported", "n")] * 1)
    sheet, key = write_labeled(tmp_path, labels)
    r = audit_score.score_check(sheet, key, disagreement_rate=0.2)
    assert r["ppv"] == 0.75 and r["false_omission"] == 0.1
    assert abs(r["estimated_wrong_accept_rate"] - (0.2 * 0.75 + 0.8 * 0.1)) < 1e-9
    lo, hi = r["ci95"]
    assert lo < r["estimated_wrong_accept_rate"] < hi and r["n_labeled"] == 30


def test_score_is_not_measured_until_every_row_is_labeled(tmp_path):
    sheet, key = write_labeled(tmp_path, [("unsupported", "n"), ("supported", "")])
    assert audit_score.score_check(sheet, key, disagreement_rate=0.2) == stats.NOT_MEASURED
