"""--population wide: final whole-branch review fixes (R12).

Hermetic like test_insight_stream_wide.py: MEMORY_DB, CURATOR_STATE and
HUMAN_CONFIG point into tmp_path (autouse fixture imported below); the model,
verification, chat.db attribution and the health probe are monkeypatched.
"""
import datetime as dt
import json
import os
import sqlite3
import sys
import urllib.error
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
sys.path.insert(0, str(Path(__file__).parent))
import insight_stream as ins  # noqa: E402
from test_insight_stream_wide import A, H, NOW, timeline, _hermetic_memory_db  # noqa: E402,F401

H2 = "+15550000099"
LABELS = {f"me{i}": "seth" for i in range(6)}
GOOD = {"note": "Priya surgery tuesday", "kind": "plan", "confidence": 0.9,
        "evidence": ["t0"], "names": [{"name": "Priya", "type": "person"}]}
DAEMON = {"note": "Marcus says hi", "evidence": ["d0"], "names": []}
INVENTED = {"note": "Dana visiting", "evidence": ["t1"], "names": [{"name": "Dana", "type": "person"}]}
INT_COUNTERS = ("notes_kept", "rejected_verification", "names_proposed", "names_rejected",
                "model_errors", "parse_failed", "skipped_min_turns")


def _db(tmp_path):
    db = sqlite3.connect(tmp_path / "m.db")
    db.executescript(ins.SCHEMA)
    ins.migrate(db)
    return db


def _stub_run(tmp_path, monkeypatch, timelines, write=True, deadline=None):
    chat = tmp_path / "chat.db"
    c = sqlite3.connect(chat)
    c.execute("CREATE TABLE message (rowid INTEGER)")
    c.commit(); c.close()
    monkeypatch.setattr(ins.urllib.request, "urlopen", lambda *a, **k: None)
    import eval_conversation_quality as cq
    monkeypatch.setattr(cq, "attribute", lambda *a, **k: {"timelines": timelines, "labels": LABELS})
    a = A()
    a.chat_db = str(chat); a.window_days = 30; a.deadline = deadline
    a.manifest_dir = str(tmp_path / "manifests"); a.write = write
    return a


def _suppress(handle):
    mem = sqlite3.connect(ins.MEMORY_DB)
    mem.execute("CREATE TABLE IF NOT EXISTS contact_suppressions (contact TEXT)")
    mem.execute("INSERT INTO contact_suppressions VALUES (?)", (handle,))
    mem.commit(); mem.close()


def _manifest(a):
    files = list(Path(a.manifest_dir).glob("curator-manifest-*.json"))
    assert len(files) == 1, files
    return json.loads(files[0].read_text())


def _raises(exc):
    def f(*a, **k):
        raise exc
    return f


# ---- C1: one contact's model error must not abort the run -----------------

def test_model_error_on_one_contact_does_not_abort_the_run(tmp_path, monkeypatch):
    db = _db(tmp_path)
    att = {"timelines": {H: timeline(), H2: timeline()}, "labels": LABELS}

    def cm(url, model, system, user, **k):
        if H in system:  # H sorts first, so the error comes before the good contact
            raise urllib.error.URLError("timed out")
        return json.dumps([GOOD])
    monkeypatch.setattr(ins, "call_model", cm)
    monkeypatch.setattr(ins, "verify_claims", lambda a, s, u, claims, k: [1] * len(claims))
    state = {}
    man = ins.wide_pass(db, A(), "id", set(), att, NOW, write=True, state=state)
    assert man["model_errors"] == 1 and man["curated"] == 1
    assert list(state) == [H2]  # only the contact that completed is marked done
    assert db.execute("SELECT contact_id FROM contact_insights").fetchall() == [(H2,)]


def test_verification_error_counts_once_and_leaves_no_partial_counts(tmp_path, monkeypatch):
    db = _db(tmp_path)
    att = {"timelines": {H: timeline()}, "labels": {**LABELS, "bot": "huuman"}}
    monkeypatch.setattr(ins, "call_model", lambda *a, **k: json.dumps([GOOD, DAEMON]))
    monkeypatch.setattr(ins, "verify_claims", _raises(TimeoutError("verify timed out")))
    state = {}
    man = ins.wide_pass(db, A(), "id", set(), att, NOW, write=True, state=state)
    assert man["model_errors"] == 1 and man["curated"] == 0
    assert man["rejected_daemon_evidence"] == 0 and man["names_proposed"] == 0
    assert state == {}


