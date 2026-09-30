"""eval_name_grounding.py (spec 2026-09-29 §4.7): the real composition, off vs live, on a
private copy of graph.db; counts only; refuses (exit 2, nothing written) with < 40
moments, a failed graph copy, a missing binary, or any failed / malformed probe.
Hermetic: synthetic chat rows, temp sqlite, a fake `human` binary. One test runs the
real built `build/human` against a temp graph (HOME pointed at tmp_path) and is
skipped when that binary is absent."""
import datetime as dt
import json
import os
import sqlite3
import stat
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import eval_name_grounding as eg  # noqa: E402

NOW = dt.datetime(2026, 9, 29, 12, 0, tzinfo=dt.timezone.utc)
REAL_BIN = ROOT / "build" / "human"

# FAKE_MODE shapes the answer: fail (exit 1), plain (the non---full header), mismatch
# (header names= disagrees with the block), owner (a contact name plus an "About you:"
# block naming Seth's own place), none ("Memory backend: none", exit 0).
FAKE_GROUND = """#!{py}
import os, sys
assert sys.argv[1:4] == ["memory", "ground", "--full"], sys.argv
assert len(sys.argv) == 6, sys.argv  # the text is ONE argv element
with open(os.environ["FAKE_LOG"], "a") as f:
    f.write(os.environ["HU_GRAPH_DB"] + "\\n")
mode = os.environ.get("FAKE_MODE", "")
if mode == "fail":
    sys.exit(1)
if mode == "none":
    print("Memory backend: none (not configured)")
    sys.exit(0)
hit = os.environ["HU_GRAPH_NAMES"] == "live" and "salim" in sys.argv[5].lower()
if mode == "plain":
    print("matched=0 bytes=0")
elif mode == "owner" and hit:
    block = "- Salim (person)\\nAbout you:\\n- St. Petersburg (place)\\n"
    print(f"matched=1 bytes={{len(block)}} fallback=0 self=1 names=1")
    print(block)
elif hit:
    names = 2 if mode == "mismatch" else 1
    print(f"matched=1 bytes=17 fallback=0 self=0 names={{names}}")
    print("- Salim (person)\\n")
else:
    print("matched=0 bytes=0 fallback=0 self=0 names=0")
"""


def msg(t_hours, text, from_me=False, atype=0):
    return {"t": NOW - dt.timedelta(hours=t_hours), "text": text, "from_me": from_me,
            "atype": atype}


def corpus():
    """6 contacts x 8 inbound; the newest 40 are contacts 0-4, half mention salim."""
    out = {}
    for k in range(6):
        h = f"+1555000{k:04d}"
        out[h] = [msg(k * 8 + j, f"did salim call {k}{j}" if j % 2 == 0 else f"hey {k}{j}")
                  for j in range(8)]
        out[h].append(msg(0.5, "from me", from_me=True))
        out[h].append(msg(0.2, "loved", atype=2000))
    out["12345"] = [msg(0.1, "short code spam")]
    return out


def make_graph(path):
    con = sqlite3.connect(path)
    con.execute("CREATE TABLE entities (id INTEGER PRIMARY KEY, name TEXT)")
    con.commit()
    con.close()


def setup(tmp_path, monkeypatch, per_contact, config=None):
    g = tmp_path / "graph.db"
    make_graph(g)
    fake = tmp_path / "human"
    fake.write_text(FAKE_GROUND.format(py=sys.executable))
    fake.chmod(0o755)
    monkeypatch.setenv("FAKE_LOG", str(tmp_path / "fake.log"))
    monkeypatch.delenv("FAKE_MODE", raising=False)
    monkeypatch.setattr(eg, "load_inbound", lambda *a, **k: per_contact)
    cfg = tmp_path / "absent-config.json"
    if config is not None:
        cfg = tmp_path / "config.json"
        cfg.write_text(json.dumps(config))
    monkeypatch.setattr(eg, "HUMAN_CONFIG", str(cfg))
    monkeypatch.setattr(eg, "_now", lambda: NOW)
    return ["--graph-db", str(g), "--human-bin", str(fake), "--out-dir", str(tmp_path / "out"),
            "--chat-db", str(tmp_path / "chat.db")]


