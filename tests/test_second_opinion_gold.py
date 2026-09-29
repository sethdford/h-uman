"""Critiques where Seth replied, reference replies where he didn't (spec §4.3)."""
import csv
import datetime as dt
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import gold, store  # noqa: E402

T0 = dt.datetime(2026, 9, 28, 12, 0, tzinfo=dt.timezone.utc)


def run_dir(tmp_path):
    d = tmp_path / "run"
    d.mkdir(exist_ok=True)
    (d / "triples.json").write_text(json.dumps([
        {"id": f"x{i}", "context": f"ctx {i}", "seth_reply": f"seth {i}",
         "huuman_reply": f"ai {i}"} for i in range(4)]))
    (d / "answer_key.json").write_text(json.dumps({"x0": "A", "x1": "B", "x2": "A", "x3": "B"}))
    cols = ["id", "context", "option_A", "option_B", "choice", "confidence"]
    with open(d / "rating_sheet_seth.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        w.writerow({"id": "x0", "choice": "A", "confidence": "5"})   # spotted, confident
        w.writerow({"id": "x1", "choice": "A", "confidence": "5"})   # fooled
        w.writerow({"id": "x2", "choice": "A", "confidence": "2"})   # spotted, unsure
        w.writerow({"id": "x3", "choice": "", "confidence": ""})     # not rated
    with open(d / "judged.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols + ["judge_model"])
        w.writeheader()
        w.writerow({"id": "x2", "choice": "A", "judge_model": "gemma"})
        w.writerow({"id": "x3", "choice": "B", "judge_model": "gemma"})
    return str(d)


def test_weak_items_human_first_then_synthetic(tmp_path):
    items = gold.weak_items(run_dir(tmp_path))
    assert [(i, s) for i, s, _ in items] == [("x0", "human"), ("x2", "synthetic"),
                                             ("x3", "synthetic")]
    assert items[0][2]["seth_reply"] == "seth 0"


def test_parse_critique_filters_gaps_and_never_guesses():
    ok = gold.parse_critique('ok {"gaps": ["tone", "bogus"], "missing": "the date", "severity": 7}')
    assert ok == (["tone"], "the date", 3, False)
    assert gold.parse_critique('{"gaps": [], "missing": "x"}')[0] == ["other"]
    assert gold.parse_critique("Sure, the AI reply is too long") == (["other"], "", None, True)


def msg(rowid, minutes, from_me, text):
    return {"rowid": rowid, "guid": f"g{rowid}", "t": T0 + dt.timedelta(minutes=minutes),
            "from_me": from_me, "text": text}


def att():
    them1, bot1, them2, bot2, me2 = (msg(1, 0, False, "hey"), msg(2, 1, True, "yo"),
                                     msg(3, 60, False, "dinner?"), msg(4, 61, True, "sure"),
                                     msg(5, 70, True, "7pm works"))
    return {"timelines": {"+1a": [them1, bot1, them2, bot2, me2]},
            "labeled": {"+1a": [(bot1, "huuman"), (bot2, "huuman"), (me2, "seth")]}}


def test_unanswered_daemon_replies_skip_ones_seth_followed_up():
    got = gold.unanswered_daemon_replies(att())
    assert [(c, m["rowid"]) for c, m, _ in got] == []  # seth replied within 24h after both
    a = att()
    a["labeled"]["+1a"] = a["labeled"]["+1a"][:2]      # no seth follow-up at all
    got = gold.unanswered_daemon_replies(a)
    assert [m["rowid"] for _, m, _ in got] == [4, 2]   # newest first
    assert [x["rowid"] for x in got[0][2]] == [1, 2, 3]  # context strictly before


def mem():
    m = sqlite3.connect(":memory:")
    m.executescript("CREATE TABLE contact_insights (id INTEGER PRIMARY KEY, contact_id TEXT,"
                    " insight TEXT, confidence REAL, as_of_ms INTEGER, source TEXT,"
                    " retired_at_ms INTEGER DEFAULT 0);"
                    "INSERT INTO contact_insights VALUES (1,'+1a','likes sushi',0.9,5,'extractor:v1',0),"
                    "(2,'+1a','wide note',0.9,6,'curator_wide:x',0),"
                    "(3,'+1a','low conf',0.2,7,'extractor:v1',0),"
                    "(4,'+1a','retired',0.9,8,'extractor:v1',9);")
    return m


def test_contact_notes_match_the_render_gate():
    assert gold.contact_notes(mem(), "+1a", wide_live=False) == ["likes sushi"]
    assert gold.contact_notes(mem(), "+1a", wide_live=True) == ["wide note", "likes sushi"]


class Fake:
    name = "fake@local"

    def __init__(self, outputs):
        self.outputs = list(outputs)

    def generate(self, system, user, max_tokens=400):
        out = self.outputs.pop(0)
        if isinstance(out, Exception):
            raise out
        return out


def test_gold_pass_writes_critiques_and_unrated_references_and_dedupes(tmp_path):
    s = store.open_store(":memory:")
    a = att()
    a["labeled"]["+1a"] = a["labeled"]["+1a"][:2]
    outputs = ['{"gaps":["specific_detail"],"missing":"m","severity":2}',
               "not json", TimeoutError(), "7pm? i'm in", "hey!"]
    c = gold.gold_pass(s, Fake(outputs), [run_dir(tmp_path)], a, mem(), limit=10)
    assert c["critiques"] == 2 and c["unparseable"] == 1 and c["errors"] == 1
    assert c["references"] == 2
    assert s.execute("SELECT COUNT(*) FROM reference_replies WHERE rated IS NULL").fetchone()[0] == 2
    again = gold.gold_pass(s, Fake([]), [run_dir(tmp_path)], a, mem(), limit=10)
    assert again["critiques"] == 0 and again["references"] == 0 and again["attempted"] == 1
    rep = gold.gold_report(s)
    assert rep["gaps"]["specific_detail"] == 1 and rep["gaps"]["other"] == 1
