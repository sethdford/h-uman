"""--names: nightly typed-name pass (spec 2026-09-29 §4.4).

Hermetic like test_insight_stream_wide.py: MEMORY_DB, CURATOR_STATE and
HUMAN_CONFIG point into tmp_path (autouse fixture imported below); the model,
chat.db attribution and the health probe are monkeypatched; the `human`
binary is a fake script in tmp_path that records its argv and env.
"""
import datetime as dt
import json
import os
import sqlite3
import stat
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
sys.path.insert(0, str(Path(__file__).parent))
import curator_evidence as ce  # noqa: E402
import curator_names as cn  # noqa: E402
import insight_stream as ins  # noqa: E402
from test_insight_stream_wide import A, H, NOW, timeline, _hermetic_memory_db  # noqa: E402,F401

LABELS = {**{f"me{i}": "seth" for i in range(6)}, "bot": "huuman"}
MODEL = json.dumps([
    {"name": "priya", "type": "person", "evidence": ["t0"]},
    {"name": "Marcus", "type": "person", "evidence": ["d0"]},
    {"name": "Dana", "type": "person", "evidence": ["t1"]},
    {"name": "Sam", "type": "person", "evidence": ["t0"]},
    {"name": "surgery", "type": "topic", "evidence": ["t0"]},
    {"name": "Tuesday", "type": "weird", "evidence": ["t0"]},
])

FAKE_BIN = """#!{py}
import json, os, sys
with open(os.environ["FAKE_LOG"], "w") as f:
    json.dump({{"argv": sys.argv[1:], "graph": os.environ.get("HU_GRAPH_DB"),
               "mode": os.stat(sys.argv[3]).st_mode & 0o777,
               "lines": open(sys.argv[3]).read().splitlines()}}, f)
print(json.dumps({{"imported": 0, "entities": int(os.environ.get("FAKE_ENTITIES", "2")),
                  "skipped": 0, "graph": "x"}}))
sys.exit(int(os.environ.get("FAKE_RC", "0")))
"""


class NA(A):
    names_days = 2
    window_days = 30
    deadline = None
    write = True


def _fake_bin(tmp_path, monkeypatch):
    p = tmp_path / "human"
    p.write_text(FAKE_BIN.format(py=sys.executable))
    p.chmod(0o755)
    monkeypatch.setenv("FAKE_LOG", str(tmp_path / "fake.json"))
    return str(p)


def _args(tmp_path, monkeypatch, write=True):
    a = NA()
    a.write = write
    a.names_dir = str(tmp_path / "names")
    a.manifest_dir = str(tmp_path / "manifests")
    a.graph_db = str(tmp_path / "graph.db")
    a.human_bin = _fake_bin(tmp_path, monkeypatch)
    chat = tmp_path / "chat.db"
    c = sqlite3.connect(chat)
    c.execute("CREATE TABLE message (rowid INTEGER)")
    c.commit(); c.close()
    a.chat_db = str(chat)
    monkeypatch.setattr(ins, "_utc_now", lambda: NOW)  # run_names' clock; timeline() is NOW-relative
    monkeypatch.setattr(ins.urllib.request, "urlopen", lambda *x, **k: None)
    import eval_conversation_quality as cq
    monkeypatch.setattr(cq, "attribute",
                        lambda *x, **k: {"timelines": {H: timeline()}, "labels": LABELS})
    return a


def _cite():
    rows = ce.chat_turn_rows(timeline(), LABELS, 80, NOW - dt.timedelta(days=2))
    return ce.number_rows(rows)[1]


def test_parse_names_drops_malformed():
    assert cn.parse_names("no json") == []
    assert cn.parse_names('[{"name": ""}, "x", {"name": "Priya", "type": "Person"}]') == [
        {"name": "Priya", "type": "person", "evidence_tokens": []}]


def test_verify_names_keeps_only_said_cited_names():
    kept, rejected = cn.verify_names(cn.parse_names(MODEL), _cite(), cn.drop_names("Sam"))
    assert kept == [{"name": "Priya", "type": "person"}, {"name": "surgery", "type": "topic"}]
    # Marcus cites a daemon row, Dana is not said in t1, Sam is dropped, Tuesday's type is bad
    assert rejected == 4


def test_verify_names_rejects_any_daemon_citation_even_beside_a_good_one():
    """A name the daemon itself wrote must not become a memory because the model
    also cited a human text that happens to contain it (fails closed)."""
    proposed = [{"name": "priya", "type": "person", "evidence_tokens": ["t0", "d0"]}]
    assert cn.verify_names(proposed, _cite(), cn.drop_names(None)) == ([], 1)