def result_of(tmp_path):
    files = list((tmp_path / "out").glob("name-grounding-*.json"))
    assert len(files) == 1 and stat.S_IMODE(os.stat(files[0]).st_mode) == 0o600
    return json.load(open(files[0]))


def probed_graphs(tmp_path):
    return set(open(tmp_path / "fake.log").read().split())


def assert_refused_and_clean(tmp_path, rc):
    assert rc == 2
    assert not (tmp_path / "out").exists()  # nothing written
    if (tmp_path / "fake.log").exists():
        for p in probed_graphs(tmp_path):
            assert not os.path.exists(p)  # the private copy is gone on refusal too


# ── pure helpers ─────────────────────────────────────────────────────────────


def test_sample_moments_caps_filters_and_orders():
    m = eg.sample_moments(corpus(), NOW, n=40, days=14, per_contact_cap=8)
    assert len(m) == 40
    assert all(h != "12345" for h, _ in m)                   # short code
    assert all(t not in ("from me", "loved") for _, t in m)  # outbound + reactions
    assert m[0] == ("+15550000000", "did salim call 00")     # newest first
    assert {h for h, _ in m} == {f"+1555000{k:04d}" for k in range(5)}
    big = {"+15559999999": [msg(i, f"x{i}") for i in range(20)]}
    capped = eg.sample_moments(big, NOW, n=40, days=14, per_contact_cap=8)
    assert [t for _, t in capped] == [f"x{i}" for i in range(8)]  # the NEWEST 8
    old = {"+15559999999": [msg(24 * 20, "old")]}
    assert eg.sample_moments(old, NOW, n=40, days=14, per_contact_cap=8) == []
    blank = {"+15559999999": [msg(1, "   "), msg(2, None)]}
    assert eg.sample_moments(blank, NOW) == []


def test_sample_moments_excludes_the_loopback_handle_normalized():
    per = {"+15550000001": [msg(1, "a")], "Seth@Example.com": [msg(2, "b")],
           "+15550000002": [msg(3, "c")]}
    m = eg.sample_moments(per, NOW, exclude=["5550000001", "seth@example.com"])
    assert m == [("+15550000002", "c")]


def test_sample_moments_is_deterministic_on_ties():
    a = {"+15550000002": [msg(1, "z"), msg(1, "y")], "+15550000001": [msg(1, "x")]}
    b = dict(reversed(list(a.items())))
    assert eg.sample_moments(a, NOW) == eg.sample_moments(b, NOW) == [
        ("+15550000001", "x"), ("+15550000002", "y"), ("+15550000002", "z")]


def test_typed_names_and_parse_probe():
    b = "- Salim (person)\n  - Salim knows Bob (person)\n- sailboat (topic)\n- Acme (organization)\n"
    assert eg.typed_names(b) == ["Salim", "Acme"]
    assert eg.parse_probe("matched=1 bytes=17 fallback=0 self=0 names=1\n- Salim (person)\n\n") == (
        17, 1, "- Salim (person)\n\n")
    assert eg.parse_probe("matched=0 bytes=0 fallback=1 self=0 names=0\n") == (0, 0, "")
    assert eg.parse_probe("error: nope") is None
    assert eg.parse_probe("matched=0 bytes=0\n") is None                       # plain header
    assert eg.parse_probe("Memory backend: none (not configured)\n") is None
    assert eg.parse_probe("matched=1 bytes=17 fallback=0 self=0 names=1\n- Sal\n") is None
    assert eg.parse_probe("matched=1 bytes=17 fallback=0 self=0 names=1") is None


def test_typed_names_stop_at_the_owner_block():
    """Controller ruling 1: the owner "About you:" block is never a contact name."""
    b = "- Salim (person)\n- sailboat (topic)\nAbout you:\n- St. Petersburg (place)\n"
    assert eg.typed_names(b) == ["Salim"]
    assert eg.typed_names("About you:\n- St. Petersburg (place)\n") == []
    # a line separator inside a name is not a line break for the C counter either
    assert eg.typed_names("- A B (person)\n") == ["A B"]


