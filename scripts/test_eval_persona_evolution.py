#!/usr/bin/env python3
"""Hermetic unit tests for scripts/eval_persona_evolution.py.

No chat.db access anywhere in this file -- every test operates on
synthetic strings or synthetic (datetime, str) tuples. Covers the metric
functions, the bootstrap CI, and the windowing/refusal logic.
"""
import datetime
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import eval_persona_evolution as epe  # noqa: E402


# ---------------------------------------------------------------------------
# msg_length_chars
# ---------------------------------------------------------------------------

def test_length_chars_basic():
    assert epe.msg_length_chars("hey") == 3
    assert epe.msg_length_chars("") == 0
    assert epe.msg_length_chars("hello world") == 11


# ---------------------------------------------------------------------------
# starts_lowercase
# ---------------------------------------------------------------------------

def test_starts_lowercase_true():
    assert epe.starts_lowercase("hey what's up") is True


def test_starts_lowercase_false_capitalized():
    assert epe.starts_lowercase("Hey what's up") is False


def test_starts_lowercase_skips_leading_whitespace():
    assert epe.starts_lowercase("   hey") is True


def test_starts_lowercase_skips_leading_emoji_and_punctuation():
    assert epe.starts_lowercase("😂 lol that's great") is True
    assert epe.starts_lowercase("...yeah") is True
    assert epe.starts_lowercase("3 more minutes") is True  # 'm' is lowercase


def test_starts_lowercase_none_when_no_alpha():
    assert epe.starts_lowercase("😂😂😂") is None
    assert epe.starts_lowercase("123") is None
    assert epe.starts_lowercase("") is None


# ---------------------------------------------------------------------------
# terminal_punctuation
# ---------------------------------------------------------------------------

def test_terminal_punctuation_none():
    assert epe.terminal_punctuation("hey what's up") == "none"


def test_terminal_punctuation_question():
    assert epe.terminal_punctuation("you coming?") == "question"


def test_terminal_punctuation_exclaim():
    assert epe.terminal_punctuation("no way!") == "exclaim"


def test_terminal_punctuation_period():
    assert epe.terminal_punctuation("sounds good.") == "period"


def test_terminal_punctuation_ellipsis_ascii_and_unicode():
    assert epe.terminal_punctuation("so anyway...") == "ellipsis"
    assert epe.terminal_punctuation("so anyway…") == "ellipsis"


def test_terminal_punctuation_trailing_whitespace_ignored():
    assert epe.terminal_punctuation("you coming?   ") == "question"


def test_terminal_punctuation_empty_is_none():
    assert epe.terminal_punctuation("") == "none"
    assert epe.terminal_punctuation("   ") == "none"


def test_terminal_punctuation_multi_mark_takes_last_char():
    assert epe.terminal_punctuation("wait what?!") == "exclaim"


# ---------------------------------------------------------------------------
# emoji detection
# ---------------------------------------------------------------------------

def test_has_emoji_true():
    assert epe.has_emoji("nice 😂") is True
    assert epe.has_emoji("👋") is True


def test_has_emoji_false():
    assert epe.has_emoji("nice") is False
    assert epe.has_emoji("") is False


# ---------------------------------------------------------------------------
# formality proxy: contractions + first-person-plural
# ---------------------------------------------------------------------------

def test_contractions_per_100_words_basic():
    # "I don't know" -> 3 words, 1 contraction -> 100/3
    val = epe.contractions_per_100_words("I don't know")
    assert abs(val - (100.0 / 3.0)) < 1e-9


def test_contractions_per_100_words_none():
    assert epe.contractions_per_100_words("I know that") == 0.0


def test_contractions_per_100_words_empty():
    assert epe.contractions_per_100_words("") == 0.0
    assert epe.contractions_per_100_words("😂😂") == 0.0


def test_contractions_per_100_words_curly_apostrophe():
    # iOS autocorrect emits U+2019, not ASCII "'" -- a real chat.db sample
    # was ~8x curly vs straight, so this must count identically to ASCII.
    straight = epe.contractions_per_100_words("I don't know")
    curly = epe.contractions_per_100_words("I don’t know")
    assert straight == curly
    assert abs(curly - (100.0 / 3.0)) < 1e-9


