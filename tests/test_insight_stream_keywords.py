"""scripts/insight_stream.py — prospective-trigger keyword quality.

Live 2026-09-13: 949 open triggers, the keyword "work" on 18 of them (7 different
intentions for one contact), so a single inbound "take off work" cued seven
reminders at once. A cue has to be a whole word that is RARE in that contact's
own texts and backs exactly one open intention. These tests pin the frequency
filter, the stop list, the one-cue-one-intention rule, and the prune pass that
retires the rows already written (fired=2, distinct from a real fire).
"""
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))

import insight_stream  # noqa: E402

CONTACT = "+15550000001"


def make_db(inbound):
    db = sqlite3.connect(":memory:")
    db.executescript(
        "CREATE TABLE messages(id INTEGER PRIMARY KEY, session_id TEXT, role TEXT, "
        "content TEXT, created_at TEXT);"
        "CREATE TABLE prospective_memories(id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "trigger_type TEXT NOT NULL, trigger_value TEXT NOT NULL, action TEXT NOT NULL, "
        "contact_id TEXT, expires_at INTEGER, fired INTEGER DEFAULT 0, "
        "created_at INTEGER NOT NULL);")
    db.executemany("INSERT INTO messages(session_id, role, content, created_at) VALUES(?,?,?,?)",
                   [(CONTACT, "user", t, "2026-09-10 10:00:00") for t in inbound])
    return db


def twenty_texts(*with_word):
    """20 inbound texts; the given ones replace the filler at the front."""
    texts = [f"filler text number {i}" for i in range(20)]
    for i, t in enumerate(with_word):
        texts[i] = t
    return texts


def test_keyword_share_counts_whole_words_only():
    texts = ["ru a bath and body works kinda guy", "i have to take off work", "nothing here"]
    assert insight_stream.keyword_share("work", texts) == 1 / 3
    assert insight_stream.keyword_share("works", texts) == 1 / 3
    assert insight_stream.keyword_share("github invite", ["did you resend the github invite?"]) == 1
    assert insight_stream.keyword_share("dyson", []) == 0.0


def test_generic_keyword_is_rejected_and_rare_one_kept():
    texts = twenty_texts("trials start monday", "trials went ok", "more trials")  # 3/20 = 15%
    assert "generic" in insight_stream.keyword_reject_reason("trials", texts)
    assert insight_stream.keyword_reject_reason("work", texts) == "stop word"
    assert insight_stream.keyword_reject_reason("dyson", texts) is None
    # exactly at the threshold is still allowed (1/20 = 5%)
    assert insight_stream.keyword_reject_reason("dyson", twenty_texts("the dyson came")) is None


def test_stop_words_and_letterless_keywords_are_rejected():
    assert insight_stream.keyword_reject_reason("today", []) == "stop word"
    assert insight_stream.keyword_reject_reason("2026", []) == "no letters"


def test_filter_keywords_drops_a_cue_already_backing_another_intention():
    texts = twenty_texts()
    open_cues = {"flight": "ask about flight home"}
    kept, dropped = insight_stream.filter_keywords(
        ["flight", "airport", "today"], texts, open_cues, "ask about the layover")
    assert kept == ["airport"]
    assert [d[0] for d in dropped] == ["flight", "today"]
    # the same intention may keep re-using its own cue (idempotent nightly rerun)
    kept, _ = insight_stream.filter_keywords(["flight"], texts, open_cues, "ask about flight home")
    assert kept == ["flight"]


def seed(db, kw, action, fired=0):
    db.execute("INSERT INTO prospective_memories(trigger_type,trigger_value,action,contact_id,"
               "expires_at,fired,created_at) VALUES('keyword',?,?,?,0,?,1)",
               (kw, action, CONTACT, fired))


def test_prune_retires_generic_and_duplicate_cues_with_fired_2():
    db = make_db(twenty_texts("off work today", "work was long", "back at work"))
    seed(db, "work", "ask about their internship")      # generic
    seed(db, "work", "ask about the wfh day")           # generic
    seed(db, "dyson", "ask about dyson charger")        # specific, first owner of "dyson"
    seed(db, "dyson", "ask about their vacuum review")  # duplicate cue
    seed(db, "charger", "ask about dyson charger")      # specific
    seed(db, "today", "check on them")                  # stop word
    seed(db, "work", "already fired", fired=1)          # not open: untouched

    dry = insight_stream.prune_pass(db, [CONTACT], write=False)
    assert dry["retired"] == 4
    assert db.execute("SELECT COUNT(*) FROM prospective_memories WHERE fired=2").fetchone()[0] == 0

    wet = insight_stream.prune_pass(db, [CONTACT], write=True)
    assert wet["retired"] == 4
    rows = db.execute("SELECT trigger_value, action, fired FROM prospective_memories "
                      "ORDER BY id").fetchall()
    assert rows == [
        ("work", "ask about their internship", 2),
        ("work", "ask about the wfh day", 2),
        ("dyson", "ask about dyson charger", 0),
        ("dyson", "ask about their vacuum review", 2),
        ("charger", "ask about dyson charger", 0),
        ("today", "check on them", 2),
        ("work", "already fired", 1),
    ]
    # intentions left without any live cue are reported, not silently orphaned
    assert wet["orphaned"] == 4  # internship, wfh day, vacuum review, check on them
    # a second pass finds nothing more to do
    assert insight_stream.prune_pass(db, [CONTACT], write=True)["retired"] == 0
