"""Critiques where Seth replied, reference replies where he didn't (spec §4.3)."""
import csv
import datetime as dt
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import gold, judge, store  # noqa: E402

T0 = dt.datetime(2026, 9, 28, 12, 0, tzinfo=dt.timezone.utc)


JUDGE_MODEL = "mlx-community/gemma-4-31b-it-4bit"
COLS = ["id", "context", "option_A", "option_B", "choice", "confidence"]


def judged_sheet(path, rows, model=JUDGE_MODEL):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=COLS + ["judge_api", "judge_model"])
        w.writeheader()
        for iid, ch in rows:
            w.writerow({"id": iid, "choice": ch, "judge_api": "openai", "judge_model": model})
    return str(path)


def run_dir(tmp_path, name="run"):
    d = tmp_path / name
    d.mkdir(exist_ok=True)
    (d / "triples.json").write_text(json.dumps([
        {"id": f"x{i}", "context": f"ctx {i}", "seth_reply": f"seth {i}",
         "huuman_reply": f"ai {i}"} for i in range(4)]))
    (d / "answer_key.json").write_text(json.dumps({"x0": "A", "x1": "B", "x2": "A", "x3": "B"}))
    with open(d / "rating_sheet_seth.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=COLS)
        w.writeheader()
        w.writerow({"id": "x0", "choice": "A", "confidence": "5"})   # spotted, confident
        w.writerow({"id": "x1", "choice": "A", "confidence": "5"})   # fooled
        w.writerow({"id": "x2", "choice": "A", "confidence": "2"})   # spotted, unsure
        w.writerow({"id": "x3", "choice": "", "confidence": ""})     # not rated
    return str(d)


def lane_judged(tmp_path, model=JUDGE_MODEL, run="run", stamp=True):
    """The lane's own judge output: <reports>/judge-YYYYMMDD/judged.csv, plus the
    source.json judge_pass writes (the run dir it was judged against)."""
    path = judged_sheet(tmp_path / "reports" / "judge-20260927" / "judged.csv",
                        [("x2", "A"), ("x3", "B"), ("x1", "A")], model)
    if stamp:
        (Path(path).parent / "source.json").write_text(
            json.dumps(judge.run_stamp(str(tmp_path / run))))
    return path


def test_weak_items_human_first_then_lane_synthetic_with_model(tmp_path):
    items = gold.weak_items(run_dir(tmp_path), lane_judged(tmp_path), JUDGE_MODEL)
    src = "synthetic:" + JUDGE_MODEL
    # x1 is in the lane sheet but the judge was fooled (A != key B): not weak.
    assert [(i, s) for i, s, _ in items] == [("x0", "human"), ("x2", src), ("x3", src)]
    assert items[0][2]["seth_reply"] == "seth 0"


def test_weak_items_ignore_judged_sheets_inside_the_blind_ab_run_dir(tmp_path):
    # I1: a judged sheet sitting in the run dir may come from the prod-family
    # judge; it must contribute nothing, even when its judge_model matches.
    d = run_dir(tmp_path)
    judged_sheet(Path(d) / "judged.csv", [("x2", "A"), ("x3", "B")])
    judged_sheet(Path(d) / "rating_sheet_api_judge.csv", [("x1", "B")])
    assert [(i, s) for i, s, _ in gold.weak_items(d)] == [("x0", "human")]
    assert [(i, s) for i, s, _ in gold.weak_items(d, None, JUDGE_MODEL)] == [("x0", "human")]


def test_weak_items_ignore_lane_rows_from_another_judge_model(tmp_path):
    d = run_dir(tmp_path)
    other = lane_judged(tmp_path, model="glm-4.5-air")
    assert [(i, s) for i, s, _ in gold.weak_items(d, other, JUDGE_MODEL)] == [("x0", "human")]


def test_weak_items_returns_empty_for_a_preference_mode_answer_key(tmp_path):
    # A blind-A/B run made with make_rating_sheet.py --mode preference writes
    # answer_key.json with a top-level "_mode": "preference" marker, and the
    # key then means the MODEL's side — treating it as a detection key would
    # silently invert which items are "weak moments".
    d = run_dir(tmp_path)
    key = json.loads((Path(d) / "answer_key.json").read_text())
    key["_mode"] = "preference"
    (Path(d) / "answer_key.json").write_text(json.dumps(key))
    assert gold.weak_items(d, lane_judged(tmp_path), JUDGE_MODEL) == []


def test_weak_items_returns_empty_without_triples(tmp_path):
    d = run_dir(tmp_path)
    (Path(d) / "triples.json").unlink()
    assert gold.weak_items(d, lane_judged(tmp_path), JUDGE_MODEL) == []


def test_latest_lane_judged_picks_newest_judge_dir(tmp_path):
    from second_opinion import judge
    assert judge.latest_lane_judged(str(tmp_path / "reports")) is None
    old = judged_sheet(tmp_path / "reports" / "judge-20260920" / "judged.csv", [])
    new = judged_sheet(tmp_path / "reports" / "judge-20260927" / "judged.csv", [])
    (tmp_path / "reports" / "judge-20260927.json").write_text("{}")  # calibration file, not a dir
    assert judge.latest_lane_judged(str(tmp_path / "reports")) == new != old


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
    model = JUDGE_MODEL

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
    d = run_dir(tmp_path)
    jc = lane_judged(tmp_path)                   # stamped against d, so it matches
    c = gold.gold_pass(s, Fake(outputs), [d], a, mem(), limit=10, judged_csv=jc)
    assert c["critiques"] == 2 and c["unparseable"] == 1 and c["errors"] == 1
    assert c["references"] == 2
    assert c["critiques_skipped_no_run_dir"] == 0 and c["critiques_skipped_no_triples"] == 0
    assert s.execute("SELECT COUNT(*) FROM reference_replies WHERE rated IS NULL").fetchone()[0] == 2
    stored = s.execute("SELECT item_id, weak_source FROM critiques ORDER BY id").fetchall()
    assert stored == [("run/x0", "human"), ("run/x2", "synthetic:" + JUDGE_MODEL)]
    again = gold.gold_pass(s, Fake([]), [run_dir(tmp_path)], a, mem(), limit=10, judged_csv=jc)
    assert again["critiques"] == 0 and again["references"] == 0 and again["attempted"] == 1
    rep = gold.gold_report(s)
    assert rep["gaps"]["specific_detail"] == 1 and rep["gaps"]["other"] == 1
    assert rep["backends"] == ["fake@local"]


def test_gold_pass_scopes_item_ids_by_run_dir(tmp_path):
    # M4: "x0" in a re-exported run dir is a different item; it must not be
    # skipped because run/x0 was already critiqued.
    s = store.open_store(":memory:")
    ok = '{"gaps":["tone"],"missing":"m","severity":1}'
    gold.gold_pass(s, Fake([ok]), [run_dir(tmp_path, "run1")], None, mem(), limit=10)
    c = gold.gold_pass(s, Fake([ok]), [run_dir(tmp_path, "run2")], None, mem(), limit=10)
    assert c["critiques"] == 1
    assert {r[0] for r in s.execute("SELECT item_id FROM critiques")} == {"run1/x0", "run2/x0"}


def test_gold_pass_without_triples_still_writes_reference_replies(tmp_path):
    # I4: no triples.json in the run dir skips critiques (counted) but the
    # reference-reply half, which needs no run dir, still runs.
    s = store.open_store(":memory:")
    d = run_dir(tmp_path)
    (Path(d) / "triples.json").unlink()
    a = att()
    a["labeled"]["+1a"] = a["labeled"]["+1a"][:2]
    c = gold.gold_pass(s, Fake(["7pm? i'm in", "hey!"]), [d], a, mem(), limit=10,
                       judged_csv=lane_judged(tmp_path))
    assert c["critiques_skipped_no_triples"] == 1 and c["critiques"] == 0
    assert c["critiques_skipped_no_run_dir"] == 0
    assert c["references"] == 2


def test_gold_report_filters_and_names_backends():
    s = store.open_store(":memory:")
    store.add_critique(s, "r/x0", "human", ["tone"], "", 1, False, "g@local", "critique-v1", 5)
    store.add_critique(s, "r/x0", "human", ["length"], "", 1, False, "gem@vertex", "critique-v1", 5)
    store.add_reference(s, "+1a", 1, "c", "r", "gem@vertex", "reference-v1", 5)
    both = gold.gold_report(s)
    assert both["backends"] == ["g@local", "gem@vertex"] and both["critiques"] == 2
    local = gold.gold_report(s, backend="g@local")
    assert local == {"critiques": 1, "references": 0, "unparseable": 0, "gaps": {"tone": 1},
                     "backends": ["g@local"]}


# ---------------------------------------------------------------------------
# Follow-up: a lane judged.csv is used only for the run dir it was judged on.
# ---------------------------------------------------------------------------

def _pairs(items):
    return [(i, s) for i, s, _ in items]


def test_lane_sheet_with_matching_source_is_used(tmp_path):
    d = run_dir(tmp_path)
    src = "synthetic:" + JUDGE_MODEL
    assert _pairs(gold.weak_items(d, lane_judged(tmp_path), JUDGE_MODEL)) == [
        ("x0", "human"), ("x2", src), ("x3", src)]


def test_lane_sheet_for_another_run_dir_is_ignored(tmp_path):
    d = run_dir(tmp_path)
    run_dir(tmp_path, "other")                   # same key bytes, different name
    jc = lane_judged(tmp_path, run="other")
    assert _pairs(gold.weak_items(d, jc, JUDGE_MODEL)) == [("x0", "human")]


def test_lane_sheet_for_same_name_but_different_key_is_ignored(tmp_path):
    d = run_dir(tmp_path)
    jc = lane_judged(tmp_path)                   # stamped with today's key hash
    key = json.loads((Path(d) / "answer_key.json").read_text())
    key["x3"] = "A"                              # the run was re-exported
    (Path(d) / "answer_key.json").write_text(json.dumps(key))
    assert _pairs(gold.weak_items(d, jc, JUDGE_MODEL)) == [("x0", "human")]


def test_lane_sheet_without_source_json_is_ignored(tmp_path):
    d = run_dir(tmp_path)
    jc = lane_judged(tmp_path, stamp=False)
    assert _pairs(gold.weak_items(d, jc, JUDGE_MODEL)) == [("x0", "human")]


def test_gold_pass_counts_missing_run_dir_and_missing_key_as_no_run_dir(tmp_path):
    s = store.open_store(":memory:")
    c = gold.gold_pass(s, Fake([]), [], None, mem(), limit=10)
    assert c["critiques_skipped_no_run_dir"] == 1 and c["critiques_skipped_no_triples"] == 0
    d = run_dir(tmp_path)
    (Path(d) / "answer_key.json").unlink()
    c = gold.gold_pass(s, Fake([]), [d], None, mem(), limit=10)
    assert c["critiques_skipped_no_run_dir"] == 1 and c["critiques_skipped_no_triples"] == 0