def test_summarize_counts_typed_blocks_from_the_header():
    res = [(17, 1, "- Salim (person)\n\n"), (40, 0, "- sailboat (topic)\n\n"),
           (0, 0, ""), (17, 1, "- salim (person)\n\n")]
    assert eg.summarize(res) == {"nonempty_blocks": 3, "typed_name_blocks": 2,
                                 "distinct_typed_names": 1, "bytes_total": 74}


# ── end to end on a fake binary ──────────────────────────────────────────────


def test_end_to_end_counts_only_on_a_private_copy(tmp_path, monkeypatch, capsys):
    argv = setup(tmp_path, monkeypatch, corpus())
    assert eg.main(argv) == 0
    r = result_of(tmp_path)
    assert r["n"] == 40
    assert r["modes"]["off"] == {"nonempty_blocks": 0, "typed_name_blocks": 0,
                                 "distinct_typed_names": 0, "bytes_total": 0}
    assert r["modes"]["live"] == {"nonempty_blocks": 20, "typed_name_blocks": 20,
                                  "distinct_typed_names": 1, "bytes_total": 340}
    assert r["gates"] == {"HU_GRAPH_GROUNDING": "live",
                          "HU_GRAPH_GROUNDING_CONTACT_FALLBACK": "live",
                          "HU_GRAPH_GROUNDING_SELF_FACTS": "live"}
    assert r["target_live_typed_name_blocks"] == 15 and r["live_gate_met"] is True
    out = capsys.readouterr()
    for text in (json.dumps(r).lower(), out.out.lower(), out.err.lower()):
        assert "salim" not in text and "5550000" not in text  # counts only
    probed = probed_graphs(tmp_path)
    assert len(probed) == 1 and str(tmp_path / "graph.db") not in probed  # one shared copy
    assert not os.path.exists(probed.pop())  # and the copy is gone
    assert len(open(tmp_path / "fake.log").read().split()) == 80  # 40 moments x 2 modes


def test_gate_flags_reach_the_output(tmp_path, monkeypatch):
    argv = setup(tmp_path, monkeypatch, corpus())
    assert eg.main(argv + ["--fallback", "off", "--self-facts", "shadow"]) == 0
    assert result_of(tmp_path)["gates"]["HU_GRAPH_GROUNDING_CONTACT_FALLBACK"] == "off"
    assert result_of(tmp_path)["gates"]["HU_GRAPH_GROUNDING_SELF_FACTS"] == "shadow"


def test_owner_block_names_never_count(tmp_path, monkeypatch):
    """Ruling 1 end to end: 'About you:' names Seth's own place; only Salim counts."""
    argv = setup(tmp_path, monkeypatch, corpus())
    monkeypatch.setenv("FAKE_MODE", "owner")
    assert eg.main(argv) == 0
    live = result_of(tmp_path)["modes"]["live"]
    assert live["typed_name_blocks"] == 20 and live["distinct_typed_names"] == 1


def test_gate_not_met_below_target_or_off_n(tmp_path, monkeypatch):
    few_hits = {f"+1555000{k:04d}": [msg(k * 8 + j, "did salim call" if j == 0 else f"hey {j}")
                                     for j in range(8)] for k in range(6)}
    argv = setup(tmp_path, monkeypatch, few_hits)
    assert eg.main(argv) == 0
    r = result_of(tmp_path)
    assert r["modes"]["live"]["typed_name_blocks"] == 5 and r["live_gate_met"] is False


def test_gate_never_met_when_n_is_not_40(tmp_path, monkeypatch):
    argv = setup(tmp_path, monkeypatch, corpus())
    assert eg.main(argv + ["--n", "30"]) == 0
    r = result_of(tmp_path)
    assert r["n"] == 30 and r["modes"]["live"]["typed_name_blocks"] >= 15
    assert r["live_gate_met"] is False


def test_loopback_handle_from_config_is_not_sampled(tmp_path, monkeypatch):
    cfg = {"channels": {"imessage": {"loopback_handle": "+15550000000"}}}
    argv = setup(tmp_path, monkeypatch, corpus(), config=cfg)
    assert eg.main(argv) == 0
    # contact 0 (4 salim texts) is excluded; contacts 1-5 fill the 40, 20 mention salim
    assert result_of(tmp_path)["modes"]["live"]["typed_name_blocks"] == 20