def test_canonical_name_capitalizes_name_types():
    """Review Focus 2: texting-lowercase names land on the capitalized row."""
    # end to end: the model answers "priya" for "priya's surgery"; the kept row is "Priya"
    kept, _ = cn.verify_names([{"name": "priya", "type": "person", "evidence_tokens": ["t0"]}],
                              _cite(), cn.drop_names(None))
    assert kept == [{"name": "Priya", "type": "person"}]
    assert cn.canonical_name("priya", "person") == "Priya"
    assert cn.canonical_name("st pete", "place") == "St Pete"
    assert cn.canonical_name("the lake house", "topic") == "the lake house"
    assert cn.canonical_name("McKinsey", "org") == "McKinsey"


def test_drop_names_covers_seth_and_the_contact():
    d = cn.drop_names("Sam Rivera")
    assert {"seth", "seth ford", "sam rivera", "sam"} <= d


def test_write_jsonl_private_is_0600(tmp_path):
    p = cn.write_jsonl_private(str(tmp_path / "d" / "n.jsonl"), [{"a": 1}])
    assert stat.S_IMODE(os.stat(p).st_mode) == 0o600
    assert stat.S_IMODE(os.stat(tmp_path / "d").st_mode) == 0o700
    assert json.loads(open(p).read()) == {"a": 1}


def test_write_jsonl_private_never_inherits_a_wider_mode_or_follows_a_symlink(tmp_path):
    d = tmp_path / "d"
    d.mkdir()
    victim = tmp_path / "victim"
    victim.write_text("keep")
    (d / "n.jsonl").symlink_to(victim)
    (d / "o.jsonl").write_text("old")
    (d / "o.jsonl").chmod(0o644)
    for name in ("n.jsonl", "o.jsonl"):
        p = cn.write_jsonl_private(str(d / name), [{"a": 1}])
        assert not os.path.islink(p)
        assert stat.S_IMODE(os.stat(p).st_mode) == 0o600
    assert victim.read_text() == "keep"
    assert sorted(os.listdir(d)) == ["n.jsonl", "o.jsonl"]  # no temp file left behind


def test_run_import_times_out_and_missing_binary_are_failures(tmp_path):
    slow = tmp_path / "slow"
    slow.write_text(f"#!{sys.executable}\nimport time; time.sleep(5)\n")
    slow.chmod(0o755)
    assert cn.run_import(str(slow), str(tmp_path / "g.db"), "x.jsonl", timeout=0.5) == (None, -1)
    assert cn.run_import(str(tmp_path / "nope"), str(tmp_path / "g.db"), "x.jsonl") == (None, -1)


def test_parse_import_output():
    assert cn.parse_import_output('log\n{"imported": 0, "entities": 3, "skipped": 1}\n') == 3
    assert cn.parse_import_output("garbage") is None


def test_names_pass_includes_persona_contacts_and_counts_only(monkeypatch):
    monkeypatch.setattr(ins, "call_model", lambda *x, **k: MODEL)
    # The contact IS Priya: her own display name must never become an entity.
    man, lines = ins.names_pass(NA(), {H: {"name": "Priya Shah"}},
                                {"timelines": {H: timeline()}, "labels": LABELS}, NOW)
    assert man["eligible"] == 1 and man["contacts"] == 1  # a persona contact is NOT skipped
    assert man["names_proposed"] == 6
    assert man["names_kept"] == 1 and man["names_rejected"] == 5
    assert man["by_type"]["topic"] == 1 and man["by_type"]["person"] == 0
    assert lines == [{"kind": "entity", "contact": H, "name": "surgery", "type": "topic",
                      "source": "names:nightly", "confidence": 0.8}]
    blob = json.dumps(man)
    assert "surgery" not in blob and H not in blob


def test_quiet_contact_is_not_eligible(monkeypatch):
    old = [dict(m, t=m["t"] - dt.timedelta(days=5)) for m in timeline()]
    monkeypatch.setattr(ins, "call_model", lambda *x, **k: pytest.fail("model called"))
    man, lines = ins.names_pass(NA(), {}, {"timelines": {H: old}, "labels": LABELS}, NOW)
    assert man["eligible"] == 0 and lines == []


def test_model_error_on_a_contact_is_counted_not_fatal(monkeypatch):
    def boom(*x, **k):
        raise TimeoutError("down")
    monkeypatch.setattr(ins, "call_model", boom)
    man, lines = ins.names_pass(NA(), {}, {"timelines": {H: timeline()}, "labels": LABELS}, NOW)
    assert man["model_errors"] == 1 and man["contacts"] == 0 and lines == []
    assert man["model_error_types"] == {"TimeoutError": 1}  # type name only, never the text


def test_deadline_stops_before_the_next_contact(monkeypatch):
    monkeypatch.setattr(ins, "call_model", lambda *x, **k: pytest.fail("model called"))
    man, _ = ins.names_pass(NA(), {}, {"timelines": {H: timeline()}, "labels": LABELS}, NOW,
                            deadline=NOW - dt.timedelta(minutes=1))
    assert man["stopped_at_deadline"] == 1 and man["unreached_at_deadline"] == 1


