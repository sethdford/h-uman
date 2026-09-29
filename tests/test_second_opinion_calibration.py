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
    assert sum(1 for kk, v in k.items() if kk != "_meta" and v["gemma"] == "unsupported") == 20
    assert k["_meta"] == {"backend": None, "source": "all", "prompt_versions": ["audit-v1"]}


def test_write_check_sheet_escapes_formula_prefixed_notes_and_cited_messages(tmp_path):
    m = sqlite3.connect(":memory:")
    m.executescript("CREATE TABLE messages (id INTEGER PRIMARY KEY, session_id TEXT, role TEXT,"
                    " content TEXT, created_at TEXT);"
                    "CREATE TABLE contact_insights (id INTEGER PRIMARY KEY, contact_id TEXT,"
                    " insight TEXT, evidence_ids TEXT, retired_at_ms INTEGER DEFAULT 0,"
                    " created_at_ms INTEGER DEFAULT 0, source TEXT);")
    for i in range(30):
        text = "=cmd|'/bin/calc'!A0" if i == 0 else f"msg {i}"
        m.execute("INSERT INTO messages VALUES (?, 'c', 'user', ?, '')", (i, text))
        note = "=HYPERLINK(\"http://evil\")" if i == 0 else f"note {i}"
        m.execute("INSERT INTO contact_insights (id, contact_id, insight, evidence_ids)"
                  " VALUES (?, 'c', ?, ?)", (i, note, json.dumps([i])))
    s = store.open_store(":memory:")
    out, key = tmp_path / "check.csv", tmp_path / "key.json"
    # Exactly n_unsupported (20) + n_supported (10) audited ids so the resolvable
    # pool == the pick count and both id 0's rows are guaranteed to appear.
    for i in range(20):
        store.add_audit(s, i, "persona", "unsupported", "", 0, "g", "audit-v1", 1)
    for i in range(20, 30):
        store.add_audit(s, i, "persona", "supported", "", 0, "g", "audit-v1", 1)
    assert audit_sheet.write_check_sheet(s, m, None, str(out), str(key)) == 0
    rows = list(csv.DictReader(open(out)))
    k = json.loads(key.read_text())
    target = next(r for r in rows if k[r["row"]]["insight_id"] == 0)
    assert target["note"] == "'=HYPERLINK(\"http://evil\")"
    assert target["cited_messages"] == "'=cmd|'/bin/calc'!A0"
    other = next(r for r in rows if k[r["row"]]["insight_id"] == 1)
    assert other["note"] == "note 1"


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


def test_score_labels_are_only_y_or_n_words(tmp_path):
    # A fully-filled sheet with one non-y/n label ("not sure") is still unlabeled.
    labels = ([("unsupported", "n")] * 14 + [("unsupported", "not sure")] +
              [("unsupported", "y")] * 5 + [("supported", "y")] * 9 + [("supported", "n")] * 1)
    sheet, key = write_labeled(tmp_path, labels)
    assert audit_score.score_check(sheet, key, disagreement_rate=0.2) == stats.NOT_MEASURED


def test_score_accepts_yes_no_words_case_insensitively(tmp_path):
    labels = ([("unsupported", "No")] * 15 + [("unsupported", "YES")] * 5 +
              [("supported", "Yes")] * 9 + [("supported", "no")] * 1)
    sheet, key = write_labeled(tmp_path, labels)
    r = audit_score.score_check(sheet, key, disagreement_rate=0.2)
    assert r != stats.NOT_MEASURED
    assert r["ppv"] == 0.75 and r["false_omission"] == 0.1 and r["n_labeled"] == 30


def test_ci_interval_uses_bonferroni_for_joint_95_coverage(tmp_path):
    labels = ([("unsupported", "n")] * 15 + [("unsupported", "y")] * 5 +
              [("supported", "y")] * 9 + [("supported", "n")] * 1)
    sheet, key = write_labeled(tmp_path, labels)
    r = audit_score.score_check(sheet, key, disagreement_rate=0.2)
    lo, hi = r["ci95"]
    d = 0.2
    p_lo95, p_hi95 = stats.wilson(15, 20, z=1.96)
    f_lo95, f_hi95 = stats.wilson(1, 10, z=1.96)
    lo95 = round(d * p_lo95 + (1 - d) * f_lo95, 4)
    hi95 = round(d * p_hi95 + (1 - d) * f_hi95, 4)
    assert (hi - lo) > (hi95 - lo95)
    assert lo < r["estimated_wrong_accept_rate"] < hi


def test_sheet_skips_deleted_or_unresolvable_notes_and_backfills(tmp_path):
    m, s = dbs(40), store.open_store(":memory:")
    out, key = tmp_path / "check.csv", tmp_path / "key.json"
    for i in range(25):
        store.add_audit(s, i, "persona", "unsupported", "", 0, "g", "audit-v1", 1)
    for i in range(25, 40):
        store.add_audit(s, i, "persona", "supported", "", 0, "g", "audit-v1", 1)
    m.execute("DELETE FROM contact_insights WHERE id IN (0, 1, 2)")
    m.commit()
    assert audit_sheet.write_check_sheet(s, m, None, str(out), str(key)) == 0
    rows = list(csv.DictReader(open(out)))
    k = json.loads(key.read_text())
    uns_rows = [k[r["row"]] for r in rows if k[r["row"]]["gemma"] == "unsupported"]
    assert len(uns_rows) == 20
    assert {v["insight_id"] for v in uns_rows}.isdisjoint({0, 1, 2})
    assert all(r["note"] for r in rows)


