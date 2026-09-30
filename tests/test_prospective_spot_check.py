"""Hermetic tests for scripts/prospective_spot_check.py (spec 2026-09-30 §3 promotion).

A synthetic log and memory.db in tmp_path; nothing reads ~/.human, no model, no
network, no chat.db -- HOME is pointed at a scratch dir by the test runner.
"""
import csv
import json
import os
import sqlite3
import stat
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

import prospective_spot_check as psc  # noqa: E402


@pytest.fixture(autouse=True)
def _hermetic_addressbook(monkeypatch):
    """Never touch the real macOS AddressBook from a test (rule: hermetic tests
    only). resolve_contact_name_tokens() (scripts/blind_ab/make_rating_sheet.py)
    honors this env var itself and returns an empty token set without reading
    the AddressBook. A test that needs a SPECIFIC name list instead monkeypatches
    psc.resolve_contact_name_tokens directly (see
    test_build_resolves_and_redacts_contact_names below)."""
    monkeypatch.setenv("HU_BLIND_AB_SKIP_ADDRESSBOOK", "1")


def make_db(tmp_path, n):
    p = tmp_path / "memory.db"
    con = sqlite3.connect(p)
    con.execute("CREATE TABLE prospective_memories(id INTEGER PRIMARY KEY, trigger_type TEXT, "
                "trigger_value TEXT, action TEXT, contact_id TEXT, created_at INTEGER)")
    con.execute("CREATE TABLE messages(id INTEGER PRIMARY KEY, session_id TEXT, role TEXT, "
                "content TEXT, created_at TEXT)")
    for i in range(1, 2 * n + 1):
        con.execute("INSERT INTO prospective_memories VALUES(?, 'keyword', 'cue', ?, ?, 0)",
                    (i, f"action {i}", f"+1555000{i:04d}"))
        con.execute("INSERT INTO messages(session_id,role,content,created_at) VALUES(?, 'user', "
                    "?, '2026-10-01 00:00:00')", (f"+1555000{i:04d}", f"hello {i}"))
    con.commit()
    con.close()
    return str(p)


def make_log(tmp_path, n):
    lines = []
    for i in range(1, 2 * n + 1):
        verdict = "fire" if i <= n else "not_now"
        lines.append(f"2026-10-02T12:{i % 60:02d}:00 INFO  [prospective] prospective shadow "
                     f"item: id={i} verdict={verdict}")
    p = tmp_path / "service.log"
    p.write_text("\n".join(lines) + "\n")
    return str(p)


def build(tmp_path, n, seed=7):
    out = tmp_path / "sheet"
    rc = psc.main(["build", "--log", make_log(tmp_path, n), "--memory-db", make_db(tmp_path, n),
                   "--since", "2026-10-01", "--until", "2026-10-03", "--out-dir", str(out),
                   "--seed", str(seed)])
    return rc, out


def test_build_mixes_fire_and_hold_blind(tmp_path):
    rc, out = build(tmp_path, 30)
    assert rc == 0
    assert stat.S_IMODE(out.stat().st_mode) == 0o700
    rows = list(csv.DictReader(open(out / "rating_sheet.csv")))
    key = json.loads((out / "answer_key.json").read_text())
    # answer_key.json also carries private "_"-prefixed metadata (seed, per-class
    # counts -- see test_readme_and_key_keep_seed_and_split_private below); only
    # the non-underscore entries are the {id: "fire"|"hold"} map.
    labels = {k: v for k, v in key.items() if not k.startswith("_")}
    assert len(rows) == 60 and sorted(set(labels.values())) == ["fire", "hold"]
    assert set(rows[0]) == {"id", "context", "reminder", "answer"}
    assert "verdict" not in (out / "rating_sheet.csv").read_text()
    for f in ("rating_sheet.csv", "answer_key.json", "README.md"):
        assert stat.S_IMODE((out / f).stat().st_mode) == 0o600


def test_build_refuses_below_thirty_would_fires(tmp_path):
    rc, out = build(tmp_path, 29)
    assert rc == 2 and not out.exists()


# ---------------------------------------------------------------------------
# Fix round I1 (prospective_shadow_report.PREFIX, reused here via psr.read_log):
# a daemon-restart gate-banner line (hu_prospective_gate_banner,
# src/memory/prospective_policy.c) names "shadow" in its text just like the
# fire/not_now item lines make_log() writes. Before the PREFIX fix, the
# banner was mis-treated as a shadow-shaped candidate, failed every known
# shape, and refused the build ("N malformed shadow log line(s) in the
# window") on any window that happened to contain a daemon restart.
# ---------------------------------------------------------------------------