def test_run_names_write_imports_through_the_binary(tmp_path, monkeypatch):
    a = _args(tmp_path, monkeypatch)
    monkeypatch.setattr(ins, "call_model", lambda *x, **k: MODEL)
    assert ins.run_names(a, {}) == 0
    log = json.load(open(tmp_path / "fake.json"))
    assert log["argv"][:2] == ["memory", "import-facts"]
    assert log["graph"] == a.graph_db
    assert [json.loads(ln)["name"] for ln in log["lines"]] == ["Priya", "surgery"]
    assert log["mode"] == 0o600  # private while the importer reads it...
    assert not os.path.exists(log["argv"][2])  # ...and gone once the import is done
    man = json.load(open(next((tmp_path / "manifests").glob("names-manifest-*.json"))))
    assert man["import_entities"] == 2 and man["import_failed"] == 0 and not man["dry_run"]
    blob = json.dumps(man)
    assert "Priya" not in blob and "surgery" not in blob and H not in blob


def test_run_names_write_with_no_kept_names_skips_the_importer(tmp_path, monkeypatch):
    a = _args(tmp_path, monkeypatch)
    monkeypatch.setattr(ins, "call_model", lambda *x, **k: "[]")
    assert ins.run_names(a, {}) == 0
    assert not (tmp_path / "fake.json").exists()  # N+E==0 would make the importer exit 1
    man = json.load(open(next((tmp_path / "manifests").glob("names-manifest-*.json"))))
    assert man["contacts"] == 1 and man["names_kept"] == 0
    assert man["import_entities"] == 0 and man["import_failed"] == 0


def test_run_names_dry_run_never_imports(tmp_path, monkeypatch):
    a = _args(tmp_path, monkeypatch, write=False)
    monkeypatch.setattr(ins, "call_model", lambda *x, **k: MODEL)
    assert ins.run_names(a, {}) == 0
    assert not (tmp_path / "fake.json").exists()
    assert list((tmp_path / "manifests").glob("names-manifest-*-dryrun.json"))
    # the dry run's only inspectable output: a private JSONL that can't pass for a real night
    (jsonl,) = (tmp_path / "names").glob("names-*-dryrun.jsonl")
    assert stat.S_IMODE(os.stat(jsonl).st_mode) == 0o600
    assert [json.loads(ln)["name"] for ln in open(jsonl)] == ["Priya", "surgery"]


def test_run_names_import_failure_exits_2(tmp_path, monkeypatch):
    a = _args(tmp_path, monkeypatch)
    monkeypatch.setenv("FAKE_RC", "1")
    monkeypatch.setattr(ins, "call_model", lambda *x, **k: MODEL)
    assert ins.run_names(a, {}) == 2
    man = json.load(open(next((tmp_path / "manifests").glob("names-manifest-*.json"))))
    assert man["import_failed"] == 1 and man["import_entities"] == 0
    assert not os.path.exists(json.load(open(tmp_path / "fake.json"))["argv"][2])


def test_run_names_unparseable_import_output_is_a_failure(tmp_path, monkeypatch):
    a = _args(tmp_path, monkeypatch)
    a.human_bin = str(tmp_path / "garbage")  # exits 0 but prints no JSON counts
    Path(a.human_bin).write_text(f"#!{sys.executable}\nprint('segfault?')\n")
    Path(a.human_bin).chmod(0o755)
    monkeypatch.setattr(ins, "call_model", lambda *x, **k: MODEL)
    assert ins.run_names(a, {}) == 2
    man = json.load(open(next((tmp_path / "manifests").glob("names-manifest-*.json"))))
    assert man["import_failed"] == 1


def test_run_names_every_contact_errored_exits_3(tmp_path, monkeypatch):
    a = _args(tmp_path, monkeypatch)
    def boom(*x, **k):
        raise TimeoutError("down")
    monkeypatch.setattr(ins, "call_model", boom)
    assert ins.run_names(a, {}) == 3
    assert not (tmp_path / "fake.json").exists()


def test_run_names_refuses_without_the_binary(tmp_path, monkeypatch):
    a = _args(tmp_path, monkeypatch)
    a.human_bin = str(tmp_path / "missing")
    monkeypatch.setattr(ins, "call_model", lambda *x, **k: pytest.fail("model called"))
    assert ins.run_names(a, {}) == 2


def test_main_refuses_names_with_wide(monkeypatch, capsys):
    def no_persona():
        raise AssertionError("persona touched")
    monkeypatch.setattr(ins, "load_persona", no_persona)
    assert ins.main(["--names", "--population", "wide"]) == 2
    assert "--names" in capsys.readouterr().err
