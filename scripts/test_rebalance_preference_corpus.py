"""Tests for scripts/rebalance_preference_corpus.py — synthetic data only,
no chat.db, no model weights. Covers: hitting casing/punctuation targets
within tolerance, refusing when the margin cannot be closed, refusing when
neither a style card nor CLI targets are available, and the KTO
{completion,label} shape alongside the {chosen,rejected} preference shape.

    python3 -m pytest scripts/test_rebalance_preference_corpus.py -v
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "rebalance_preference_corpus.py")
sys.path.insert(0, HERE)

import rebalance_preference_corpus as rbc  # noqa: E402


def _run(args):
    return subprocess.run(
        [sys.executable, SCRIPT] + args,
        capture_output=True, text=True, timeout=30,
    )


def _write_jsonl(path, rows):
    with open(path, "w") as fh:
        for r in rows:
            fh.write(json.dumps(r) + "\n")


def _pref_rows(n, lowercase_frac=1.0, punct_frac=0.0, rejected_text="I would be happy to help with that."):
    """n synthetic {prompt,chosen,rejected} rows. `lowercase_frac` of the
    chosen texts start lowercase; `punct_frac` end with a period."""
    rows = []
    for i in range(n):
        lc = i < round(n * lowercase_frac)
        pt = i < round(n * punct_frac)
        body = "yeah that works for me" if lc else "Yeah that works for me"
        if pt:
            body += "."
        rows.append({"prompt": f"ctx {i}", "chosen": body, "rejected": rejected_text})
    return rows


def _kto_rows(n_true, n_false, lowercase_frac=1.0):
    rows = []
    for i in range(n_true):
        lc = i < round(n_true * lowercase_frac)
        body = "yeah that works for me" if lc else "Yeah that works for me"
        rows.append({"prompt": f"ctx {i}", "completion": body, "label": True})
    for i in range(n_false):
        rows.append({"prompt": f"ctx {i}", "completion": "I would be happy to help with that.", "label": False})
    return rows


# --------------------------------------------------------------------------
# pure-function unit tests (imported directly)
# --------------------------------------------------------------------------


def test_starts_lowercase_transforms_round_trip():
    assert rbc.to_sentence_case("yeah sure") == "Yeah sure"
    assert rbc.to_lowercase_start("Yeah sure") == "yeah sure"
    # no alphabetic char at all -> no-op
    assert rbc.to_sentence_case("123") == "123"
    assert rbc.to_lowercase_start("😀") == "😀"


def test_terminal_punct_transforms():
    assert rbc.add_terminal_punct("hello") == "hello."
    assert rbc.add_terminal_punct("hello!") == "hello!"  # already has one -> no-op
    assert rbc.strip_terminal_punct("hello.") == "hello"
    assert rbc.strip_terminal_punct("hello?!") == "hello"
    assert rbc.strip_terminal_punct("hello") == "hello"  # no-op


def test_transforms_never_change_wording():
    """The whole point: only the first-letter case and trailing punctuation
    may change. Strip both ends and the remaining text must be identical."""
    original = "yeah that totally works for me right now"
    sc = rbc.to_sentence_case(original)
    assert sc.lower() == original.lower()
    assert sc[1:] == original[1:]
    punct = rbc.add_terminal_punct(original)
    assert punct.rstrip(".!?") == original.rstrip(".!?")


def test_index_rows_pref_shape():
    rows = _pref_rows(5)
    chosen_idx, rejected = rbc.index_rows(rows)
    assert len(chosen_idx) == 5
    assert len(rejected) == 5
    assert all(f == "chosen" for _, f in chosen_idx)


def test_index_rows_kto_shape():
    rows = _kto_rows(4, 3)
    chosen_idx, rejected = rbc.index_rows(rows)
    assert len(chosen_idx) == 4
    assert len(rejected) == 3
    assert all(f == "completion" for _, f in chosen_idx)


def test_index_rows_refuses_on_unknown_shape():
    rows = [{"prompt": "x", "response": "y"}]
    try:
        rbc.index_rows(rows)
        assert False, "expected SystemExit"
    except SystemExit as e:
        assert "neither" in str(e)


# --------------------------------------------------------------------------
# resample_axis / rebalance_chosen_texts
# --------------------------------------------------------------------------


def test_resample_axis_hits_target_within_rounding():
    texts = ["yeah sure"] * 90 + ["Yeah sure"] * 10  # 90% lowercase
    import random
    rng = random.Random(1)
    out, report = rbc.resample_axis(
        texts, rbc.is_lowercase_start, 0.08, rbc.to_sentence_case, rbc.to_lowercase_start, rng)
    assert abs(report["after"] - 0.08) < 0.02
    # rows that did NOT change must be byte-identical to their originals
    unchanged = [i for i in range(100) if out[i] == texts[i]]
    assert len(unchanged) > 0


def test_rebalance_chosen_texts_hits_both_axes_within_tolerance():
    rows = _pref_rows(300, lowercase_frac=0.9, punct_frac=0.05)
    _, rejected = rbc.index_rows(rows)
    chosen = [r["chosen"] for r in rows]
    out, axis_reports = rbc.rebalance_chosen_texts(chosen, target_lowercase=0.08, target_punct=0.22, seed=42)
    lc_rate, _ = rbc.lowercase_rate(out)
    pt_rate, _ = rbc.punct_rate(out)
    assert abs(lc_rate - 0.08) < 0.02
    assert abs(pt_rate - 0.22) < 0.02


def test_rebalance_is_deterministic_given_seed():
    rows = _pref_rows(150, lowercase_frac=0.8, punct_frac=0.1)
    chosen = [r["chosen"] for r in rows]
    out1, _ = rbc.rebalance_chosen_texts(list(chosen), 0.08, 0.22, seed=7)
    out2, _ = rbc.rebalance_chosen_texts(list(chosen), 0.08, 0.22, seed=7)
    assert out1 == out2
    out3, _ = rbc.rebalance_chosen_texts(list(chosen), 0.08, 0.22, seed=8)
    assert out1 != out3 or out1 == out3  # different seed MAY coincide; just must not crash


def test_never_invents_text_only_transforms_casing_punct():
    rows = _pref_rows(200, lowercase_frac=0.9, punct_frac=0.0)
    chosen = [r["chosen"] for r in rows]
    out, _ = rbc.rebalance_chosen_texts(list(chosen), 0.08, 0.22, seed=42)
    for before, after in zip(chosen, out):
        # normalize: drop leading-char case and trailing terminal punctuation,
        # everything else must be byte-identical
        b = rbc.to_sentence_case(rbc.strip_terminal_punct(before))
        a = rbc.to_sentence_case(rbc.strip_terminal_punct(after))
        assert a == b


# --------------------------------------------------------------------------
# resolve_targets
# --------------------------------------------------------------------------


def test_resolve_targets_from_cli():
    lc, pt, prov = rbc.resolve_targets(0.1, 0.3, "/nonexistent/style-card.json")
    assert lc == 0.1 and pt == 0.3
    assert "cli" in prov["lowercase_start"].lower() or "--target" in prov["lowercase_start"]


def test_resolve_targets_from_style_card(tmp_path):
    card = {"axes": {"lowercase_start_rate": {"value": 0.09},
                      "no_terminal_punct_rate": {"value": 0.75}}}
    card_path = tmp_path / "seth.style-card.json"
    card_path.write_text(json.dumps(card))
    lc, pt, prov = rbc.resolve_targets(None, None, str(card_path))
    assert abs(lc - 0.09) < 1e-9
    assert abs(pt - 0.25) < 1e-9
    assert "style_card" in prov["lowercase_start"]
    assert "style_card" in prov["terminal_punct"]


def test_resolve_targets_refuses_without_card_or_cli():
    try:
        rbc.resolve_targets(None, None, "/nonexistent/style-card.json")
        assert False, "expected SystemExit"
    except SystemExit as e:
        assert "REFUSING" in str(e)


def test_resolve_targets_cli_overrides_style_card(tmp_path):
    card = {"axes": {"lowercase_start_rate": {"value": 0.5},
                      "no_terminal_punct_rate": {"value": 0.5}}}
    card_path = tmp_path / "seth.style-card.json"
    card_path.write_text(json.dumps(card))
    lc, pt, prov = rbc.resolve_targets(0.08, None, str(card_path))
    assert lc == 0.08          # explicit CLI value wins
    assert abs(pt - 0.5) < 1e-9  # falls back to the card for the other axis


# --------------------------------------------------------------------------
# CLI / subprocess integration tests
# --------------------------------------------------------------------------


def test_cli_writes_output_and_sidecar_on_success():
    d = tempfile.mkdtemp()
    inp = os.path.join(d, "in.jsonl")
    out = os.path.join(d, "out.jsonl")
    _write_jsonl(inp, _pref_rows(200, lowercase_frac=0.9, punct_frac=0.1))
    r = _run(["--input", inp, "--output", out,
              "--target-lowercase", "0.08", "--target-punct", "0.22"])
    assert r.returncode == 0, r.stdout + r.stderr
    assert os.path.isfile(out)
    sidecar = out + ".stats.json"
    assert os.path.isfile(sidecar)
    stats = json.load(open(sidecar))
    assert "lowercase_start_rate" in stats["stats"]
    assert stats["stats"]["lowercase_start_rate"]["margin_after"] < 0.15
    # output row count matches input row count, and is valid JSON per line
    out_rows = [json.loads(line) for line in open(out)]
    assert len(out_rows) == 200


def test_cli_refuses_when_margin_cannot_be_closed_and_writes_nothing():
    d = tempfile.mkdtemp()
    inp = os.path.join(d, "in.jsonl")
    out = os.path.join(d, "out.jsonl")
    # rejected side is ALSO heavily lowercase -> chosen can never get far
    # enough away from it to close a 0.15 margin against a 0.08 target.
    rows = _pref_rows(60, lowercase_frac=1.0, rejected_text="yeah that works too")
    _write_jsonl(inp, rows)
    r = _run(["--input", inp, "--output", out,
              "--target-lowercase", "0.08", "--target-punct", "0.22"])
    assert r.returncode != 0
    assert "REFUSING" in r.stdout or "REFUSING" in r.stderr
    assert not os.path.exists(out)
    assert not os.path.exists(out + ".stats.json")


def test_cli_refuses_without_targets_or_style_card():
    d = tempfile.mkdtemp()
    inp = os.path.join(d, "in.jsonl")
    out = os.path.join(d, "out.jsonl")
    _write_jsonl(inp, _pref_rows(50))
    r = _run(["--input", inp, "--output", out, "--style-card", "/nonexistent/card.json"])
    assert r.returncode != 0
    assert "REFUSING" in r.stderr or "REFUSING" in r.stdout
    assert not os.path.exists(out)


def test_cli_refuses_on_missing_input():
    d = tempfile.mkdtemp()
    r = _run(["--input", os.path.join(d, "nope.jsonl"), "--output", os.path.join(d, "out.jsonl"),
              "--target-lowercase", "0.08", "--target-punct", "0.22"])
    assert r.returncode != 0
    assert "REFUSING" in r.stderr or "REFUSING" in r.stdout


def test_cli_refuses_on_empty_input():
    d = tempfile.mkdtemp()
    inp = os.path.join(d, "in.jsonl")
    open(inp, "w").close()
    r = _run(["--input", inp, "--output", os.path.join(d, "out.jsonl"),
              "--target-lowercase", "0.08", "--target-punct", "0.22"])
    assert r.returncode != 0
    assert "REFUSING" in r.stderr or "REFUSING" in r.stdout


def test_cli_dry_run_writes_nothing():
    d = tempfile.mkdtemp()
    inp = os.path.join(d, "in.jsonl")
    out = os.path.join(d, "out.jsonl")
    _write_jsonl(inp, _pref_rows(100, lowercase_frac=0.9))
    r = _run(["--input", inp, "--output", out, "--dry-run",
              "--target-lowercase", "0.08", "--target-punct", "0.22"])
    assert r.returncode == 0, r.stdout + r.stderr
    assert not os.path.exists(out)
    assert not os.path.exists(out + ".stats.json")
    assert "chosen_after" in r.stdout


def test_cli_handles_kto_shape():
    d = tempfile.mkdtemp()
    inp = os.path.join(d, "in.jsonl")
    out = os.path.join(d, "out.jsonl")
    _write_jsonl(inp, _kto_rows(150, 60, lowercase_frac=0.9))
    r = _run(["--input", inp, "--output", out,
              "--target-lowercase", "0.08", "--target-punct", "0.22"])
    assert r.returncode == 0, r.stdout + r.stderr
    out_rows = [json.loads(line) for line in open(out)]
    true_rows = [r2 for r2 in out_rows if r2["label"] is True]
    false_rows = [r2 for r2 in out_rows if r2["label"] is False]
    assert len(true_rows) == 150
    assert len(false_rows) == 60
    lc_rate, _ = rbc.lowercase_rate([r2["completion"] for r2 in true_rows])
    assert abs(lc_rate - 0.08) < 0.03


def test_cli_refuses_on_unknown_row_shape():
    d = tempfile.mkdtemp()
    inp = os.path.join(d, "in.jsonl")
    out = os.path.join(d, "out.jsonl")
    _write_jsonl(inp, [{"prompt": "x", "response": "y"}])
    r = _run(["--input", inp, "--output", out,
              "--target-lowercase", "0.08", "--target-punct", "0.22"])
    assert r.returncode != 0
    assert not os.path.exists(out)


# --------------------------------------------------------------------------
# index_rows now indexes BOTH sides (needed for --match-sides to write back
# the rejected side); confirm the index shape didn't regress.
# --------------------------------------------------------------------------


def test_index_rows_pref_shape_rejected_index_points_at_same_rows():
    rows = _pref_rows(5)
    chosen_idx, rejected_idx = rbc.index_rows(rows)
    assert len(rejected_idx) == 5
    assert all(f == "rejected" for _, f in rejected_idx)
    # preference shape: chosen and rejected indices name the SAME rows
    assert [i for i, _ in chosen_idx] == [i for i, _ in rejected_idx]


def test_index_rows_kto_shape_rejected_index_points_at_label_false_rows():
    rows = _kto_rows(4, 3)
    chosen_idx, rejected_idx = rbc.index_rows(rows)
    assert len(rejected_idx) == 3
    assert all(f == "completion" for _, f in rejected_idx)
    # KTO shape: chosen (label=True) and rejected (label=False) are DIFFERENT rows
    assert set(i for i, _ in chosen_idx).isdisjoint(set(i for i, _ in rejected_idx))


# --------------------------------------------------------------------------
# --match-sides
# --------------------------------------------------------------------------


def _pref_rows_mismatched(n, chosen_lc_frac=0.9, chosen_pt_frac=0.05,
                           rejected_lc_frac=0.0, rejected_pt_frac=0.9):
    """Reproduces the 2026-09-04 shape: rejected is heavily punctuated and
    rarely lowercase, chosen is the opposite -- rebalancing chosen ALONE
    widens the terminal-punct margin instead of closing it."""
    rows = []
    for i in range(n):
        c_lc = i < round(n * chosen_lc_frac)
        c_pt = i < round(n * chosen_pt_frac)
        chosen = "yeah that works for me" if c_lc else "Yeah that works for me"
        if c_pt:
            chosen += "."
        r_lc = i < round(n * rejected_lc_frac)
        r_pt = i < round(n * rejected_pt_frac)
        rejected = "sure happy to help with that" if r_lc else "Sure happy to help with that"
        if r_pt:
            rejected += "."
        rows.append({"prompt": f"ctx {i}", "chosen": chosen, "rejected": rejected})
    return rows


def test_match_sides_off_by_default():
    ap = rbc.build_parser()
    args = ap.parse_args(["--input", "x", "--output", "y"])
    assert args.match_sides is False
    assert args.max_margin == rbc.DEFAULT_MAX_MARGIN


def test_match_sides_pulls_rejected_to_the_same_target_as_chosen():
    """The headline contract: with --match-sides both sides converge to the
    SAME target rate on both axes, so the chosen-vs-rejected margin collapses
    toward 0 instead of the un-matched run's margin (which the 2026-09-04
    dry-run measured getting WORSE on terminal-punct, 0.338 -> 0.406)."""
    d = tempfile.mkdtemp()
    inp = os.path.join(d, "in.jsonl")
    out = os.path.join(d, "out.jsonl")
    _write_jsonl(inp, _pref_rows_mismatched(300))
    r = _run(["--input", inp, "--output", out,
              "--target-lowercase", "0.08", "--target-punct", "0.18",
              "--match-sides"])
    assert r.returncode == 0, r.stdout + r.stderr
    out_rows = [json.loads(line) for line in open(out)]
    chosen_texts = [row["chosen"] for row in out_rows]
    rejected_texts = [row["rejected"] for row in out_rows]
    c_lc, _ = rbc.lowercase_rate(chosen_texts)
    r_lc, _ = rbc.lowercase_rate(rejected_texts)
    c_pt, _ = rbc.punct_rate(chosen_texts)
    r_pt, _ = rbc.punct_rate(rejected_texts)
    assert abs(c_lc - 0.08) < 0.02
    assert abs(r_lc - 0.08) < 0.02
    assert abs(c_pt - 0.18) < 0.02
    assert abs(r_pt - 0.18) < 0.02
    stats = json.load(open(out + ".stats.json"))
    assert stats["match_sides"] is True
    assert stats["stats"]["lowercase_start_rate"]["margin_after"] < 0.03
    assert stats["stats"]["terminal_punct_rate"]["margin_after"] < 0.03
    # length is only ever perturbed by +/-1 char from a punctuation add/strip
    # (never a wording change) -- it must NOT be dragged toward the other
    # side's median the way casing/punct are.
    assert abs(stats["stats"]["median_length"]["rejected_before"] -
               stats["stats"]["median_length"]["rejected_after"]) <= 1


def test_match_sides_never_alters_wording_on_either_side():
    d = tempfile.mkdtemp()
    inp = os.path.join(d, "in.jsonl")
    out = os.path.join(d, "out.jsonl")
    rows = _pref_rows_mismatched(120)
    _write_jsonl(inp, rows)
    r = _run(["--input", inp, "--output", out,
              "--target-lowercase", "0.08", "--target-punct", "0.18",
              "--match-sides"])
    assert r.returncode == 0, r.stdout + r.stderr
    out_rows = [json.loads(line) for line in open(out)]
    for before, after in zip(rows, out_rows):
        for field in ("chosen", "rejected"):
            b = rbc.to_sentence_case(rbc.strip_terminal_punct(before[field]))
            a = rbc.to_sentence_case(rbc.strip_terminal_punct(after[field]))
            assert a == b, f"{field} wording changed: {before[field]!r} -> {after[field]!r}"


def test_match_sides_kto_labels_get_same_distribution():
    d = tempfile.mkdtemp()
    inp = os.path.join(d, "in.jsonl")
    out = os.path.join(d, "out.jsonl")
    rows = _kto_rows(150, 60, lowercase_frac=0.9)
    # push the label=False (rejected) side to nearly-always-punctuated, the
    # opposite of the label=True target, mirroring the pref-shape hazard.
    for r in rows:
        if r["label"] is False:
            r["completion"] = r["completion"].rstrip(".") + "."
    _write_jsonl(inp, rows)
    r = _run(["--input", inp, "--output", out,
              "--target-lowercase", "0.08", "--target-punct", "0.18",
              "--match-sides"])
    assert r.returncode == 0, r.stdout + r.stderr
    out_rows = [json.loads(line) for line in open(out)]
    true_rows = [row for row in out_rows if row["label"] is True]
    false_rows = [row for row in out_rows if row["label"] is False]
    assert len(true_rows) == 150
    assert len(false_rows) == 60
    lc_true, _ = rbc.lowercase_rate([row["completion"] for row in true_rows])
    lc_false, _ = rbc.lowercase_rate([row["completion"] for row in false_rows])
    pt_true, _ = rbc.punct_rate([row["completion"] for row in true_rows])
    pt_false, _ = rbc.punct_rate([row["completion"] for row in false_rows])
    assert abs(lc_true - 0.08) < 0.03
    assert abs(lc_false - 0.08) < 0.03
    assert abs(pt_true - 0.18) < 0.03
    assert abs(pt_false - 0.18) < 0.03


def test_match_sides_refuses_when_a_side_cannot_reach_target_and_writes_nothing():
    """Rejected side has NO alphabetic character at all (e.g. "123 456"),
    so lowercase-start is undefined (None) on every row and its rate is
    permanently pinned at 0.0 -- it can never reach a far-away target, so
    the post-rebalance margin must exceed --max-margin and the run must
    refuse and write nothing."""
    d = tempfile.mkdtemp()
    inp = os.path.join(d, "in.jsonl")
    out = os.path.join(d, "out.jsonl")
    rows = []
    for i in range(60):
        chosen = "yeah that works" if i < 54 else "Yeah that works"
        rows.append({"prompt": f"ctx {i}", "chosen": chosen, "rejected": "123 456"})
    _write_jsonl(inp, rows)
    r = _run(["--input", inp, "--output", out,
              "--target-lowercase", "0.5", "--target-punct", "0.18",
              "--match-sides"])
    assert r.returncode != 0
    assert "REFUSING" in r.stdout or "REFUSING" in r.stderr
    assert not os.path.exists(out)
    assert not os.path.exists(out + ".stats.json")


def test_match_sides_refuses_on_zero_rejected_rows():
    d = tempfile.mkdtemp()
    inp = os.path.join(d, "in.jsonl")
    out = os.path.join(d, "out.jsonl")
    # KTO shape with every row label=True -> zero rejected rows to match toward
    _write_jsonl(inp, _kto_rows(50, 0, lowercase_frac=0.9))
    r = _run(["--input", inp, "--output", out,
              "--target-lowercase", "0.08", "--target-punct", "0.18",
              "--match-sides"])
    assert r.returncode != 0
    assert "REFUSING" in r.stdout or "REFUSING" in r.stderr
    assert not os.path.exists(out)


def test_match_sides_respects_custom_max_margin():
    """A --max-margin tight enough that even a successful convergence run
    trips it must refuse -- confirms --max-margin is actually wired to the
    gate, not just accepted and ignored. Uses the KTO shape with UNEQUAL
    label=True/label=False counts (150 vs 61) so rounding
    round(target_rate * n) lands on a different exact rate per side (a
    preference-shape {chosen,rejected} pair always has equal n on both
    sides, so it cannot exhibit this rounding gap)."""
    d = tempfile.mkdtemp()
    inp = os.path.join(d, "in.jsonl")
    out = os.path.join(d, "out.jsonl")
    rows = _kto_rows(150, 61, lowercase_frac=0.9)
    for r in rows:
        if r["label"] is False:
            r["completion"] = r["completion"].rstrip(".") + "."
    _write_jsonl(inp, rows)
    r = _run(["--input", inp, "--output", out,
              "--target-lowercase", "0.08", "--target-punct", "0.18",
              "--match-sides", "--max-margin", "0.001"])
    assert r.returncode != 0
    assert "REFUSING" in r.stdout or "REFUSING" in r.stderr
    assert not os.path.exists(out)
    # the SAME corpus with a generous margin must succeed -- proves the
    # refusal above was the margin gate, not some other failure.
    r2 = _run(["--input", inp, "--output", out,
               "--target-lowercase", "0.08", "--target-punct", "0.18",
               "--match-sides", "--max-margin", "0.5"])
    assert r2.returncode == 0, r2.stdout + r2.stderr
    assert os.path.exists(out)


def test_default_behavior_unaffected_when_match_sides_omitted():
    """Backward compatibility: rejected side must be byte-identical
    before vs after when --match-sides is not passed, even though the
    corpus is the same mismatched shape --match-sides is designed to fix."""
    d = tempfile.mkdtemp()
    inp = os.path.join(d, "in.jsonl")
    out = os.path.join(d, "out.jsonl")
    rows = _pref_rows_mismatched(150)
    _write_jsonl(inp, rows)
    r = _run(["--input", inp, "--output", out,
              "--target-lowercase", "0.08", "--target-punct", "0.18"])
    assert r.returncode == 0, r.stdout + r.stderr
    out_rows = [json.loads(line) for line in open(out)]
    for before, after in zip(rows, out_rows):
        assert before["rejected"] == after["rejected"]
    stats = json.load(open(out + ".stats.json"))
    assert stats["match_sides"] is False
    assert stats["stats"]["terminal_punct_rate"]["rejected_before"] == \
        stats["stats"]["terminal_punct_rate"]["rejected_after"]


if __name__ == "__main__":
    import pytest
    raise SystemExit(pytest.main([__file__, "-v"]))

# ---------------------------------------------------------------------------
# --match-emoji: emoji must not be a chosen/rejected discriminator
# (2026-09-05; see scripts/build_v6_preference_corpus.py neutralize_emoji_pairs)
# ---------------------------------------------------------------------------

def _emoji_rows():
    rows = _pref_rows(40, lowercase_frac=0.1, punct_frac=0.2)
    for i in range(0, 40, 4):           # 10 rejected-only emoji rows
        rows[i]["rejected"] = "I would be happy to help with that! 😊"
    rows[1]["chosen"] = "miss you 🙂"    # chosen-only: must be kept
    return rows


def test_match_emoji_strips_rejected_only_emoji_and_keeps_chosen_emoji():
    with tempfile.TemporaryDirectory() as d:
        inp, out = os.path.join(d, "in.jsonl"), os.path.join(d, "out.jsonl")
        _write_jsonl(inp, _emoji_rows())
        r = _run(["--input", inp, "--output", out, "--target-lowercase", "0.1",
                  "--target-punct", "0.2", "--match-sides", "--match-emoji"])
        assert r.returncode == 0, r.stderr + r.stdout
        rows = [json.loads(l) for l in open(out)]
        assert not any(rbc.has_emoji(x["rejected"]) for x in rows if not rbc.has_emoji(x["chosen"]))
        assert rbc.has_emoji(rows[1]["chosen"])
        assert rows[0]["rejected"].startswith("I would be happy to help with that!") or \
            rows[0]["rejected"].startswith("i would be happy to help with that!")
        stats = json.load(open(out + ".stats.json"))
        assert stats["match_emoji"] is True
        assert stats["stats"]["emoji_rate"]["rejected_only_before"] == 10
        assert stats["stats"]["emoji_rate"]["rejected_only_after"] == 0


def test_without_match_emoji_rejected_emoji_is_untouched():
    with tempfile.TemporaryDirectory() as d:
        inp, out = os.path.join(d, "in.jsonl"), os.path.join(d, "out.jsonl")
        _write_jsonl(inp, _emoji_rows())
        r = _run(["--input", inp, "--output", out, "--target-lowercase", "0.1",
                  "--target-punct", "0.2", "--match-sides"])
        assert r.returncode == 0, r.stderr + r.stdout
        rows = [json.loads(l) for l in open(out)]
        assert sum(rbc.has_emoji(x["rejected"]) for x in rows) == 10
        stats = json.load(open(out + ".stats.json"))
        assert stats["match_emoji"] is False
        assert stats["stats"]["emoji_rate"]["rejected_only_after"] == 10


def test_match_emoji_report_mentions_emoji_axis():
    with tempfile.TemporaryDirectory() as d:
        inp = os.path.join(d, "in.jsonl")
        _write_jsonl(inp, _emoji_rows())
        r = _run(["--input", inp, "--dry-run", "--target-lowercase", "0.1",
                  "--target-punct", "0.2", "--match-sides", "--match-emoji"])
        assert r.returncode == 0, r.stderr + r.stdout
        assert "emoji" in r.stdout.lower()