def test_first_person_plural_basic():
    # "we should go now" -> 4 words, 1 plural ('we') -> 25.0
    val = epe.first_person_plural_per_100_words("we should go now")
    assert abs(val - 25.0) < 1e-9


def test_first_person_plural_none():
    assert epe.first_person_plural_per_100_words("I should go now") == 0.0


def test_first_person_plural_multiple_hits():
    # "we got our stuff" -> 4 words, 'we' + 'our' = 2 -> 50.0
    val = epe.first_person_plural_per_100_words("we got our stuff")
    assert abs(val - 50.0) < 1e-9


# ---------------------------------------------------------------------------
# warmth proxy
# ---------------------------------------------------------------------------

def test_warmth_hits_basic():
    # "thanks so much i love this" -> 6 words, 2 hits (thanks, love) -> 33.33
    val = epe.warmth_hits_per_100_words("thanks so much i love this")
    assert abs(val - (200.0 / 6.0)) < 1e-9


def test_warmth_hits_none():
    assert epe.warmth_hits_per_100_words("meeting moved to 3pm") == 0.0


def test_warmth_lexicon_is_small_and_fixed():
    # Contract: the lexicon is a fixed tuple documented in the script, not
    # derived at runtime. Guards against someone quietly wiring in a live
    # sentiment model behind the same function name.
    assert isinstance(epe.WARMTH_LEXICON, tuple)
    assert 5 <= len(epe.WARMTH_LEXICON) <= 30
    assert all(isinstance(w, str) and w == w.lower() for w in epe.WARMTH_LEXICON)


# ---------------------------------------------------------------------------
# compute_features
# ---------------------------------------------------------------------------

def test_compute_features_shape():
    feats = epe.compute_features("hey you coming?")
    for key, _ in epe.AXES:
        assert key in feats


def test_compute_features_starts_lowercase_none_excluded_from_axes_by_caller():
    feats = epe.compute_features("😂😂😂")
    assert feats["starts_lowercase"] is None


# ---------------------------------------------------------------------------
# bootstrap_ci
# ---------------------------------------------------------------------------

def test_bootstrap_ci_empty():
    assert epe.bootstrap_ci([]) == (0.0, 0.0, 0.0)


def test_bootstrap_ci_single_value():
    mean, lo, hi = epe.bootstrap_ci([5.0])
    assert mean == lo == hi == 5.0


def test_bootstrap_ci_constant_values_zero_width():
    mean, lo, hi = epe.bootstrap_ci([3.0] * 50, n_resamples=200)
    assert mean == 3.0
    assert lo == 3.0
    assert hi == 3.0


def test_bootstrap_ci_bounds_contain_mean():
    values = [0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0]
    mean, lo, hi = epe.bootstrap_ci(values, n_resamples=500, seed=7)
    assert lo <= mean <= hi
    assert 0.0 <= lo and hi <= 1.0


def test_bootstrap_ci_deterministic_given_seed():
    values = [1.0, 2.0, 3.0, 4.0, 5.0, 2.0, 3.0]
    a = epe.bootstrap_ci(values, n_resamples=300, seed=42)
    b = epe.bootstrap_ci(values, n_resamples=300, seed=42)
    assert a == b


def test_bootstrap_ci_narrower_with_more_data_same_variance():
    # More draws from the same underlying distribution should not blow the
    # CI wider; a crude monotonicity check rather than an exact bound.
    small = [0.0, 1.0] * 5
    large = [0.0, 1.0] * 200
    _, lo_s, hi_s = epe.bootstrap_ci(small, n_resamples=1000, seed=1)
    _, lo_l, hi_l = epe.bootstrap_ci(large, n_resamples=1000, seed=1)
    assert (hi_l - lo_l) <= (hi_s - lo_s) + 1e-9


# ---------------------------------------------------------------------------
# aggregate_window
# ---------------------------------------------------------------------------

def test_aggregate_window_reports_all_axes_and_n():
    texts = ["hey what's up", "you coming?", "no way!!", "😂😂"] * 30  # n=120
    agg = epe.aggregate_window(texts, n_resamples=200)
    assert agg["n"] == 120
    for _, label in epe.AXES:
        assert label in agg["axes"]
        assert "mean" in agg["axes"][label]