def test_run_wide_exits_3_when_every_contact_errors_but_keeps_the_evidence(tmp_path, monkeypatch,
                                                                          capsys):
    db = _db(tmp_path)
    a = _stub_run(tmp_path, monkeypatch, {H: timeline(), H2: timeline()})
    monkeypatch.setattr(ins, "call_model", _raises(urllib.error.URLError("down")))
    assert ins.run_wide(a, db, "id", set(), 1) == 3
    man = _manifest(a)
    assert man["model_errors"] == 2 and man["curated"] == 0
    assert json.loads(Path(ins.CURATOR_STATE).read_text()) == {}
    assert "model error" in capsys.readouterr().err


# ---- m3: state load tolerates undecodable bytes and non-int values ---------

def test_load_curator_state_tolerates_bad_bytes_and_drops_non_int(tmp_path, monkeypatch):
    p = tmp_path / "st.json"
    monkeypatch.setattr(ins, "CURATOR_STATE", str(p))
    p.write_bytes(b"\xff\xfe{\x00")  # UnicodeDecodeError, a ValueError but not a JSONDecodeError
    assert ins.load_curator_state() == {}
    p.write_text(json.dumps({"+1a": 5, "+1b": "x", "+1c": 2.5, "+1d": True, "+1e": None}))
    assert ins.load_curator_state() == {"+1a": 5}


# ---- I5: a missed window is closed, not rolled to a 22-hour run -------------

def test_run_wide_window_closed_writes_nothing(tmp_path, monkeypatch, capsys):
    db = _db(tmp_path)
    db.execute("INSERT INTO contact_insights (contact_id, kind, insight, confidence, as_of_ms,"
               " source, created_at_ms) VALUES (?, 'fact', 'old note', 0.9, 1, 'extractor:v2', 1)",
               (H,))
    db.commit()
    _suppress(H)
    a = _stub_run(tmp_path, monkeypatch, {H: timeline(), H2: timeline()}, deadline="07:30")
    tz = dt.timezone(dt.timedelta(hours=-4))
    monkeypatch.setattr(ins, "_local_now", lambda: dt.datetime(2026, 9, 28, 9, 0, tzinfo=tz))
    monkeypatch.setattr(ins, "call_model", _raises(AssertionError("model called")))
    assert ins.run_wide(a, db, "id", set(), 1) == 0
    assert "window closed" in capsys.readouterr().err
    assert db.execute("SELECT retired_at_ms FROM contact_insights").fetchone()[0] == 0
    assert not Path(ins.CURATOR_STATE).exists()
    assert not Path(a.manifest_dir).exists()


# ---- I1: wide calls ask for more tokens; unparseable output is counted ------

def test_wide_pass_asks_for_1500_tokens(tmp_path, monkeypatch):
    seen = []

    def cm(*a, **k):
        seen.append(k.get("max_tokens"))
        return "[]"
    monkeypatch.setattr(ins, "call_model", cm)
    ins.wide_pass(_db(tmp_path), A(), "id", set(), {"timelines": {H: timeline()}, "labels": LABELS},
                  NOW, write=False)
    assert seen == [1500]


@pytest.mark.parametrize("raw,failed", [("sorry, I can't help with that", 1),
                                        ('[{"note": ""}]', 1), ("[]", 0), ("", 0)])
def test_parse_failed_counts_nonempty_output_that_yields_no_notes(tmp_path, monkeypatch, raw,
                                                                  failed):
    monkeypatch.setattr(ins, "call_model", lambda *a, **k: raw)
    man = ins.wide_pass(_db(tmp_path), A(), "id", set(),
                        {"timelines": {H: timeline()}, "labels": LABELS}, NOW, write=False)
    assert man["parse_failed"] == failed and man["curated"] == 1


# ---- I2(a): the verifier only sees citable human rows ----------------------

def test_verification_context_has_only_citable_rows(tmp_path, monkeypatch):
    seen = {}
    monkeypatch.setattr(ins, "call_model", lambda *a, **k: json.dumps([GOOD]))

    def vc(a, system, context, claims, k):
        seen["ctx"] = context
        return [1] * len(claims)
    monkeypatch.setattr(ins, "verify_claims", vc)
    att = {"timelines": {H: timeline()}, "labels": {**LABELS, "bot": "huuman"}}
    ins.wide_pass(_db(tmp_path), A(), "id", set(), att, NOW, write=False)
    assert "[t0] them: priya's surgery is tuesday" in seen["ctx"]
    assert "[d0]" not in seen["ctx"] and "Marcus says hi" not in seen["ctx"]


# ---- I3: validate first, verify only survivors -----------------------------

def _verify_driver(monkeypatch, notes, supported=True):
    calls = []

    def cm(url, model, system, user, **k):
        calls.append(user)
        if len(calls) == 1:
            return json.dumps(notes)
        return json.dumps([{"i": i, "supported": supported} for i in range(8)])
    monkeypatch.setattr(ins, "call_model", cm)
    return calls


