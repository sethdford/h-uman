"""scripts/insight_stream.py — memory-write admission control + provenance.

Before 2026-09-19 one sample at temperature 0.3 wrote whatever the model said,
an insight carried no pointer to the texts it came from, and nothing ever
retired one (hu_contact_insights_retire had no production caller). These tests
pin: generate once, verify K times — agreement scales confidence and a
minority note is rejected (ConsistencyGate 2607.22962; regenerate-and-intersect
was measured to admit 0 of 8 on 2026-09-19 because samples pick disjoint facts); the agreement count is persisted in `source`
(MemGuard 2608.21867); evidence turn indices map to message ids and the newest
evidence date becomes as_of_ms (PGMem 2608.01708); a supersession pair needs a
majority AND a newer-by-evidence superseder (A-TMA 2607.01935 ghost memory);
the live-table migration adds the two columns exactly once.
"""
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))

import insight_stream as ins  # noqa: E402


def note(text, conf=0.8, kind="fact", ev=None):
    return {"note": text, "kind": kind, "confidence": conf, "evidence": ev or []}


# ---- verify_claims / admit -----------------------------------------------------

class FakeArgs:
    url, model = "http://127.0.0.1:1", "fake"


def test_k1_is_the_historical_no_verification_path(monkeypatch):
    calls = []
    monkeypatch.setattr(ins, "call_model", lambda *a, **k: calls.append(1) or "[]")
    agree = ins.verify_claims(FakeArgs, "sys", "ctx", ["a", "b"], 1)
    assert agree == [1, 1] and calls == []  # no model call at K=1
    kept = ins.admit([note("a", 0.9), note("b", 0.4)], agree, 1)
    assert [(k["note"], k["confidence"], k["agree"]) for k in kept] == [("a", 0.9, 1), ("b", 0.4, 1)]
    assert ins.source_tag(1, 1) == "extractor:v1"


def test_verification_votes_are_counted_per_claim_across_shuffled_orders(monkeypatch):
    # The fake verifier supports any presented line whose text contains "mindy"
    # and rejects the rest, whatever position it is shown at.
    seen_orders = []

    def fake(url, model, system, user, timeout=300, temperature=0.3):
        lines = [l for l in user.split("candidate notes:\n", 1)[1].splitlines()]
        seen_orders.append(lines)
        out = [{"i": i, "supported": "mindy" in l} for i, l in enumerate(lines)]
        assert temperature == ins.VERIFY_TEMPERATURE
        return json.dumps(out)

    monkeypatch.setattr(ins, "call_model", fake)
    claims = ["her sister mindy lives in tampa", "hates cilantro", "mindy has a goldendoodle"]
    agree = ins.verify_claims(FakeArgs, "sys", "recent texts", claims, 3)
    assert agree == [3, 0, 3]
    assert len(seen_orders) == 3 and len({tuple(o) for o in seen_orders}) >= 2  # shuffled


def test_agreement_scales_confidence_and_minority_is_rejected():
    items = [note("her sister mindy lives in tampa", 0.9), note("hates cilantro", 0.9),
             note("cabo in november", 0.6, "plan")]
    kept = ins.admit(items, [3, 1, 2], 3)
    assert [k["note"] for k in kept] == ["her sister mindy lives in tampa", "cabo in november"]
    assert kept[0]["confidence"] == 0.9 and kept[0]["agree"] == 3
    assert kept[1]["confidence"] == 0.4 and kept[1]["agree"] == 2   # 0.6 * 2/3
    assert items[1]["confidence"] == 0.3                            # 0.9 * 1/3: under the 0.5 floor
    assert ins.source_tag(3, 2) == "extractor:v2:k3:a2"


def test_parse_verdicts_defaults_missing_or_junk_to_unsupported():
    assert ins.parse_verdicts('ok [{"i":0,"supported":true},{"i":2,"supported":"yes"},{"i":9,"supported":true},7]', 3) == [True, False, False]
    assert ins.parse_verdicts("no json", 2) == [False, False]