def test_aggregate_window_lowercase_start_excludes_alpha_free_messages():
    texts = ["hey", "😂😂😂"]
    agg = epe.aggregate_window(texts, n_resamples=100)
    # "hey" starts lowercase (1/1); the emoji-only message has no alpha char
    # and must not count as a lowercase-start=0 case.
    assert agg["axes"]["lowercase_start_rate"]["n"] == 1
    assert agg["axes"]["lowercase_start_rate"]["mean"] == 1.0


# ---------------------------------------------------------------------------
# bucket_by_window
# ---------------------------------------------------------------------------

def _dt(y, m, d, h=12):
    return datetime.datetime(y, m, d, h)


def test_bucket_by_window_splits_pre_post_correctly():
    event = datetime.date(2026, 7, 26)
    messages = [
        (_dt(2026, 7, 20), "pre A"),
        (_dt(2026, 7, 25), "pre B"),
        (_dt(2026, 7, 26), "post A (event day itself)"),
        (_dt(2026, 8, 1), "post B"),
        (_dt(2026, 6, 1), "too early, outside window"),
        (_dt(2026, 9, 1), "too late, outside window"),
    ]
    pre, post = epe.bucket_by_window(messages, event, window_days=30)
    assert pre == ["pre A", "pre B"]
    assert post == ["post A (event day itself)", "post B"]


def test_bucket_by_window_empty_when_no_messages_in_range():
    event = datetime.date(2026, 7, 26)
    messages = [(_dt(2026, 1, 1), "way outside")]
    pre, post = epe.bucket_by_window(messages, event, window_days=30)
    assert pre == []
    assert post == []


# ---------------------------------------------------------------------------
# delta_report -- directional shift detection
# ---------------------------------------------------------------------------

def test_delta_report_detects_a_real_register_shift():
    # Pre: consistently capitalized/formal-ish. Post: consistently lowercase.
    pre_texts = ["Hello, how are you today."] * 150
    post_texts = ["yo whats up lol"] * 150
    pre_agg = epe.aggregate_window(pre_texts, n_resamples=200)
    post_agg = epe.aggregate_window(post_texts, n_resamples=200)
    report = epe.delta_report(pre_agg, post_agg)

    lc = report["lowercase_start_rate"]
    assert lc["pre"]["mean"] == 0.0
    assert lc["post"]["mean"] == 1.0
    assert lc["delta"] == 1.0
    assert lc["moved_beyond_ci"] is True


def test_delta_report_no_shift_when_windows_identical():
    texts = ["hey what's up"] * 150
    agg_a = epe.aggregate_window(texts, n_resamples=200, seed=1)
    agg_b = epe.aggregate_window(texts, n_resamples=200, seed=1)
    report = epe.delta_report(agg_a, agg_b)
    for label in report:
        assert report[label]["delta"] == 0.0
        assert report[label]["moved_beyond_ci"] is False


# ---------------------------------------------------------------------------
# EVENTS constant sanity -- dates parse, citations non-empty, confidence
# flagged (this pins the "documented low-confidence dates" contract itself)
# ---------------------------------------------------------------------------

def test_events_dates_parse_and_are_flagged_low_confidence():
    for name, ev in epe.EVENTS.items():
        d = datetime.datetime.strptime(ev["date"], "%Y-%m-%d").date()
        assert isinstance(d, datetime.date)
        assert "LOW" in ev["confidence"]
        assert len(ev["citations"]) >= 2
        assert ev["note"]


# ---------------------------------------------------------------------------
# run() refusal contract -- no chat.db access; monkeypatch fetch_outbound_messages
# ---------------------------------------------------------------------------

class _Args:
    def __init__(self, **kw):
        self.db = "unused"
        self.start = "2026-03-01"
        self.end = "2026-09-03"
        self.window_days = 30
        self.min_n = 100
        self.event = "job"
        self.n_resamples = 100
        self.seed = 42
        self.out = None
        self.explain_dates = False
        self.full_range = False
        self.source = None
        self.__dict__.update(kw)


