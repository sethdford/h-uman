"""Tests for scripts/merge_seth_preference_sources.py — synthetic fixtures
only, no chat.db, no real ~/.human paths, no message text from any real
person. Covers: the two accepted Seth-authored export shapes, the FATAL
per-row provenance check (AC-1.2), de-dup at second precision, tapback/empty
filtering, the --floor and empty-rejected-pool refusals, privacy of the
manifest (AC-1.5), prompt-type normalization (every emitted `prompt` is a
plain string), and end-to-end compatibility with BOTH
scripts/rebalance_preference_corpus.py's KTO/paired-shape contract AND
scripts/mlx_tune_train.py's on-disk paired-shape validator (the critic
finding this file's newer tests pin: train.jsonl must be the PAIRED shape
the trainer actually reads, not the KTO shape alone).

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


def _validate_via_mlx_tune_train(data_dir):
    """Validate a merge output's train.jsonl using scripts/mlx_tune_train.py's
    OWN on-disk validator (validate_data_dir/_count_and_validate_jsonl,
    REQUIRED_PAIR_KEYS = ("prompt","chosen","rejected")), not a copy of its
    logic -- this is the point of the test that calls this helper (US-1
    critic finding: prove the ACTUAL trainer accepts the file).

    mlx_tune_train.py's own module-level imports are verified stdlib +
    PyYAML only (argparse/json/os/shutil/sys/pathlib/yaml -- no mlx, no
    torch; see that file's own NO-GO comment for why: the real, weight-
    loading path is gated behind HU_MLX_TUNE_ALLOW_LOAD and only reachable
    through functions this helper never calls), so importing it does not
    load any model. The one risk is PyYAML itself: this test file's CI job
    (.github/workflows/ci.yml capability-gate-check) runs `python3` with NO
    pip-install step, i.e. is meant to be stdlib-only, and doesn't
    guarantee PyYAML is present. If the import fails FOR THAT REASON ONLY,
    fall back to reimplementing the exact same key check (mirroring
    mlx_tune_train.py's REQUIRED_PAIR_KEYS + _count_and_validate_jsonl,
    cited above) so the test still pins the real contract rather than
    skipping silently; any other ImportError is a genuine bug and re-raised.
    """
    try:
        import mlx_tune_train
        return mlx_tune_train.validate_data_dir(data_dir)
    except ImportError as e:
        if "yaml" not in str(e).lower():
            raise
        required_keys = ("prompt", "chosen", "rejected")  # mlx_tune_train.py REQUIRED_PAIR_KEYS
        path = os.path.join(data_dir, "train.jsonl")
        count = 0
        bad_line_total = 0
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                count += 1
                obj = json.loads(line)
                if [k for k in required_keys if k not in obj]:
                    bad_line_total += 1
        return {
            "train": {"exists": True, "count": count, "bad_line_total": bad_line_total},
            "ok": count > 0 and bad_line_total == 0,
        }


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
    # train.kto.jsonl carries the KTO shape (prompt/completion/label) this
    # assertion needs; train.jsonl (checked elsewhere) is the paired shape.
    rows = [json.loads(line) for line in open(os.path.join(out_dir, "train.kto.jsonl"))]
    chosen = [r_ for r_ in rows if r_["label"] is True]
    assert len(chosen) == 1
    assert chosen[0]["completion"] == known_text
    assert isinstance(chosen[0]["prompt"], str)


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
    rows = [json.loads(line) for line in open(os.path.join(out_dir, "train.kto.jsonl"))]
    chosen = [r_ for r_ in rows if r_["label"] is True]
    assert len(chosen) == 1
    assert chosen[0]["completion"] == known_text
    assert isinstance(chosen[0]["prompt"], str)


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
    rows = [json.loads(line) for line in open(os.path.join(out_dir, "train.kto.jsonl"))]
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
    rows = [json.loads(line) for line in open(os.path.join(out_dir, "train.kto.jsonl"))]
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
    rows = [json.loads(line) for line in open(os.path.join(out_dir, "train.kto.jsonl"))]
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
# 8/11: end-to-end -- BOTH output shapes are valid for their respective
# downstream consumers (rebalance_preference_corpus.py accepts either shape
# per-row auto-detected; mlx_tune_train.py's on-disk validator requires the
# paired shape specifically -- train.jsonl, not train.kto.jsonl).
# --------------------------------------------------------------------------

def _write_40x40_fixture(d):
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
    return primary, rejected_pool


def test_paired_train_jsonl_is_valid_shape_for_rebalance_script():
    d = tempfile.mkdtemp()
    primary, rejected_pool = _write_40x40_fixture(d)
    out_dir = os.path.join(d, "out")
    r = _run(["--primary", primary, "--rejected-pool", rejected_pool,
              "--out-dir", out_dir, "--floor", "1"])
    assert r.returncode == 0, r.stdout + r.stderr
    merged_train = os.path.join(out_dir, "train.jsonl")  # PAIRED shape

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


def test_kto_train_jsonl_is_still_valid_shape_for_rebalance_script():
    """train.kto.jsonl must keep working with rebalance_preference_corpus.py
    too -- the paired train.jsonl addition (US-1 critic fix) must not have
    broken the original KTO-shape contract this script's design settled on."""
    d = tempfile.mkdtemp()
    primary, rejected_pool = _write_40x40_fixture(d)
    out_dir = os.path.join(d, "out")
    r = _run(["--primary", primary, "--rejected-pool", rejected_pool,
              "--out-dir", out_dir, "--floor", "1"])
    assert r.returncode == 0, r.stdout + r.stderr
    merged_kto = os.path.join(out_dir, "train.kto.jsonl")

    style_card = os.path.join(d, "style-card.json")
    json.dump({"axes": {
        "lowercase_start_rate": {"value": 0.08},
        "no_terminal_punct_rate": {"value": 0.8},
    }}, open(style_card, "w"))

    rebalanced_out = os.path.join(d, "rebalanced-kto.jsonl")
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = rbc.main([
            "--input", merged_kto, "--output", rebalanced_out,
            "--style-card", style_card, "--dry-run",
        ])
    out = buf.getvalue()
    assert rc == 0, out
    assert "REFUSING" not in out