def test_build_ignores_gate_banner_line_in_log(tmp_path):
    log_path = make_log(tmp_path, 30)
    with open(log_path, "a") as f:
        f.write("2026-10-02T12:00:11 INFO  [prospective] prospective SHADOW "
                "(HU_PROSPECTIVE=shadow): legacy directive unchanged; Filter+Decide "
                "counts logged\n")
    out = tmp_path / "sheet"
    rc = psc.main(["build", "--log", log_path, "--memory-db", make_db(tmp_path, 30),
                   "--since", "2026-10-01", "--until", "2026-10-03", "--out-dir", str(out),
                   "--seed", "7"])
    assert rc == 0
    rows = list(csv.DictReader(open(out / "rating_sheet.csv")))
    assert len(rows) == 60


def test_score_precision_and_refusal(tmp_path):
    rc, out = build(tmp_path, 30)
    key = json.loads((out / "answer_key.json").read_text())
    rows = list(csv.DictReader(open(out / "rating_sheet.csv")))
    fire_ids = [r["id"] for r in rows if key[r["id"]] == "fire"]
    for r in rows:  # 25 of 30 would-fires judged right; every hold rated no
        r["answer"] = "yes" if r["id"] in fire_ids[:25] else "no"
    with open(out / "rating_sheet.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["id", "context", "reminder", "answer"])
        w.writeheader()
        w.writerows(rows)
    res = psc.score_sheet(str(out))
    assert res["fire_rated"] == 30 and res["fire_yes"] == 25
    assert abs(res["precision"] - 25 / 30) < 1e-9 and res["pass"] is True
    assert res["hold_yes"] == 0
    for r in rows[:40]:
        r["answer"] = ""
    with open(out / "rating_sheet.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["id", "context", "reminder", "answer"])
        w.writeheader()
        w.writerows(rows)
    assert psc.main(["score", "--out-dir", str(out)]) == 2  # fewer than 30 answered would-fires


# ---------------------------------------------------------------------------
# Additional coverage beyond the spec brief: privacy refusals, dedup, and the
# unanswered-vs-no / null-precision edges of score_sheet.
# ---------------------------------------------------------------------------

def test_build_refuses_out_dir_inside_repo(tmp_path):
    candidate = os.path.join(psc.REPO_ROOT, "zz_prospective_spot_check_should_not_exist")
    assert not os.path.exists(candidate)
    rc = psc.main(["build", "--log", make_log(tmp_path, 30), "--memory-db", make_db(tmp_path, 30),
                   "--since", "2026-10-01", "--until", "2026-10-03",
                   "--out-dir", candidate, "--seed", "1"])
    assert rc == 2
    assert not os.path.exists(candidate)  # refused before creating anything


def test_build_refuses_symlink_out_dir(tmp_path):
    real_target = tmp_path / "real_target"
    real_target.mkdir()
    link_path = tmp_path / "link_out"
    link_path.symlink_to(real_target)
    rc = psc.main(["build", "--log", make_log(tmp_path, 30), "--memory-db", make_db(tmp_path, 30),
                   "--since", "2026-10-01", "--until", "2026-10-03",
                   "--out-dir", str(link_path), "--seed", "1"])
    assert rc == 2
    assert list(real_target.iterdir()) == []  # nothing written through the symlink


def test_build_sheet_never_contains_raw_contact_handles(tmp_path):
    rc, out = build(tmp_path, 30)
    assert rc == 0
    sheet_text = (out / "rating_sheet.csv").read_text()
    readme_text = (out / "README.md").read_text()
    key_text = (out / "answer_key.json").read_text()
    for i in range(1, 61):
        handle = f"+1555000{i:04d}"
        assert handle not in sheet_text
        assert handle not in readme_text
        assert handle not in key_text


def test_build_redacts_raw_contact_handle_from_message_text(tmp_path):
    n = 30
    phone = "+15550001234"
    db_path = tmp_path / "memory_redact.db"
    con = sqlite3.connect(db_path)
    con.execute("CREATE TABLE prospective_memories(id INTEGER PRIMARY KEY, trigger_type TEXT, "
                "trigger_value TEXT, action TEXT, contact_id TEXT, created_at INTEGER)")
    con.execute("CREATE TABLE messages(id INTEGER PRIMARY KEY, session_id TEXT, role TEXT, "
                "content TEXT, created_at TEXT)")
    for i in range(1, 2 * n + 1):
        contact = phone if i == 1 else f"+1555000{i:04d}"
        content = f"call me back at {phone}" if i == 1 else f"hello {i}"
        con.execute("INSERT INTO prospective_memories VALUES(?, 'keyword', 'cue', ?, ?, 0)",
                    (i, f"action {i}", contact))
        con.execute("INSERT INTO messages(session_id,role,content,created_at) VALUES(?, 'user', "
                    "?, '2026-10-01 00:00:00')", (contact, content))
    con.commit()
    con.close()

    out = tmp_path / "sheet_redact"
    rc = psc.main(["build", "--log", make_log(tmp_path, n), "--memory-db", str(db_path),
                   "--since", "2026-10-01", "--until", "2026-10-03",
                   "--out-dir", str(out), "--seed", "3"])
    assert rc == 0

    digits = "15550001234"
    sheet_text = (out / "rating_sheet.csv").read_text()
    readme_text = (out / "README.md").read_text()
    key_text = (out / "answer_key.json").read_text()
    assert digits not in sheet_text
    assert digits not in readme_text
    assert digits not in key_text
    # Proves redact() actually transformed the text (not merely that the number
    # never appeared): the placeholder must be present where the phone was.
    assert "[phone]" in sheet_text


def test_collapse_to_intentions_dedupes_repeated_passes_by_latest_verdict():
    meta = {1: ("c1", "call back")}
    rows = [
        {"ts": 100, "time": False, "kind": "item", "kv": {"id": "1", "verdict": "not_now"}},
        {"ts": 200, "time": False, "kind": "item", "kv": {"id": "1", "verdict": "fire"}},
    ]
    result = psc.collapse_to_intentions(rows, meta)
    assert result == {("c1", "call back"): (200, "fire", 1)}


def test_collapse_to_intentions_dedupes_multiple_ids_sharing_one_intention():
    meta = {1: ("c1", "call back"), 2: ("c1", "call back")}
    rows = [
        {"ts": 100, "time": False, "kind": "item", "kv": {"id": "1", "verdict": "fire"}},
        {"ts": 200, "time": False, "kind": "item", "kv": {"id": "2", "verdict": "not_now"}},
    ]
    result = psc.collapse_to_intentions(rows, meta)
    assert len(result) == 1
    assert result[("c1", "call back")] == (200, "not_now", 2)


def test_score_non_yes_no_answer_is_unanswered_not_no(tmp_path):
    rc, out = build(tmp_path, 30)
    key = json.loads((out / "answer_key.json").read_text())
    rows = list(csv.DictReader(open(out / "rating_sheet.csv")))
    for r in rows:
        r["answer"] = "yes" if key[r["id"]] == "fire" else "maybe"  # garbage, never "no"
    with open(out / "rating_sheet.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["id", "context", "reminder", "answer"])
        w.writeheader()
        w.writerows(rows)
    res = psc.score_sheet(str(out))
    assert res["fire_rated"] == 30 and res["fire_yes"] == 30
    # If "maybe" had been folded into "no", hold_rated would read 30 and
    # hold_yes_rate would read 0.0 instead of None.
    assert res["hold_rated"] == 0 and res["hold_yes"] == 0
    assert res["hold_yes_rate"] is None
    assert res["precision"] == 1.0 and res["pass"] is True


def test_score_precision_is_null_with_zero_fire_rated(tmp_path):
    rc, out = build(tmp_path, 30)
    assert rc == 0
    res = psc.score_sheet(str(out))  # freshly built: every `answer` column is blank
    assert res["fire_rated"] == 0
    assert res["precision"] is None
    assert res["pass"] is False
    assert psc.main(["score", "--out-dir", str(out)]) == 2


# ---------------------------------------------------------------------------
# Fix round 1: blindness (seed/split must never reach a rater-facing artifact)
# and the name-token resolver (build() must call it and actually redact names).
# ---------------------------------------------------------------------------

def test_readme_and_key_keep_seed_and_split_private(tmp_path):
    """CRITICAL fix: README.md must never carry the shuffle seed or the
    fire/hold split -- a rater with both can replay Fisher-Yates
    (random.Random(seed).shuffle(list(range(n)))) and invert it to recover
    every row's pre-shuffle (fire-block vs hold-block) position, deanonymizing
    the sheet without ever opening answer_key.json. Both facts must instead
    live ONLY in the private, 0600 answer_key.json."""
    seed = 918273  # distinctive: won't collide with any other number README prints
    rc, out = build(tmp_path, 30, seed=seed)
    assert rc == 0
    readme_text = (out / "README.md").read_text()
    assert str(seed) not in readme_text
    # The old leaky phrasing named both class counts together in one sentence
    # ("{fire} SHADOW would-fires, {hold} held items"); that sentence must be
    # gone. (README legitimately still uses the words "fire"/"held" elsewhere,
    # e.g. describing what the score command measures -- that is vocabulary,
    # not this sheet's actual split, so it is not banned outright.)
    assert "would-fires," not in readme_text
    assert "held items" not in readme_text

    key = json.loads((out / "answer_key.json").read_text())
    assert key["_seed"] == seed
    assert key["_fire_count"] == 30
    assert key["_hold_count"] == 30


def test_readme_seed_absence_defeats_permutation_reconstruction(tmp_path):
    """Demonstrates the actual exploit the CRITICAL fix closes: given the seed
    and the row count, an attacker can reconstruct the exact Fisher-Yates
    permutation and therefore the pre-shuffle (fire-block/hold-block) index of
    every shuffled row -- so the seed alone (with a public, fixed pre-shuffle
    fire-then-hold order) is sufficient to deanonymize. Confirms this sheet's
    README carries no usable seed for that reconstruction."""
    import random as _random

    seed = 42424242
    rc, out = build(tmp_path, 30, seed=seed)
    assert rc == 0
    n = len(list(csv.DictReader(open(out / "rating_sheet.csv"))))

    # The exploit: replay the exact permutation build() used.
    order = list(range(n))
    _random.Random(seed).shuffle(order)
    assert len(order) == n  # the reconstruction is real and well-formed

    # The defense: README (the only rater-facing artifact besides the CSV
    # itself) never named this seed, so a rater cannot perform the replay above.
    readme_text = (out / "README.md").read_text()
    assert str(seed) not in readme_text


def test_build_resolves_and_redacts_contact_names(tmp_path, monkeypatch):
    """IMPORTANT fix: build() must resolve contact name tokens (the same way
    scripts/blind_ab/make_rating_sheet.py does) and pass them to EVERY
    redact() call, so a real name typed into a message body (not just a
    phone number) is scrubbed before it reaches rating_sheet.csv."""
    calls = []

    def fake_resolver():
        calls.append(1)
        return {"John", "Smith"}

    monkeypatch.setattr(psc, "resolve_contact_name_tokens", fake_resolver)

    n = 30
    db_path = tmp_path / "memory_names.db"
    con = sqlite3.connect(db_path)
    con.execute("CREATE TABLE prospective_memories(id INTEGER PRIMARY KEY, trigger_type TEXT, "
                "trigger_value TEXT, action TEXT, contact_id TEXT, created_at INTEGER)")
    con.execute("CREATE TABLE messages(id INTEGER PRIMARY KEY, session_id TEXT, role TEXT, "
                "content TEXT, created_at TEXT)")
    for i in range(1, 2 * n + 1):
        contact = f"+1555000{i:04d}"
        content = "call John Smith back" if i == 1 else f"hello {i}"
        con.execute("INSERT INTO prospective_memories VALUES(?, 'keyword', 'cue', ?, ?, 0)",
                    (i, f"action {i}", contact))
        con.execute("INSERT INTO messages(session_id,role,content,created_at) VALUES(?, 'user', "
                    "?, '2026-10-01 00:00:00')", (contact, content))
    con.commit()
    con.close()

    out = tmp_path / "sheet_names"
    rc = psc.main(["build", "--log", make_log(tmp_path, n), "--memory-db", str(db_path),
                   "--since", "2026-10-01", "--until", "2026-10-03",
                   "--out-dir", str(out), "--seed", "9"])
    assert rc == 0
    assert calls, "resolve_contact_name_tokens was never invoked by build() -- " \
                  "a regression back to a no-arg redact() call would show up here"

    sheet_text = (out / "rating_sheet.csv").read_text()
    readme_text = (out / "README.md").read_text()
    key_text = (out / "answer_key.json").read_text()
    for leaked in ("John", "Smith"):
        assert leaked not in sheet_text
        assert leaked not in readme_text
        assert leaked not in key_text
    # Proves the tokens actually reached redact() and transformed the text
    # (not merely that "John Smith" never appeared some other way).
    assert "[name]" in sheet_text


def test_build_without_seed_still_records_a_reproducible_seed(tmp_path):
    out = tmp_path / "sheet"
    rc = psc.main(["build", "--log", make_log(tmp_path, 30), "--memory-db", make_db(tmp_path, 30),
                   "--since", "2026-10-01", "--until", "2026-10-03", "--out-dir", str(out)])
    assert rc == 0
    key = json.loads((out / "answer_key.json").read_text())
    assert isinstance(key["_seed"], int)  # never None: the shuffle must be replayable from the key
    assert str(key["_seed"]) not in (out / "README.md").read_text()