def test_run_refuses_and_writes_nothing_when_n_below_min(monkeypatch, tmp_path, capsys):
    # Only 5 messages total -- both windows will be far under min_n=100.
    fake_messages = [(_dt(2026, 7, 26), "hi")] * 5
    monkeypatch.setattr(epe, "fetch_outbound_messages", lambda *a, **kw: fake_messages)

    out_path = tmp_path / "should_not_exist.json"
    args = _Args(out=str(out_path))
    rc = epe.run(args)

    assert rc == 1
    assert not out_path.exists()
    printed = capsys.readouterr().out
    assert "INSUFFICIENT_DATA" in printed


def test_run_succeeds_and_writes_out_when_n_sufficient(monkeypatch, tmp_path, capsys):
    pre = [(_dt(2026, 7, 1), "hey what's up")] * 150
    post = [(_dt(2026, 7, 27), "yo lol")] * 150
    monkeypatch.setattr(epe, "fetch_outbound_messages", lambda *a, **kw: pre + post)

    out_path = tmp_path / "results.json"
    args = _Args(out=str(out_path), n_resamples=100)
    rc = epe.run(args)

    assert rc == 0
    assert out_path.exists()
    data = out_path.read_text()
    assert "INSUFFICIENT_DATA" not in data
    assert '"overall_status": "OK"' in data


def test_run_explain_dates_makes_no_db_call(monkeypatch, capsys):
    def _boom(*a, **kw):
        raise AssertionError("fetch_outbound_messages must not be called with --explain-dates")

    monkeypatch.setattr(epe, "fetch_outbound_messages", _boom)
    args = _Args(explain_dates=True, event="both")
    rc = epe.run(args)
    assert rc == 0
    printed = capsys.readouterr().out
    assert "move" in printed and "job" in printed


def test_run_event_none_skips_event_analysis(monkeypatch):
    messages = [(_dt(2026, 8, 10), "hey what's up")] * 200
    monkeypatch.setattr(epe, "fetch_outbound_messages", lambda *a, **kw: messages)
    args = _Args(event="none")
    rc = epe.run(args)
    assert rc == 0


def test_run_full_range_reports_summary_independent_of_event_refusal(monkeypatch, tmp_path):
    # Events will both refuse (n=0 in every pre-window), but full-range has
    # plenty of data and should still refuse the WHOLE run (writes nothing)
    # because overall_status folds in event insufficiency too -- this test
    # pins that full_range does not silently mask a refused event.
    messages = [(_dt(2026, 8, 10), "hey what's up")] * 200
    monkeypatch.setattr(epe, "fetch_outbound_messages", lambda *a, **kw: messages)
    out_path = tmp_path / "out.json"
    args = _Args(event="job", full_range=True, out=str(out_path))
    rc = epe.run(args)
    assert rc == 1
    assert not out_path.exists()


def test_run_full_range_only_succeeds_when_event_analysis_skipped(monkeypatch, tmp_path):
    messages = [(_dt(2026, 8, 10), "hey what's up")] * 200
    monkeypatch.setattr(epe, "fetch_outbound_messages", lambda *a, **kw: messages)
    out_path = tmp_path / "out.json"
    args = _Args(event="none", full_range=True, out=str(out_path))
    rc = epe.run(args)
    assert rc == 0
    assert out_path.exists()
    data = json.loads(out_path.read_text())
    assert data["full_range_summary"]["status"] == "OK"
    assert data["full_range_summary"]["n"] == 200
    assert "events" in data and data["events"] == {}


# ---------------------------------------------------------------------------
# --source: second store reader + de-dup (synthetic fixtures only)
# ---------------------------------------------------------------------------

_MINUS4 = datetime.timezone(datetime.timedelta(hours=-4))


def _write_jsonl(path, records):
    path.write_text("".join(json.dumps(r) + "\n" for r in records))
    return str(path)


def _training_pair(ts_local_iso, text, prior_role="user"):
    return {
        "messages": [
            {"role": prior_role, "content": "context turn"},
            {"role": "assistant", "content": text},
        ],
        "metadata": {"chat_id": "x", "timestamp": ts_local_iso, "reply_length": len(text)},
    }