def test_invalid_notes_are_dropped_before_verification(tmp_path, monkeypatch):
    calls = _verify_driver(monkeypatch, [GOOD, DAEMON, INVENTED])
    a = A(); a.consistency_k = 3
    att = {"timelines": {H: timeline()}, "labels": {**LABELS, "bot": "huuman"}}
    man = ins.wide_pass(_db(tmp_path), a, "id", set(), att, NOW, write=True)
    assert len(calls) == 1 + 3  # one generation, K verification passes
    for user in calls[1:]:
        candidates = user.split("candidate notes:\n", 1)[1]
        assert candidates == "[0] Priya surgery tuesday"
    assert man["notes_kept"] == 1 and man["notes_written"] == 1
    assert man["rejected_verification"] == 0


def test_all_invalid_notes_cost_no_verification_calls(tmp_path, monkeypatch):
    calls = _verify_driver(monkeypatch, [DAEMON, INVENTED])
    a = A(); a.consistency_k = 3
    att = {"timelines": {H: timeline()}, "labels": {**LABELS, "bot": "huuman"}}
    man = ins.wide_pass(_db(tmp_path), a, "id", set(), att, NOW, write=True)
    assert len(calls) == 1 and man["notes_kept"] == 0


def test_unsupported_notes_count_as_rejected_verification(tmp_path, monkeypatch):
    _verify_driver(monkeypatch, [GOOD], supported=False)
    a = A(); a.consistency_k = 3
    man = ins.wide_pass(_db(tmp_path), a, "id", set(),
                        {"timelines": {H: timeline()}, "labels": LABELS}, NOW, write=True)
    assert man["rejected_verification"] == 1 and man["notes_kept"] == 0
    assert man["notes_written"] == 0


def test_manifest_counter_types_rate_and_promotion_block(tmp_path, monkeypatch):
    monkeypatch.setattr(ins, "call_model", lambda *a, **k: json.dumps([GOOD, INVENTED]))
    monkeypatch.setattr(ins, "verify_claims", lambda a, s, u, claims, k: [1] * len(claims))
    db = _db(tmp_path)
    man = ins.wide_pass(db, A(), "id", set(), {"timelines": {H: timeline()}, "labels": LABELS},
                        NOW, write=True)
    for key in INT_COUNTERS:
        assert type(man[key]) is int, key
    assert man["dry_run"] is False and type(man["elapsed_s"]) is float
    assert man["names_proposed"] == 2 and man["names_rejected"] == 1
    assert man["names_rejected_rate"] == 0.5 and man["promotion_blocked"] is True
    # blocked promotion does not block writes: rows stay unrendered (HU_INSIGHT_WIDE=off)
    assert man["notes_written"] == 1


def test_names_rejected_rate_is_zero_without_names(tmp_path, monkeypatch):
    monkeypatch.setattr(ins, "call_model", lambda *a, **k: "[]")
    man = ins.wide_pass(_db(tmp_path), A(), "id", set(),
                        {"timelines": {H: timeline()}, "labels": LABELS}, NOW, write=False)
    assert man["names_rejected_rate"] == 0.0 and type(man["names_rejected_rate"]) is float
    assert man["promotion_blocked"] is False and man["dry_run"] is True


def _small_timeline():
    t = NOW - dt.timedelta(hours=5)
    return ([{"rowid": 500 + i, "guid": f"s{i}", "from_me": False, "text": f"s {i}", "t": t,
              "atype": 0} for i in range(10)] +
            [{"rowid": 600 + i, "guid": f"me{i}", "from_me": True, "text": f"m {i}", "t": t,
              "atype": 0} for i in range(5)])


def test_every_eligible_contact_is_accounted_for_exactly_once(tmp_path, monkeypatch):
    sup, nev, skip, good, err, late1, late2 = (f"+1555000000{i}" for i in range(1, 8))
    _suppress(sup)
    never = tmp_path / "never.json"
    never.write_text(json.dumps([nev]))
    timelines = {h: timeline() for h in (sup, nev, good, err, late1, late2)}
    timelines[skip] = _small_timeline()
    state = {h: i for i, h in enumerate((sup, nev, skip, good, err, late1, late2), 1)}

    def cm(url, model, system, user, **k):
        if err in system:
            raise ConnectionResetError("reset")
        return json.dumps([GOOD])
    monkeypatch.setattr(ins, "call_model", cm)
    monkeypatch.setattr(ins, "verify_claims", lambda a, s, u, claims, k: [1] * len(claims))
    ticks = iter(range(100))
    monkeypatch.setattr(ins, "_utc_now", lambda: NOW + dt.timedelta(minutes=next(ticks)))
    a = A(); a.min_turns = 16; a.never_path = str(never)
    man = ins.wide_pass(_db(tmp_path), a, "id", set(), {"timelines": timelines, "labels": LABELS},
                        NOW, write=True, deadline=NOW + dt.timedelta(minutes=4, seconds=30),
                        state=state)
    assert man["eligible"] == 7
    buckets = ("excluded_suppressed", "excluded_never", "skipped_min_turns", "curated",
               "model_errors", "unreached_at_deadline")
    assert [man[b] for b in buckets] == [1, 1, 1, 1, 1, 2]
    assert sum(man[b] for b in buckets) == man["eligible"]
    assert man["stopped_at_deadline"] == 1