def test_sheet_refuses_when_resolvable_pool_too_small(tmp_path):
    m, s = dbs(40), store.open_store(":memory:")
    out, key = tmp_path / "check.csv", tmp_path / "key.json"
    for i in range(21):
        store.add_audit(s, i, "persona", "unsupported", "", 0, "g", "audit-v1", 1)
    for i in range(25, 35):
        store.add_audit(s, i, "persona", "supported", "", 0, "g", "audit-v1", 1)
    m.execute("DELETE FROM contact_insights WHERE id IN (0, 1, 2)")
    m.commit()
    assert audit_sheet.write_check_sheet(s, m, None, str(out), str(key)) == 2
    assert not out.exists() and not key.exists()


# ---------------------------------------------------------------------------
# Final-review fix round: I2 (provenance filters), M2 (key path), M8 (d known).
# ---------------------------------------------------------------------------

GEMMA = "gemma-4-31b-it-4bit@local"
VERTEX = "gemini-3.8-flash@vertex"


def seed_two_backends(s):
    """Gemma: 20 wide unsupported (0-19), 10 wide supported (20-29), 20 persona
    unsupported (40-59), 10 persona supported (60-69). Vertex disagrees on the
    SAME wide insights: calls 0-9 supported and 20-29 unsupported."""
    for i in range(20):
        store.add_audit(s, i, "wide", "unsupported", "", 0, GEMMA, "audit-v1", 1)
    for i in range(20, 30):
        store.add_audit(s, i, "wide", "supported", "", 0, GEMMA, "audit-v1", 1)
    for i in range(40, 60):
        store.add_audit(s, i, "persona", "unsupported", "", 0, GEMMA, "audit-v1", 1)
    for i in range(60, 70):
        store.add_audit(s, i, "persona", "supported", "", 0, GEMMA, "audit-v1", 1)
    for i in range(10):
        store.add_audit(s, i, "wide", "supported", "", 0, VERTEX, "audit-v2", 1)
    for i in range(20, 30):
        store.add_audit(s, i, "wide", "unsupported", "", 0, VERTEX, "audit-v2", 1)


def sheet_ids(out, key):
    k = json.loads(key.read_text())
    return [k[r["row"]]["insight_id"] for r in csv.DictReader(open(out))], k


def test_sheet_filters_to_one_backend_and_source(tmp_path):
    m, s = dbs(70), store.open_store(":memory:")
    seed_two_backends(s)
    out, key = tmp_path / "check.csv", tmp_path / "check.key.json"
    assert audit_sheet.write_check_sheet(s, m, None, str(out), str(key), backend=GEMMA,
                                         source="wide") == 0
    ids, k = sheet_ids(out, key)
    assert sorted(ids) == list(range(30))               # wide only: no persona 40-69
    assert all(k[str(n)]["gemma"] == ("unsupported" if iid < 20 else "supported")
               for n, iid in enumerate(ids))           # Gemma's verdicts, never Vertex's
    assert k["_meta"] == {"backend": GEMMA, "source": "wide", "prompt_versions": ["audit-v1"]}
    assert audit_sheet.write_check_sheet(s, m, None, str(out), str(key), backend=GEMMA,
                                         source="persona") == 0
    ids, k = sheet_ids(out, key)
    assert sorted(ids) == list(range(40, 70)) and k["_meta"]["source"] == "persona"


def test_sheet_never_repeats_an_insight_across_backends(tmp_path):
    # Two backends audited insights 0-9 and 20-29 with opposite verdicts; even
    # pooled (backend=None) an insight must appear at most once on the sheet.
    m, s = dbs(70), store.open_store(":memory:")
    seed_two_backends(s)
    for i in range(30, 40):   # keep the pooled supported pool big enough to fill
        store.add_audit(s, i, "wide", "supported", "", 0, VERTEX, "audit-v2", 1)
    out, key = tmp_path / "check.csv", tmp_path / "check.key.json"
    for seed in range(5):
        assert audit_sheet.write_check_sheet(s, m, None, str(out), str(key), seed=seed,
                                             source="wide") == 0
        ids, _ = sheet_ids(out, key)
        assert len(ids) == 30 and len(set(ids)) == 30, seed
    # ...and with the backend filter the pools cannot overlap in the first place.
    assert audit_sheet.write_check_sheet(s, m, None, str(out), str(key), backend=GEMMA,
                                         source="wide") == 0
    ids, _ = sheet_ids(out, key)
    assert len(ids) == 30 and len(set(ids)) == 30