def test_local_naive_to_utc_naive_shifts_by_offset():
    local = datetime.datetime(2026, 7, 10, 12, 0, 0)
    assert epe.local_naive_to_utc_naive(local, tz=_MINUS4) == datetime.datetime(2026, 7, 10, 16, 0, 0)


def test_read_export_training_pairs_shape(tmp_path):
    path = _write_jsonl(tmp_path / "tp.jsonl", [
        _training_pair("2026-07-10T12:00:00", "yeah on my way"),
        _training_pair("2026-07-11T08:30:15.123456", "lol ok"),
    ])
    rows = epe.read_export_jsonl(path, tz=_MINUS4)
    assert rows == [
        (datetime.datetime(2026, 7, 10, 16, 0, 0), "yeah on my way"),
        (datetime.datetime(2026, 7, 11, 12, 30, 15, 123456), "lol ok"),
    ]


def test_read_export_ground_truth_shape(tmp_path):
    path = _write_jsonl(tmp_path / "gt.jsonl", [
        {"incoming": "you coming?", "seth_reply": "yep 5 min", "timestamp": "2026-07-10T12:00:00",
         "chat_id": "x", "delay_seconds": 40.0, "hour_of_day": 12, "day_of_week": 4},
    ])
    rows = epe.read_export_jsonl(path, tz=_MINUS4)
    assert rows == [(datetime.datetime(2026, 7, 10, 16, 0, 0), "yep 5 min")]


def test_read_export_unknown_shape_raises(tmp_path):
    path = _write_jsonl(tmp_path / "bad.jsonl", [{"prompt": "a", "chosen": "b", "rejected": "c"}])
    try:
        epe.read_export_jsonl(path, tz=_MINUS4)
    except ValueError as e:
        assert "shape" in str(e)
    else:
        raise AssertionError("expected ValueError for a DPO-shaped record (no Seth provenance)")


def test_read_export_skips_tapback_echo_and_blank(tmp_path):
    path = _write_jsonl(tmp_path / "tp.jsonl", [
        _training_pair("2026-07-10T12:00:00", "Loved \u201cok see you\u201d"),
        _training_pair("2026-07-10T12:01:00", "   "),
        _training_pair("2026-07-10T12:02:00", "real one"),
    ])
    rows = epe.read_export_jsonl(path, tz=_MINUS4)
    assert [t for _, t in rows] == ["real one"]


def test_read_export_last_turn_must_be_assistant(tmp_path):
    rec = _training_pair("2026-07-10T12:00:00", "x")
    rec["messages"].append({"role": "user", "content": "they replied"})
    path = _write_jsonl(tmp_path / "tp.jsonl", [rec])
    try:
        epe.read_export_jsonl(path, tz=_MINUS4)
    except ValueError:
        pass
    else:
        raise AssertionError("a record whose last turn is not Seth-authored must be rejected, not silently miscounted")


def test_dedup_key_second_precision_and_stripped_text():
    a = epe.dedup_key(datetime.datetime(2026, 7, 10, 16, 0, 0, 999), "  hey ")
    b = epe.dedup_key(datetime.datetime(2026, 7, 10, 16, 0, 0, 0), "hey")
    c = epe.dedup_key(datetime.datetime(2026, 7, 10, 16, 0, 1, 0), "hey")
    assert a == b
    assert a != c


def test_merge_sources_drops_rows_already_in_primary():
    primary = [(_dt(2026, 8, 5), "same text"), (_dt(2026, 8, 6), "only in chat.db")]
    extra = [(_dt(2026, 8, 5), "same text"), (_dt(2026, 7, 1), "only in export")]
    merged, stats = epe.merge_sources(primary, [("export.jsonl", extra)])
    assert [t for _, t in merged] == ["only in export", "same text", "only in chat.db"]
    assert stats == [{"path": "export.jsonl", "rows": 2, "added": 1, "duplicates": 1}]


def test_merge_sources_dedups_across_two_extras():
    primary = []
    extra1 = [(_dt(2026, 7, 1), "a"), (_dt(2026, 7, 2), "b")]
    extra2 = [(_dt(2026, 7, 2), "b"), (_dt(2026, 7, 3), "c")]
    merged, stats = epe.merge_sources(primary, [("one", extra1), ("two", extra2)])
    assert [t for _, t in merged] == ["a", "b", "c"]
    assert stats[1] == {"path": "two", "rows": 2, "added": 1, "duplicates": 1}