def test_dry_run_manifest_is_suffixed_atomic_and_counts_only(tmp_path, monkeypatch):
    db = _db(tmp_path)
    a = _stub_run(tmp_path, monkeypatch, {H: timeline()}, write=False)
    monkeypatch.setattr(ins, "call_model", lambda *a, **k: json.dumps([GOOD]))
    monkeypatch.setattr(ins, "verify_claims", lambda a, s, u, claims, k: [1] * len(claims))
    replaced, real_replace = [], os.replace

    def spy(src, dst):
        replaced.append((src, dst))
        real_replace(src, dst)
    monkeypatch.setattr(ins.os, "replace", spy)
    assert ins.run_wide(a, db, "id", set(), 1) == 0
    files = sorted(p.name for p in Path(a.manifest_dir).iterdir())
    assert len(files) == 1 and files[0].endswith("-dryrun.json")
    assert replaced and replaced[-1][1].endswith(files[0]) and replaced[-1][0] != replaced[-1][1]
    text = (Path(a.manifest_dir) / files[0]).read_text()
    for secret in (H, "Priya", "surgery"):
        assert secret not in text
    assert json.loads(text)["dry_run"] is True


# ---- I4: nothing identifying reaches stdout/stderr --------------------------

def test_wide_pass_prints_no_note_text_or_handle(tmp_path, monkeypatch, capsys):
    notes = [{"note": "SECRET priya diagnosis", "evidence": ["t0"]},
             {"note": "OTHER private thing", "evidence": ["d0"]}]

    def cm(url, model, system, user, **k):
        if H2 in system:
            raise urllib.error.URLError(f"failed for {H2}")
        return json.dumps(notes)
    monkeypatch.setattr(ins, "call_model", cm)
    monkeypatch.setattr(ins, "verify_claims", lambda a, s, u, claims, k: [0] * len(claims))
    never = tmp_path / "never.json"
    never.write_text(json.dumps(["nobody@example.com"]))
    a = A(); a.consistency_k = 3; a.never_path = str(never)
    att = {"timelines": {H: timeline(), H2: timeline()}, "labels": {**LABELS, "bot": "huuman"}}
    ins.wide_pass(_db(tmp_path), a, "id", set(), att, NOW, write=True)
    out = capsys.readouterr()
    printed = out.out + out.err
    for secret in ("SECRET", "OTHER", "priya", H, H2, "nobody@example.com"):
        assert secret not in printed, secret
    assert "1 curator_never" in out.err  # unmatched never entries: a count, not the entry


# ---- I7: loopback handle from config.json is excluded -----------------------

def test_run_wide_excludes_the_configured_loopback_handle(tmp_path, monkeypatch, capsys):
    cfg = tmp_path / "config.json"
    cfg.write_text(json.dumps({"channels": {"imessage": {"loopback_handle": [H]}}}))
    monkeypatch.setattr(ins, "HUMAN_CONFIG", str(cfg))
    a = _stub_run(tmp_path, monkeypatch, {H: timeline()})
    monkeypatch.setattr(ins, "call_model", _raises(AssertionError("model called")))
    assert ins.run_wide(a, _db(tmp_path), "id", set(), 1) == 2  # 0 eligible -> refuse
    assert "0 eligible" in capsys.readouterr().err


# ---- m2: persona-only flags are refused with --population wide ---------------

@pytest.mark.parametrize("extra", [["--contact", "+15550000042"], ["--prospective"],
                                   ["--retire-superseded"], ["--prune-triggers"]])
def test_main_refuses_wide_with_persona_only_flags(extra, monkeypatch, capsys):
    monkeypatch.setattr(ins, "load_persona", _raises(AssertionError("persona touched")))
    assert ins.main(["--population", "wide", *extra]) == 2
    assert "refusing" in capsys.readouterr().err


def test_main_wide_alone_is_not_refused_by_the_flag_check(monkeypatch):
    monkeypatch.setattr(ins, "load_persona", _raises(LookupError("reached persona load")))
    with pytest.raises(LookupError):
        ins.main(["--population", "wide"])
