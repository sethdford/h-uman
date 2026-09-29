"""scripts/consolidate_wiki.py — sleep-time consolidation into per-contact pages.

Pins: every bullet carries a provenance tag and points at a real row; near-
duplicate insights collapse; a page is trimmed to its byte budget from the
oldest memory up; the self page repeats the persona identity sentence; a lint
failure on any page writes NOTHING; open threads come from unfired,
unexpired prospective rows grouped by intention.
"""
import os
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))

import consolidate_wiki as cw  # noqa: E402

BREA = "+15550000001"
NOW_S = 1_789_300_000            # 2026-09-13
SEP_2026_MS = 1_789_290_000_000

PERSONA = {
    "name": "Seth",
    "core": {"identity": "Seth Ford, Chief Architect at Acme in St. Petersburg, Florida. "
                         "Father of three who visit on breaks."},
    "life_events": [
        {"description": "renting out the old place", "state": "in_progress", "as_of": "2026-07-26"},
        {"description": "moved to st pete", "state": "completed", "as_of": "2026-07-28"},
    ],
    "style_rules": ["Default to natural short texts.", "Normal capitalization.", "Use contractions.",
                    "A fourth rule that must not appear."],
    "contacts": {
        BREA: {"name": "Brea", "relationship": "broker", "greeting_style": "professional, warm",
               "prefers_short_texts": False, "uses_emoji": False, "texts_in_bursts": True,
               "warmth_level": "medium",
               "identity": "Brea is the realtor renting out Seth's old place. She lives in PA."},
        "+15550000009": {"name": "Unknown caller", "relationship": ""},
        "+15550000008": {"name": "Tester", "relationship": "test"},
    },
}


def make_db():
    db = sqlite3.connect(":memory:")
    db.executescript(
        "CREATE TABLE contact_insights(id INTEGER PRIMARY KEY, contact_id TEXT, kind TEXT, "
        "insight TEXT, confidence REAL, as_of_ms INTEGER, retired_at_ms INTEGER DEFAULT 0);"
        "CREATE TABLE prospective_memories(id INTEGER PRIMARY KEY, trigger_type TEXT, "
        "trigger_value TEXT, action TEXT, contact_id TEXT, expires_at INTEGER, "
        "fired INTEGER DEFAULT 0, created_at INTEGER);")
    return db


def insight(db, cid, text, kind="fact", conf=0.8, as_of=SEP_2026_MS, retired=0, rid=None):
    db.execute("INSERT INTO contact_insights(id, contact_id, kind, insight, confidence, as_of_ms, "
               "retired_at_ms) VALUES(?,?,?,?,?,?,?)", (rid, cid, kind, text, conf, as_of, retired))


def trigger(db, cid, kw, action, fired=0, expires=0, created=NOW_S - 100, rid=None):
    db.execute("INSERT INTO prospective_memories(id, trigger_type, trigger_value, action, "
               "contact_id, expires_at, fired, created_at) VALUES(?,'keyword',?,?,?,?,?,?)",
               (rid, kw, action, cid, expires, fired, created))


def test_contact_page_has_tagged_lines_from_real_rows():
    db = make_db()
    insight(db, BREA, "asking rent $4500, pet fee refundable", rid=7)
    insight(db, BREA, "security deposit at Chase downtown", kind="fact", rid=8)
    trigger(db, BREA, "lease", "review lease agreement", rid=12)
    trigger(db, BREA, "sign", "review lease agreement", rid=13)
    trigger(db, BREA, "old", "already fired", fired=1)
    trigger(db, BREA, "gone", "expired thread", expires=NOW_S - 1)
    pages, problems = cw.build_all(db, PERSONA, [BREA], NOW_S)
    assert problems == {}
    text = pages[f"{BREA}.md"]
    assert text.startswith("# Brea (broker) — compiled 2026-09-13\n")
    assert "- Brea is the realtor renting out Seth's old place. [persona:contacts.+15550000001.identity]" in text
    assert "- review lease agreement (cue: lease, sign) [pm:12,13]" in text
    assert "already fired" not in text and "expired thread" not in text
    assert "- asking rent $4500, pet fee refundable (Sep 2026) [ins:7]" in text
    assert "- professional, warm; texts in bursts; warmth medium [persona:contacts.+15550000001]" in text
    for ln in text.splitlines():
        if ln and not ln.startswith("#"):
            assert cw.TAG.search(ln), ln