# ── refusals: exit 2, nothing written, no copy left behind ───────────────────


def test_refuses_with_fewer_than_40_moments(tmp_path, monkeypatch):
    few = {"+15550000001": [msg(i, f"hi {i}") for i in range(5)]}
    argv = setup(tmp_path, monkeypatch, few)
    assert_refused_and_clean(tmp_path, eg.main(argv))
    assert not (tmp_path / "fake.log").exists()  # no probe ran


@pytest.mark.parametrize("mode", ["fail", "plain", "mismatch", "none"])
def test_refuses_when_a_probe_fails_or_breaks_the_contract(tmp_path, monkeypatch, capsys, mode):
    argv = setup(tmp_path, monkeypatch, corpus())
    monkeypatch.setenv("FAKE_MODE", mode)
    assert_refused_and_clean(tmp_path, eg.main(argv))
    err = capsys.readouterr().err.lower()
    assert "refusing" in err and "salim" not in err and "5550000" not in err


def test_refuses_when_a_probe_times_out(tmp_path, monkeypatch):
    argv = setup(tmp_path, monkeypatch, corpus())

    def slow(*a, **k):
        raise subprocess.TimeoutExpired(cmd="human", timeout=1)
    monkeypatch.setattr(eg.subprocess, "run", slow)
    assert_refused_and_clean(tmp_path, eg.main(argv))


def test_refuses_without_the_binary(tmp_path, monkeypatch):
    argv = setup(tmp_path, monkeypatch, corpus())
    argv[argv.index("--human-bin") + 1] = str(tmp_path / "missing")
    assert_refused_and_clean(tmp_path, eg.main(argv))


def test_refuses_a_non_executable_binary(tmp_path, monkeypatch):
    argv = setup(tmp_path, monkeypatch, corpus())
    (tmp_path / "human").chmod(0o644)
    assert_refused_and_clean(tmp_path, eg.main(argv))


@pytest.mark.parametrize("graph", ["missing", "not-sqlite", "no-entities"])
def test_refuses_when_the_graph_copy_fails(tmp_path, monkeypatch, graph):
    argv = setup(tmp_path, monkeypatch, corpus())
    g = tmp_path / "bad.db"
    if graph == "not-sqlite":
        g.write_text("this is not a database, just text padding " * 100)
    elif graph == "no-entities":
        con = sqlite3.connect(g)
        con.execute("CREATE TABLE other (x)")
        con.commit(); con.close()
    argv[argv.index("--graph-db") + 1] = str(g)
    assert_refused_and_clean(tmp_path, eg.main(argv))
    assert not (tmp_path / "fake.log").exists()  # no probe ran
    if graph == "missing":
        assert not g.exists()  # a read-only copy never creates the source


def test_refuses_a_malformed_config(tmp_path, monkeypatch):
    argv = setup(tmp_path, monkeypatch, corpus())
    bad = tmp_path / "bad-config.json"
    bad.write_text("{not json")
    monkeypatch.setattr(eg, "HUMAN_CONFIG", str(bad))
    assert_refused_and_clean(tmp_path, eg.main(argv))


def test_copy_is_deleted_when_probing_raises(tmp_path, monkeypatch):
    argv = setup(tmp_path, monkeypatch, corpus())
    seen = []

    def boom(human_bin, graph_copy, *a, **k):
        seen.append(graph_copy)
        assert os.path.exists(graph_copy)
        assert stat.S_IMODE(os.stat(graph_copy).st_mode) == 0o600
        raise RuntimeError("probe blew up")
    monkeypatch.setattr(eg, "probe", boom)
    with pytest.raises(RuntimeError):
        eg.main(argv)
    assert seen and not os.path.exists(seen[0]) and not os.path.exists(os.path.dirname(seen[0]))
    assert not (tmp_path / "out").exists()


def test_source_graph_is_never_written(tmp_path, monkeypatch):
    argv = setup(tmp_path, monkeypatch, corpus())
    g = tmp_path / "graph.db"
    before = g.read_bytes()
    assert eg.main(argv) == 0
    assert g.read_bytes() == before