def test_produced_train_jsonl_passes_mlx_tune_train_validator():
    """HIGH critic finding: scripts/mlx_tune_train.py's on-disk validator
    (REQUIRED_PAIR_KEYS = ("prompt","chosen","rejected"),
    validate_data_dir/_count_and_validate_jsonl, mlx_tune_train.py
    ~lines 220-256) is what the nightly candidate stage actually reads.
    This runs that REAL validator (see _validate_via_mlx_tune_train) against
    train.jsonl and asserts every row is accepted -- not just that the two
    scripts agree on a schema diagram."""
    d = tempfile.mkdtemp()
    # _floor_free_env's timestamp format (f"...:0{i}") is single-digit-safe
    # only -- n_chosen=9 stays within that (see its own docstring/usage
    # elsewhere in this file for the same convention).
    primary, rejected_pool = _floor_free_env(d, n_chosen=9)
    out_dir = os.path.join(d, "out")
    r = _run(["--primary", primary, "--rejected-pool", rejected_pool,
              "--out-dir", out_dir, "--floor", "1"])
    assert r.returncode == 0, r.stdout + r.stderr
    report = _validate_via_mlx_tune_train(out_dir)
    assert report["train"]["count"] == 9, report
    assert report["train"]["bad_line_total"] == 0, report
    assert report["ok"] is True, report


def test_kto_train_jsonl_would_fail_the_same_validator():
    """Non-vacuous control for the test above: train.kto.jsonl (lacking
    "chosen"/"rejected" keys) MUST be rejected by the same validator, proving
    the previous test's PASS is discriminating on shape, not vacuously
    true for any file mlx_tune_train.py is pointed at."""
    d = tempfile.mkdtemp()
    primary, rejected_pool = _floor_free_env(d, n_chosen=5)
    out_dir = os.path.join(d, "out")
    r = _run(["--primary", primary, "--rejected-pool", rejected_pool,
              "--out-dir", out_dir, "--floor", "1"])
    assert r.returncode == 0, r.stdout + r.stderr
    kto_as_train_dir = os.path.join(d, "kto_as_train")
    os.makedirs(kto_as_train_dir)
    # Point the validator at the KTO file under the name it looks for.
    with open(os.path.join(out_dir, "train.kto.jsonl")) as src, \
            open(os.path.join(kto_as_train_dir, "train.jsonl"), "w") as dst:
        dst.write(src.read())
    report = _validate_via_mlx_tune_train(kto_as_train_dir)
    assert report["train"]["bad_line_total"] > 0, report
    assert report["ok"] is False, report


# --------------------------------------------------------------------------
# 12: prompt-type normalization (AC-1.2 follow-up) -- every emitted `prompt`
# is a plain string in BOTH output files, regardless of source shape.
# --------------------------------------------------------------------------