def test_merge_sources_keeps_same_text_at_different_times():
    primary = [(_dt(2026, 8, 5), "ok")]
    extra = [(_dt(2026, 7, 5), "ok")]
    merged, _ = epe.merge_sources(primary, [("e", extra)])
    assert len(merged) == 2


def test_export_frame_filter_drops_single_char_messages():
    msgs = [(_dt(2026, 8, 5), "k"), (_dt(2026, 8, 5), "ok"), (_dt(2026, 8, 5), "\U0001F44D")]
    kept, dropped = epe.export_frame_filter(msgs)
    assert [t for _, t in kept] == ["ok"]
    assert dropped == 2


def test_window_coverage_reports_first_last_and_days():
    lo, hi = datetime.datetime(2026, 6, 1), datetime.datetime(2026, 7, 1)
    msgs = [(_dt(2026, 6, 26, 9), "a"), (_dt(2026, 6, 30, 22), "b"), (_dt(2026, 7, 2), "outside")]
    cov = epe.window_coverage(msgs, lo, hi)
    assert cov == {"first": "2026-06-26T09:00:00", "last": "2026-06-30T22:00:00", "covered_days": 4.5}


def test_window_coverage_empty_window():
    assert epe.window_coverage([], datetime.datetime(2026, 6, 1), datetime.datetime(2026, 7, 1)) == {
        "first": None, "last": None, "covered_days": 0.0}


def test_run_with_source_merges_export_into_windows_and_reports_provenance(monkeypatch, tmp_path):
    # chat.db (mocked): only post-event rows, so without --source the pre window is n=0.
    chat_rows = [(_dt(2026, 8, 10), "post text %d" % i) for i in range(150)]
    chat_rows.append((_dt(2026, 8, 10), "k"))  # 1-char row: dropped by the export frame filter
    monkeypatch.setattr(epe, "fetch_outbound_messages", lambda *a, **kw: chat_rows)
    # export fixture: pre-event rows (local time, offset -4h) + one exact duplicate of a chat.db row
    recs = [_training_pair("2026-07-%02dT10:00:00" % (1 + i % 25), "pre text %d" % i) for i in range(150)]
    recs.append(_training_pair("2026-08-10T08:00:00", "post text 0"))  # == chat.db 2026-08-10T12:00 UTC
    src = _write_jsonl(tmp_path / "tp.jsonl", recs)
    monkeypatch.setattr(epe, "EXPORT_TZ", _MINUS4)

    out_path = tmp_path / "out.json"
    rc = epe.run(_Args(event="job", source=[src], out=str(out_path)))
    assert rc == 0, "pre window should now satisfy min_n via --source"
    data = json.loads(out_path.read_text())
    ev = data["events"]["job"]
    assert ev["status"] == "OK"
    assert ev["pre_window"]["n"] == 150
    assert ev["post_window"]["n"] == 150  # 150 chat.db rows; the 1-char row dropped; duplicate not double-counted
    assert ev["pre_window"]["coverage"]["first"].startswith("2026-07-01")
    assert data["sources"] == [{"path": src, "rows": 151, "added": 150, "duplicates": 1}]
    assert data["primary_rows_dropped_by_export_frame"] == 1
    # no message text anywhere in the report
    assert "pre text" not in out_path.read_text() and "post text" not in out_path.read_text()


def test_run_with_source_still_refuses_when_pre_window_short(monkeypatch, tmp_path):
    chat_rows = [(_dt(2026, 8, 10), "post %d" % i) for i in range(150)]
    monkeypatch.setattr(epe, "fetch_outbound_messages", lambda *a, **kw: chat_rows)
    src = _write_jsonl(tmp_path / "tp.jsonl",
                       [_training_pair("2026-07-%02dT10:00:00" % (1 + i), "pre %d" % i) for i in range(20)])
    out_path = tmp_path / "out.json"
    rc = epe.run(_Args(event="job", source=[src], out=str(out_path), min_n=100))
    assert rc == 1
    assert not out_path.exists()


if __name__ == "__main__":
    import pytest
    sys.exit(pytest.main([__file__, "-v"]))