# ── chat.db reader (temp fixture, never the real chat.db) ────────────────────


def test_load_inbound_reads_1to1_rows_and_skips_groups(tmp_path):
    chat = tmp_path / "chat.db"
    con = sqlite3.connect(chat)
    con.execute("CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT)")
    con.execute("CREATE TABLE message (ROWID INTEGER PRIMARY KEY, guid TEXT, text TEXT, "
                "attributedBody BLOB, handle_id INTEGER, is_from_me INTEGER, date INTEGER, "
                "cache_roomnames TEXT, associated_message_guid TEXT, "
                "associated_message_type INTEGER)")
    con.execute("INSERT INTO handle VALUES (1, '+15550000001')")
    ns = lambda h: int((NOW - dt.timedelta(hours=h) - dt.datetime(  # noqa: E731
        2001, 1, 1, tzinfo=dt.timezone.utc)).total_seconds() * 1e9)
    con.execute("INSERT INTO message VALUES (1, 'g1', 'one on one', NULL, 1, 0, ?, NULL, "
                "NULL, 0)", (ns(1),))
    con.execute("INSERT INTO message VALUES (2, 'g2', 'group text', NULL, 1, 0, ?, 'chat9', "
                "NULL, 0)", (ns(2),))
    con.commit(); con.close()
    per = eg.load_inbound(str(chat), NOW - dt.timedelta(days=14))
    assert eg.sample_moments(per, NOW) == [("+15550000001", "one on one")]


# ── the real binary (skipped when build/human is absent) ─────────────────────


@pytest.mark.skipif(not os.access(REAL_BIN, os.X_OK), reason="build/human not built")
def test_real_binary_off_vs_live_on_a_temp_graph(tmp_path, monkeypatch):
    home = tmp_path / "home"
    (home / ".human").mkdir(parents=True)
    (home / ".human" / "config.json").write_text(json.dumps(
        {"memory": {"backend": "sqlite", "sqlite_path": str(tmp_path / "mem.db")}}))
    monkeypatch.setenv("HOME", str(home))
    for var in ("HU_GRAPH_NAMES", "HU_GRAPH_DB", "HU_MEMORY_SQLITE_PATH"):
        monkeypatch.delenv(var, raising=False)
    graph = tmp_path / "graph.db"
    jl = tmp_path / "e.jsonl"
    jl.write_text("".join(json.dumps(
        {"kind": "entity", "contact": f"+1555000{k:04d}", "name": "Salim", "type": "person",
         "source": "names:nightly", "confidence": 0.8}) + "\n" for k in range(6)))
    r = subprocess.run([str(REAL_BIN), "memory", "import-facts", str(jl)],
                       env={**os.environ, "HU_GRAPH_DB": str(graph)}, capture_output=True)
    assert r.returncode == 0, r.stderr
    monkeypatch.setattr(eg, "load_inbound", lambda *a, **k: corpus())
    monkeypatch.setattr(eg, "HUMAN_CONFIG", str(tmp_path / "absent.json"))
    monkeypatch.setattr(eg, "_now", lambda: NOW)
    before = graph.read_bytes()
    base = ["--graph-db", str(graph), "--human-bin", str(REAL_BIN), "--chat-db",
            str(tmp_path / "chat.db")]
    counts = {}
    for label, extra in (("prod", []), ("nofb", ["--fallback", "off"])):
        out = tmp_path / label
        assert eg.main(base + ["--out-dir", str(out)] + extra) == 0
        (f,) = out.glob("name-grounding-*.json")
        counts[label] = json.load(open(f))["modes"]
    # prod gates: the contact fallback grounds every miss in the contact's typed person
    assert counts["prod"]["live"]["typed_name_blocks"] == 40
    # fallback off: only the 20 texts that say "salim" match, in both modes
    for mode in ("off", "live"):
        assert counts["nofb"][mode]["typed_name_blocks"] == 20
        assert counts["nofb"][mode]["nonempty_blocks"] == 20
        assert counts["nofb"][mode]["distinct_typed_names"] == 1
    assert graph.read_bytes() == before
    assert not (tmp_path / "mem.db").exists()  # the probe stayed off the configured memory.db