def test_admit_uses_the_action_key_for_prospective_items(capsys):
    items = [{"action": "ask how the interview went", "keywords": ["interview"], "days": 14,
              "confidence": 0.7}]
    assert ins.admit(items, [1], 3, label="c", text_key="action") == []
    assert "rejected (1/3 verifications) ask how the interview went" in capsys.readouterr().out


# ---- provenance -------------------------------------------------------------

def test_parse_notes_keeps_evidence_indices_in_any_spelling():
    raw = json.dumps([{"note": "just moved to st pete", "kind": "fact", "confidence": 0.9,
                       "evidence": ["t3", 7, "[t9]"]}])
    (n,) = ins.parse_notes(raw, 8)
    assert n["evidence"] == [3, 7, 9]


def test_evidence_maps_turn_indices_to_message_ids_and_newest_date():
    rows = [(101, 1_000, "them", "a"), (105, 5_000, "me", "b"), (109, 9_000, "them", "c")]
    assert ins.numbered_turns(rows)[1] == "[t1] me: b"
    ids, newest = ins.evidence_for([2, 0, 2, 40], rows)  # dup + out of range ignored
    assert ids == [101, 109] and newest == 9_000
    assert ins.evidence_for([], rows) == ([], 0)


def test_created_at_text_parses_and_garbage_is_zero():
    assert ins._created_at_ms("2026-09-19 19:21:17") > 1_780_000_000_000
    assert ins._created_at_ms("") == 0 and ins._created_at_ms("not a date") == 0


def test_migrate_adds_the_columns_once_to_a_live_table():
    db = sqlite3.connect(":memory:")
    db.executescript(
        "CREATE TABLE contact_insights(id INTEGER PRIMARY KEY, contact_id TEXT, kind TEXT,"
        " insight TEXT, confidence REAL, as_of_ms INTEGER, source TEXT, created_at_ms INTEGER,"
        " retired_at_ms INTEGER NOT NULL DEFAULT 0);")
    ins.migrate(db)
    ins.migrate(db)  # idempotent
    cols = {r[1] for r in db.execute("PRAGMA table_info(contact_insights)")}
    assert {"evidence_ids", "superseded_by_id"} <= cols
    db.execute("INSERT INTO contact_insights(contact_id, kind, insight, confidence, as_of_ms,"
               " source, created_at_ms, evidence_ids) VALUES('c','fact','x',0.9,1,'s',1,'[101]')")
    assert db.execute("SELECT superseded_by_id FROM contact_insights").fetchone()[0] == 0


# ---- supersession -----------------------------------------------------------

LIVE = {  # id: (as_of_ms, kind, insight)
    1: (1_000, "fact", "works at vanguard"),
    2: (2_000, "plan", "interview at publix thursday"),
    3: (9_000, "fact", "started at publix, loves the team"),
}


def pairs(*ps):
    return [{"action": f"{a}->{b}", "stale": a, "by": b, "confidence": 1.0} for a, b in ps]


def test_supersession_needs_majority_and_a_newer_superseder():
    # (1,3) verified 3/3; (2,3) 1/3; (3,1) 3/3 but backwards in time
    acc, ref = ins.supersession_candidates(pairs((1, 3), (2, 3), (3, 1)), [3, 1, 3], 3, LIVE)
    assert acc == [(1, 3)]
    reasons = {(s, b): why for s, b, why in ref}
    assert "1/3" in reasons[(2, 3)]
    assert "not newer" in reasons[(3, 1)]  # older note can never supersede a newer one


def test_supersession_refuses_unknown_ids_and_k1_admits_one_pass():
    acc, ref = ins.supersession_candidates(pairs((1, 3), (7, 3)), [1, 1], 1, LIVE)
    assert acc == [(1, 3)] and ref[0][:2] == (7, 3)


def test_parse_supersessions_drops_self_pairs_and_junk():
    raw = 'sure: [{"stale_id": 1, "superseded_by_id": 3}, {"stale_id": 2, "superseded_by_id": 2}, 5]'
    out = ins.parse_supersessions(raw)
    assert [(o["stale"], o["by"]) for o in out] == [(1, 3)]
    assert ins.parse_supersessions("no json here") == []