def test_near_duplicate_insights_collapse_and_kinds_are_capped():
    db = make_db()
    insight(db, BREA, "rent payment via Zelle, $500k renters insurance required", rid=1,
            as_of=SEP_2026_MS)
    insight(db, BREA, "rent payment via zelle and $500k renters insurance required", rid=2,
            as_of=SEP_2026_MS - 1000)
    plans = ["showing with lewis late morning tomorrow", "walkthrough friday with the appliance guy",
             "sign the lease monday at chase downtown", "photographer coming for listing photos",
             "compare both tenant applications before deciding", "call about the pet fee refund"]
    for i, p in enumerate(plans):
        insight(db, BREA, p, kind="plan", rid=10 + i)
    notes = cw.insights_for(db, BREA)
    ids = [n["id"] for n in notes]
    assert 1 in ids and 2 not in ids           # newer twin kept, older dropped
    assert sum(1 for n in notes if n["kind"] == "plan") == cw.MAX_PER_KIND


def test_page_is_trimmed_to_budget_from_the_oldest_memory(monkeypatch):
    db = make_db()
    for i in range(10):
        insight(db, BREA, f"memory {i} " + "x" * 150, rid=100 + i, as_of=SEP_2026_MS - i)
    monkeypatch.setattr(cw, "MAX_BYTES", 900)
    pages, problems = cw.build_all(db, PERSONA, [BREA], NOW_S)
    assert problems == {}
    text = pages[f"{BREA}.md"]
    assert len(text.encode()) <= 900
    assert "[ins:100]" in text                 # newest survives
    assert "[ins:109]" not in text             # oldest trimmed


def test_self_page_repeats_identity_and_in_progress_events():
    db = make_db()
    pages, problems = cw.build_all(db, PERSONA, ["self"], NOW_S)
    assert problems == {}
    text = pages["self.md"]
    assert "- Seth Ford, Chief Architect at Acme in St. Petersburg, Florida. [persona:core.identity]" in text
    assert "- renting out the old place (as of 2026-07-26) [persona:life_events.0]" in text
    assert "moved to st pete" not in text
    assert "[persona:style_rules.2]" in text and "fourth rule" not in text


def test_lint_rejects_untagged_lines_and_identity_drift():
    assert cw.lint_page("# t\n- fine [ins:1]\n") == []
    assert any("provenance" in p for p in cw.lint_page("# t\n- no tag here\n"))
    assert any("provenance" in p for p in cw.lint_page("# t\nprose without bullet [ins:1]\n"))
    assert any("bytes" in p for p in cw.lint_page("# t\n- " + "x" * 50 + " [ins:1]\n", max_bytes=20))
    assert any("missing" in p for p in cw.lint_page("# t\n- a [ins:1]\n", must_contain="Acme"))


def test_run_writes_nothing_when_any_page_fails_lint(tmp_path, monkeypatch, capsys):
    db = make_db()
    insight(db, BREA, "fine note", rid=1)
    bad = dict(PERSONA)
    bad["core"] = {"identity": ""}   # self page loses its identity line -> lint must fail
    monkeypatch.setattr(cw, "lint_page",
                        lambda text, max_bytes=cw.MAX_BYTES, must_contain=None:
                        ["forced failure"] if text.startswith("# Seth (self)") else [])
    rc = cw.run(db, bad, str(tmp_path), write=True, now_s=NOW_S)
    assert rc == 1
    assert list(tmp_path.iterdir()) == []


def test_run_publishes_all_pages_atomically_and_skips_test_contacts(tmp_path):
    db = make_db()
    insight(db, BREA, "fine note", rid=1)
    rc = cw.run(db, PERSONA, str(tmp_path), write=True, now_s=NOW_S)
    assert rc == 0
    names = sorted(p.name for p in tmp_path.iterdir())
    assert names == [f"{BREA}.md", "self.md"]   # 'test' and 'Unknown' contacts skipped
    assert not any(n.startswith(".") for n in names)
    assert os.path.getsize(tmp_path / f"{BREA}.md") <= cw.MAX_BYTES