def test_key_path_is_derived_with_splitext_and_never_the_sheet(tmp_path):
    assert audit_sheet.key_path_for("/a.csv.d/check") == "/a.csv.d/check.key.json"
    assert audit_sheet.key_path_for("/a.csv.d/check.csv") == "/a.csv.d/check.key.json"
    m, s = dbs(40), store.open_store(":memory:")
    for i in range(20):
        store.add_audit(s, i, "wide", "unsupported", "", 0, GEMMA, "audit-v1", 1)
    for i in range(20, 30):
        store.add_audit(s, i, "wide", "supported", "", 0, GEMMA, "audit-v1", 1)
    same = tmp_path / "check"
    assert audit_sheet.write_check_sheet(s, m, None, str(same), str(same)) == 2
    assert not same.exists()


def test_audit_sheet_main_defaults_to_gemma_wide_in_private_dir(tmp_path, monkeypatch):
    monkeypatch.setenv("HOME", str(tmp_path))
    mem_path = tmp_path / "mem.db"
    m = dbs(70)
    m.commit()
    m.execute("VACUUM INTO ?", (str(mem_path),))
    chat_path = tmp_path / "chat.db"
    sqlite3.connect(chat_path).execute("CREATE TABLE message (ROWID INTEGER PRIMARY KEY)")
    so = tmp_path / "so.db"
    seed_two_backends(store.open_store(str(so)))
    assert audit_sheet.main(["--store", str(so), "--mem-db", str(mem_path),
                             "--chat-db", str(chat_path)]) == 0
    d = tmp_path / ".human" / "second_opinion"
    k = json.loads((d / "audit_check.key.json").read_text())
    assert k["_meta"]["backend"] == GEMMA and k["_meta"]["source"] == "wide"
    assert (d.stat().st_mode & 0o777) == 0o700
    assert ((d / "audit_check.csv").stat().st_mode & 0o777) == 0o600


def labeled_with_meta(tmp_path, backend, source):
    labels = ([("unsupported", "n")] * 15 + [("unsupported", "y")] * 5 +
              [("supported", "y")] * 9 + [("supported", "n")] * 1)
    sheet, key = write_labeled(tmp_path, labels)
    k = json.loads(Path(key).read_text())
    k["_meta"] = {"backend": backend, "source": source, "prompt_versions": ["audit-v1"]}
    Path(key).write_text(json.dumps(k))
    return sheet, key


def test_audit_score_uses_the_backend_and_source_disagreement(tmp_path, capsys):
    so = tmp_path / "so.db"
    s = store.open_store(str(so))
    # Gemma wide: 1 unsupported / 4 supported -> d = 0.2. Gemma persona: all
    # unsupported. Vertex wide: all unsupported. Pooling would move d.
    store.add_audit(s, 1, "wide", "unsupported", "", 0, GEMMA, "audit-v1", 1)
    for i in range(2, 6):
        store.add_audit(s, i, "wide", "supported", "", 0, GEMMA, "audit-v1", 1)
    for i in range(10, 16):
        store.add_audit(s, i, "persona", "unsupported", "", 0, GEMMA, "audit-v1", 1)
    for i in range(1, 6):
        store.add_audit(s, i, "wide", "unsupported", "", 0, VERTEX, "audit-v2", 1)
    sheet, key = labeled_with_meta(tmp_path, GEMMA, "wide")
    assert audit_score.main([sheet, "--key", key, "--store", str(so)]) == 0
    r = json.loads(capsys.readouterr().out)
    assert r["disagreement_rate"] == 0.2
    assert r["backend"] == GEMMA and r["source"] == "wide" and r["prompt_versions"] == ["audit-v1"]
    assert r["d_treated_as_known"] is True and "known" in r["note"]
    assert "treats d as KNOWN" in audit_score.__doc__
    # persona population for the same backend gives a different d
    sheet, key = labeled_with_meta(tmp_path, GEMMA, "persona")
    assert audit_score.main([sheet, "--key", key, "--store", str(so), "--source",
                             "persona"]) == 0
    assert json.loads(capsys.readouterr().out)["disagreement_rate"] == 1.0


def test_audit_score_refuses_a_key_drawn_from_another_population(tmp_path):
    so = tmp_path / "so.db"
    s = store.open_store(str(so))
    store.add_audit(s, 1, "wide", "unsupported", "", 0, GEMMA, "audit-v1", 1)
    store.add_audit(s, 2, "wide", "supported", "", 0, GEMMA, "audit-v1", 1)
    sheet, key = labeled_with_meta(tmp_path, VERTEX, "wide")
    assert audit_score.main([sheet, "--key", key, "--store", str(so)]) == 2
    sheet, key = labeled_with_meta(tmp_path, GEMMA, "persona")
    assert audit_score.main([sheet, "--key", key, "--store", str(so)]) == 2


def test_audit_score_refuses_when_backend_has_no_audits(tmp_path):
    so = tmp_path / "so.db"
    s = store.open_store(str(so))
    store.add_audit(s, 1, "wide", "unsupported", "", 0, VERTEX, "audit-v2", 1)
    sheet, key = labeled_with_meta(tmp_path, GEMMA, "wide")
    assert audit_score.main([sheet, "--key", key, "--store", str(so)]) == 2
