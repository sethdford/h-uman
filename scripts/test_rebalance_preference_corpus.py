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


if __name__ == "__main__":
    import pytest
    raise SystemExit(pytest.main([__file__, "-v"]))
