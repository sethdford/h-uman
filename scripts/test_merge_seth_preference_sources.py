"""Tests for scripts/merge_seth_preference_sources.py — synthetic fixtures
only, no chat.db, no real ~/.human paths, no message text from any real
person. Covers: the two accepted Seth-authored export shapes, the FATAL
per-row provenance check (AC-1.2), de-dup at second precision, tapback/empty
filtering, the --floor and empty-rejected-pool refusals, privacy of the
manifest (AC-1.5), and end-to-end compatibility with
scripts/rebalance_preference_corpus.py's KTO-shape contract.

    python3 -m pytest scripts/test_merge_seth_preference_sources.py -v
"""
import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "merge_seth_preference_sources.py")
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


def _tp_row(ts, text, context=None):
    """One synthetic training_pairs-shaped row."""
    context = context or [{"role": "user", "content": "hey what's up"}]
    return {
        "messages": context + [{"role": "assistant", "content": text}],
        "metadata": {"chat_id": "c1", "timestamp": ts, "reply_length": len(text)},
    }


def _gt_row(ts, text, incoming="hey what's up"):
    """One synthetic ground_truth-shaped row."""
    return {
        "incoming": incoming,
        "seth_reply": text,
        "context_turns": [],
        "delay_seconds": 12.0,
        "chat_id": "c1",
        "timestamp": ts,
        "hour_of_day": 10,
        "day_of_week": 2,
    }


def _daemon_row(prompt="hey", chosen="ok", rejected="I would be happy to help with that."):
    """One synthetic memory.db/dpo_pairs-shaped row — no messages/metadata
    or seth_reply/timestamp, so extract_seth_record must reject it."""
    return {"prompt": prompt, "chosen": chosen, "rejected": rejected, "source": "outbound_edit"}


def _pref_row(prompt, chosen, rejected):
    return {"prompt": prompt, "chosen": chosen, "rejected": rejected}


def _floor_free_env(d, n_chosen=5, floor=1):
    """Write a minimal primary + rejected-pool pair and return the CLI args
    needed to run the merge successfully, with --floor low enough not to
    interfere with the test's own assertion."""
    primary = os.path.join(d, "primary.jsonl")
    rejected_pool = os.path.join(d, "rejected_pool.jsonl")
    _write_jsonl(primary, [
        _tp_row(f"2026-06-01T10:00:0{i}", f"seth reply number {i}") for i in range(n_chosen)
    ])
    _write_jsonl(rejected_pool, [
        _pref_row(f"ctx {i}", "chosen placeholder", "I would be happy to help with that.")
        for i in range(3)
    ])
    return primary, rejected_pool


# --------------------------------------------------------------------------
# 1/2: the two accepted Seth-authored shapes, non-vacuous content assertions
# --------------------------------------------------------------------------

def test_training_pairs_shape_parses_and_extracts_assistant_turn():
    d = tempfile.mkdtemp()
    primary = os.path.join(d, "primary.jsonl")
    _, rejected_pool = _floor_free_env(d, n_chosen=0)
    known_text = "yeah I will be there at seven sharp"
    _write_jsonl(primary, [_tp_row("2026-06-01T10:00:00", known_text)])
    out_dir = os.path.join(d, "out")
    r = _run(["--primary", primary, "--rejected-pool", rejected_pool,
              "--out-dir", out_dir, "--floor", "1"])
    assert r.returncode == 0, r.stdout + r.stderr
    rows = [json.loads(line) for line in open(os.path.join(out_dir, "train.jsonl"))]
    chosen = [r_ for r_ in rows if r_["label"] is True]
    assert len(chosen) == 1
    assert chosen[0]["completion"] == known_text


def test_ground_truth_shape_parses():
    d = tempfile.mkdtemp()
    primary = os.path.join(d, "primary.jsonl")
    _, rejected_pool = _floor_free_env(d, n_chosen=0)
    known_text = "sounds good see you then"
    _write_jsonl(primary, [_gt_row("2026-06-01T10:00:00", known_text)])
    out_dir = os.path.join(d, "out")
    r = _run(["--primary", primary, "--rejected-pool", rejected_pool,
              "--out-dir", out_dir, "--floor", "1"])
    assert r.returncode == 0, r.stdout + r.stderr
    rows = [json.loads(line) for line in open(os.path.join(out_dir, "train.jsonl"))]
    chosen = [r_ for r_ in rows if r_["label"] is True]
    assert len(chosen) == 1
    assert chosen[0]["completion"] == known_text


# --------------------------------------------------------------------------
# 3: AC-1.2 FATAL per-row admission check
# --------------------------------------------------------------------------

def test_daemon_shaped_row_is_fatal():
    d = tempfile.mkdtemp()
    primary = os.path.join(d, "primary.jsonl")
    _, rejected_pool = _floor_free_env(d, n_chosen=0)
    _write_jsonl(primary, [
        _tp_row("2026-06-01T10:00:00", "a real seth text"),
        _daemon_row(),  # no messages/metadata.timestamp, no seth_reply/timestamp
    ])
    out_dir = os.path.join(d, "out")
    r = _run(["--primary", primary, "--rejected-pool", rejected_pool,
              "--out-dir", out_dir, "--floor", "1"])
    assert r.returncode != 0
    combined = r.stdout + r.stderr
    assert "REFUSING" in combined
    assert not os.path.exists(out_dir)