def test_every_emitted_prompt_is_a_plain_string_in_both_files():
    d = tempfile.mkdtemp()
    primary = os.path.join(d, "primary.jsonl")
    rejected_pool = os.path.join(d, "rejected_pool.jsonl")
    # training_pairs shape yields a list[{"role","content"}] prompt;
    # ground_truth shape yields a plain string prompt -- mix both so the
    # normalization is exercised on both source types in one run.
    _write_jsonl(primary, [
        _tp_row("2026-06-01T10:00:00", "multi turn reply here",
                context=[{"role": "user", "content": "yo"},
                         {"role": "assistant", "content": "hey"},
                         {"role": "user", "content": "you around"}]),
        _tp_row("2026-06-01T10:00:01", "single turn reply"),
        _gt_row("2026-06-01T10:00:02", "ground truth reply", incoming="what's good"),
    ])
    _write_jsonl(rejected_pool, [
        _pref_row("some rejected-pool prompt", "unused", "a rejected completion"),
    ])
    out_dir = os.path.join(d, "out")
    r = _run(["--primary", primary, "--rejected-pool", rejected_pool,
              "--out-dir", out_dir, "--floor", "1"])
    assert r.returncode == 0, r.stdout + r.stderr

    kto_rows = [json.loads(line) for line in open(os.path.join(out_dir, "train.kto.jsonl"))]
    paired_rows = [json.loads(line) for line in open(os.path.join(out_dir, "train.jsonl"))]
    assert len(kto_rows) >= 4  # 3 chosen + >=1 rejected
    assert len(paired_rows) == 3  # one paired row per chosen row
    for row in kto_rows:
        assert isinstance(row["prompt"], str), row
    for row in paired_rows:
        assert isinstance(row["prompt"], str), row

    # The multi-turn training_pairs row renders as alternating Them:/Seth:
    # lines (matching ~/.human/training-data/glm-v61-pref/train.jsonl's own
    # multi-turn convention), not a stringified list.
    multi_turn_chosen = [r_ for r_ in kto_rows if r_.get("completion") == "multi turn reply here"]
    assert len(multi_turn_chosen) == 1
    assert multi_turn_chosen[0]["prompt"] == "Them: yo\nSeth: hey\nThem: you around"

    # The single-turn training_pairs row renders as the bare unprefixed text.
    single_turn_chosen = [r_ for r_ in kto_rows if r_.get("completion") == "single turn reply"]
    assert len(single_turn_chosen) == 1
    assert single_turn_chosen[0]["prompt"] == "hey what's up"

    # The ground_truth row's `incoming` string passes through unchanged.
    gt_chosen = [r_ for r_ in kto_rows if r_.get("completion") == "ground truth reply"]
    assert len(gt_chosen) == 1
    assert gt_chosen[0]["prompt"] == "what's good"


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


# ---------------------------------------------------------------------------
# valid.jsonl + config.yaml (2026-09-06: the merged corpus was not trainable
# as-is -- train-glm-adapter.sh requires valid.jsonl and a config whose data:
# line points at the directory)
# ---------------------------------------------------------------------------
import merge_seth_preference_sources as msps  # noqa: E402


def test_split_valid_rows_is_deterministic_disjoint_and_content_keyed():
    rows = [{"prompt": f"p{i}", "chosen": f"c{i}", "rejected": f"r{i}"} for i in range(400)]
    v1, t1 = msps.split_valid_rows(rows, 0.1)
    v2, t2 = msps.split_valid_rows(list(reversed(rows)), 0.1)
    assert v1 and t1 and len(v1) + len(t1) == 400
    assert {json.dumps(r, sort_keys=True) for r in v1} == {json.dumps(r, sort_keys=True) for r in v2}
    assert not {json.dumps(r, sort_keys=True) for r in v1} & {json.dumps(r, sort_keys=True) for r in t1}
    assert msps.split_valid_rows(rows, 0) == ([], rows)


def test_render_config_rewrites_only_the_data_line():
    with tempfile.TemporaryDirectory() as d:
        tpl = os.path.join(d, "t.yaml")
        with open(tpl, "w") as fh:
            fh.write("model: m\n# data: not this one\ndata: /old/dir\niters: 400\n")
        out = msps.render_config(tpl, "/new/dir")
        assert "data: /new/dir\n" in out and "/old/dir" not in out and "iters: 400" in out
        with open(tpl, "w") as fh:
            fh.write("model: m\niters: 400\n")
        try:
            msps.render_config(tpl, "/new/dir")
            assert False, "expected refusal"
        except SystemExit as e:
            assert "no 'data:' line" in str(e)


def test_cli_writes_valid_and_config_and_manifest_counts():
    with tempfile.TemporaryDirectory() as d:
        primary, rejected_pool = _write_40x40_fixture(d)
        tpl = os.path.join(d, "t.yaml")
        with open(tpl, "w") as fh:
            fh.write("model: m\ndata: /old\niters: 4\n")
        out_dir = os.path.join(d, "out")
        proc = _run(["--primary", primary, "--rejected-pool", rejected_pool, "--out-dir", out_dir,
                     "--floor", "1", "--valid-frac", "0.2", "--config-template", tpl])
        assert proc.returncode == 0, proc.stderr
        train = [json.loads(l) for l in open(os.path.join(out_dir, "train.jsonl"))]
        valid = [json.loads(l) for l in open(os.path.join(out_dir, "valid.jsonl"))]
        man = json.load(open(os.path.join(out_dir, "manifest.json")))
        assert valid and man["n_valid"] == len(valid)
        assert len(train) + len(valid) == man["pairing"]["n_paired_rows"]
        assert man["config_path"].endswith("config.yaml")
        assert f"data: {os.path.abspath(out_dir)}\n" in open(man["config_path"]).read()
        assert _validate_via_mlx_tune_train(out_dir)["ok"]