# --------------------------------------------------------------------------
# 4/5: de-dup at exactly second precision
# --------------------------------------------------------------------------

def test_dedup_key_collision_across_two_sources_drops_the_duplicate():
    d = tempfile.mkdtemp()
    primary = os.path.join(d, "primary.jsonl")
    extra = os.path.join(d, "extra.jsonl")
    _, rejected_pool = _floor_free_env(d, n_chosen=0)
    shared_text = "same exact text same exact second"
    _write_jsonl(primary, [_tp_row("2026-06-01T10:00:00", shared_text)])
    _write_jsonl(extra, [_tp_row("2026-06-01T10:00:00", shared_text)])
    out_dir = os.path.join(d, "out")
    r = _run(["--primary", primary, "--extra", extra, "--rejected-pool", rejected_pool,
              "--out-dir", out_dir, "--floor", "1"])
    assert r.returncode == 0, r.stdout + r.stderr
    rows = [json.loads(line) for line in open(os.path.join(out_dir, "train.jsonl"))]
    chosen = [r_ for r_ in rows if r_["label"] is True]
    assert len(chosen) == 1
    manifest = json.load(open(os.path.join(out_dir, "manifest.json")))
    assert len(manifest["extras"]) == 1
    assert manifest["extras"][0]["duplicates"] == 1
    assert manifest["extras"][0]["added"] == 0


def test_dedup_key_near_miss_is_not_deduped():
    d = tempfile.mkdtemp()
    primary = os.path.join(d, "primary.jsonl")
    extra = os.path.join(d, "extra.jsonl")
    _, rejected_pool = _floor_free_env(d, n_chosen=0)
    same_text = "same text different second"
    _write_jsonl(primary, [_tp_row("2026-06-01T10:00:00", same_text)])
    _write_jsonl(extra, [_tp_row("2026-06-01T10:00:02", same_text)])
    out_dir = os.path.join(d, "out")
    r = _run(["--primary", primary, "--extra", extra, "--rejected-pool", rejected_pool,
              "--out-dir", out_dir, "--floor", "1"])
    assert r.returncode == 0, r.stdout + r.stderr
    rows = [json.loads(line) for line in open(os.path.join(out_dir, "train.jsonl"))]
    chosen = [r_ for r_ in rows if r_["label"] is True]
    assert len(chosen) == 2
    manifest = json.load(open(os.path.join(out_dir, "manifest.json")))
    assert manifest["extras"][0]["duplicates"] == 0
    assert manifest["extras"][0]["added"] == 1


# --------------------------------------------------------------------------
# 6/7: refusal floors
# --------------------------------------------------------------------------

def test_rejected_pool_empty_is_fatal():
    d = tempfile.mkdtemp()
    primary, _ = _floor_free_env(d, n_chosen=5)
    empty_rejected_pool = os.path.join(d, "empty_rejected.jsonl")
    open(empty_rejected_pool, "w").close()
    out_dir = os.path.join(d, "out")
    r = _run(["--primary", primary, "--rejected-pool", empty_rejected_pool,
              "--out-dir", out_dir, "--floor", "1"])
    assert r.returncode != 0
    assert "REFUSING" in (r.stdout + r.stderr)
    assert not os.path.exists(out_dir)


def test_rejected_pool_all_rows_missing_rejected_field_is_fatal():
    d = tempfile.mkdtemp()
    primary, _ = _floor_free_env(d, n_chosen=5)
    rejected_pool = os.path.join(d, "rejected_pool.jsonl")
    # every row lacks a `rejected` field entirely
    _write_jsonl(rejected_pool, [{"prompt": "ctx", "chosen": "ok"}])
    out_dir = os.path.join(d, "out")
    r = _run(["--primary", primary, "--rejected-pool", rejected_pool,
              "--out-dir", out_dir, "--floor", "1"])
    assert r.returncode != 0
    assert "REFUSING" in (r.stdout + r.stderr)
    assert not os.path.exists(out_dir)


def test_below_floor_is_fatal():
    d = tempfile.mkdtemp()
    primary = os.path.join(d, "primary.jsonl")
    _, rejected_pool = _floor_free_env(d, n_chosen=0)
    _write_jsonl(primary, [_tp_row(f"2026-06-01T10:00:0{i}", f"reply {i}") for i in range(3)])
    out_dir = os.path.join(d, "out")
    r = _run(["--primary", primary, "--rejected-pool", rejected_pool,
              "--out-dir", out_dir, "--floor", "500"])
    assert r.returncode != 0
    combined = r.stdout + r.stderr
    assert "REFUSING" in combined
    assert "3" in combined and "500" in combined
    assert not os.path.exists(out_dir)


# --------------------------------------------------------------------------
# 9: no raw text in manifest.json
# --------------------------------------------------------------------------

def test_no_raw_text_in_manifest():
    d = tempfile.mkdtemp()
    primary = os.path.join(d, "primary.jsonl")
    _, rejected_pool = _floor_free_env(d, n_chosen=0)
    secret_chosen_text = "the super secret seth message contents xyzzy12345"
    secret_rejected_text = "the super secret rejected message contents plugh98765"
    _write_jsonl(primary, [_tp_row("2026-06-01T10:00:00", secret_chosen_text)])
    _write_jsonl(rejected_pool, [_pref_row("ctx 0", "chosen placeholder", secret_rejected_text)])
    out_dir = os.path.join(d, "out")
    r = _run(["--primary", primary, "--rejected-pool", rejected_pool,
              "--out-dir", out_dir, "--floor", "1"])
    assert r.returncode == 0, r.stdout + r.stderr
    manifest_raw = open(os.path.join(out_dir, "manifest.json")).read()
    assert secret_chosen_text not in manifest_raw
    assert secret_rejected_text not in manifest_raw
    # stdout must not leak it either
    assert secret_chosen_text not in r.stdout
    assert secret_rejected_text not in r.stdout


# --------------------------------------------------------------------------
# 10: tapback + empty rows dropped, and counted separately from duplicates
# --------------------------------------------------------------------------

def test_tapback_and_empty_rows_are_dropped_not_counted_as_duplicates():
    d = tempfile.mkdtemp()
    primary = os.path.join(d, "primary.jsonl")
    _, rejected_pool = _floor_free_env(d, n_chosen=0)
    from eval_persona_evolution import TAPBACK_PREFIXES
    tapback_text = TAPBACK_PREFIXES[0] + 'nice one”'
    _write_jsonl(primary, [
        _tp_row("2026-06-01T10:00:00", "a genuine seth reply"),
        _tp_row("2026-06-01T10:00:01", tapback_text),
        _tp_row("2026-06-01T10:00:02", "   "),  # whitespace-only -> dropped as empty
    ])
    out_dir = os.path.join(d, "out")
    r = _run(["--primary", primary, "--rejected-pool", rejected_pool,
              "--out-dir", out_dir, "--floor", "1"])
    assert r.returncode == 0, r.stdout + r.stderr
    rows = [json.loads(line) for line in open(os.path.join(out_dir, "train.jsonl"))]
    chosen = [r_ for r_ in rows if r_["label"] is True]
    assert len(chosen) == 1
    for r_ in chosen:
        assert tapback_text not in r_["completion"]
    manifest = json.load(open(os.path.join(out_dir, "manifest.json")))
    assert manifest["dropped_tapback"] == 1
    assert manifest["dropped_empty"] == 1
    # dropped rows must not be reported as "duplicates" -- distinct buckets
    assert manifest["extras"] == []


# --------------------------------------------------------------------------
# 8: end-to-end -- output is valid KTO shape for rebalance_preference_corpus.py
# --------------------------------------------------------------------------

def test_output_is_valid_kto_shape_for_rebalance_script():
    d = tempfile.mkdtemp()
    primary = os.path.join(d, "primary.jsonl")
    rejected_pool = os.path.join(d, "rejected_pool.jsonl")
    # 40 lowercase-start chosen rows (so rebalance has real casing signal),
    # 40 rejected rows with their own prompts.
    _write_jsonl(primary, [
        _tp_row(f"2026-06-01T10:{i:02d}:00", f"yeah reply number {i} here we go")
        for i in range(40)
    ])
    _write_jsonl(rejected_pool, [
        _pref_row(f"ctx {i}", "unused chosen placeholder",
                  f"I would be happy to help with request number {i}.")
        for i in range(40)
    ])
    out_dir = os.path.join(d, "out")
    r = _run(["--primary", primary, "--rejected-pool", rejected_pool,
              "--out-dir", out_dir, "--floor", "1"])
    assert r.returncode == 0, r.stdout + r.stderr
    merged_train = os.path.join(out_dir, "train.jsonl")

    style_card = os.path.join(d, "style-card.json")
    json.dump({"axes": {
        "lowercase_start_rate": {"value": 0.08},
        "no_terminal_punct_rate": {"value": 0.8},
    }}, open(style_card, "w"))

    rebalanced_out = os.path.join(d, "rebalanced.jsonl")
    # rbc.main() prints its report via plain print() -- captured manually
    # (not via a pytest fixture) so this test collects and runs identically
    # whether invoked through pytest or the stdlib runner below.
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = rbc.main([
            "--input", merged_train, "--output", rebalanced_out,
            "--style-card", style_card, "--dry-run",
        ])
    out = buf.getvalue()
    assert rc == 0, out
    assert "REFUSING" not in out


def _run_all():
    """Stdlib-only runner (no pytest dependency) so this file also passes
    when invoked directly, matching this repo's other capability-gate-check
    test scripts (e.g. scripts/test_check_capability_gates.py's _run())."""
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for fn in fns:
        try:
            fn()
            print(f"PASS {fn.__name__}")
        except Exception as e:  # noqa: BLE001 -- test runner, report and continue
            failed += 1
            print(f"FAIL {fn.__name__}: {e}")
    print(f"\n{len(fns) - failed}/{len(fns)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(_run_all())
