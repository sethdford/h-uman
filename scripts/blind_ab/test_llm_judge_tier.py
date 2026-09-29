import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import llm_judge_tier as lj


def _fake_always_correct(context, a_text, b_text, real_seth):
    """A fake judge that always identifies the real reply correctly, whichever
    slot it landed in -- proves order-unshuffle: judge_trial must record
    human_is as the slot that ACTUALLY holds real_seth, not a fixed slot."""
    choice = "A" if a_text == real_seth else "B"
    return {"choice": choice, "confidence": 9, "reasoning": "matched"}


def test_assign_order_is_deterministic_per_seed_and_index():
    assert lj.assign_order(1337, 0) == lj.assign_order(1337, 0)
    seq_a = [lj.assign_order(7, i) for i in range(20)]
    seq_b = [lj.assign_order(7, i) for i in range(20)]
    assert seq_a == seq_b
    # Not degenerate -- both letters actually occur over 20 draws.
    assert "A" in seq_a and "B" in seq_a


def test_order_unshuffle_records_the_true_slot():
    """judge_trial must record human_is == the slot real_seth was actually
    placed in, for BOTH coin outcomes -- this is the bug shape where a judge
    wrapper forgets it shuffled and reports the pre-shuffle assumption."""
    real, ai = "the real one", "the ai one"
    saw_a_slot, saw_b_slot = False, False
    for i in range(20):
        human_is = lj.assign_order(1337, i)
        row = lj.judge_trial(
            f"t{i}", "some context", real, ai, seed=1337, i=i,
            judge_fn=lambda ctx, a, b: _fake_always_correct(ctx, a, b, real),
        )
        assert row["human_is"] == human_is
        # The always-correct fake judge proves the mapping is right: since it
        # picks whichever slot literally equals `real`, and judge_trial scores
        # correct = (choice == human_is), correct must be True for every trial
        # iff human_is truly names the slot holding `real`.
        assert row["correct"] is True
        assert row["confidence"] == 9
        # No reply text leaks into the row.
        assert real not in json.dumps(row) and ai not in json.dumps(row)
        if human_is == "A":
            saw_a_slot = True
        else:
            saw_b_slot = True
    assert saw_a_slot and saw_b_slot


def test_order_unshuffle_detects_a_fooled_judge():
    """A judge that ALWAYS says 'A' must be scored correct only on the trials
    where the coin actually put the real reply in slot A -- if the mapping
    bookkeeping were wrong (e.g. human_is hardcoded to 'A'), every trial would
    read 'correct', hiding the fooling."""
    real, ai = "real text", "ai text"
    rows = [
        lj.judge_trial(f"t{i}", "ctx", real, ai, seed=99, i=i,
                        judge_fn=lambda ctx, a, b: {"choice": "A", "confidence": 5, "reasoning": "x"})
        for i in range(30)
    ]
    n_a = sum(1 for i in range(30) if lj.assign_order(99, i) == "A")
    n_correct = sum(1 for r in rows if r["correct"])
    assert n_correct == n_a
    assert 0 < n_correct < 30  # both outcomes occur -- the test would be vacuous otherwise


def test_compute_auc_perfect_separation():
    assert lj.compute_auc([1.0, 0.9, 0.8], [0.2, 0.1, 0.0]) == 1.0


def test_compute_auc_perfect_anti_separation():
    assert lj.compute_auc([0.0, 0.1], [0.9, 1.0]) == 0.0


def test_compute_auc_all_tied_is_a_half():
    assert lj.compute_auc([0.5, 0.5, 0.5], [0.5, 0.5, 0.5]) == 0.5


def test_compute_auc_empty_side_is_none():
    assert lj.compute_auc([], [0.1]) is None
    assert lj.compute_auc([0.1], []) is None


def test_score_for_auc_sums_to_one_and_direction():
    h, a = lj.score_for_auc({"correct": True, "confidence": 10})
    assert abs((h + a) - 1.0) < 1e-9
    assert h > 0.5 > a
    h2, a2 = lj.score_for_auc({"correct": False, "confidence": 10})
    assert h2 < 0.5 < a2
    # Higher confidence pushes the score further from the midpoint either way.
    h_lo, _ = lj.score_for_auc({"correct": True, "confidence": 1})
    h_hi, _ = lj.score_for_auc({"correct": True, "confidence": 10})
    assert h_hi > h_lo > 0.5


def test_run_refuses_below_min_trials():
    trials = [{"context": "c", "real_seth": "r", "ai_response": "a"}] * 5
    try:
        lj.run(trials, min_trials=20, judge_fn=lambda *a: {"choice": "A", "confidence": 5})
        assert False, "expected SystemExit"
    except SystemExit as e:
        assert "REFUSING" in str(e)


def test_run_with_perfect_fake_judge_gives_accuracy_one_and_auc_one():
    trials = [
        {"i": f"item_{i:02d}", "context": f"ctx {i}", "real_seth": f"real {i}", "ai_response": f"ai {i}"}
        for i in range(25)
    ]

    def perfect_judge(context, a_text, b_text):
        real = context.replace("ctx", "real")
        return {"choice": "A" if a_text == real else "B", "confidence": 8, "reasoning": "x"}

    summary = lj.run(trials, seed=42, min_trials=20, judge_fn=perfect_judge)
    assert summary["n"] == 25
    assert summary["accuracy"] == 1.0
    assert summary["auc"] == 1.0
    assert len(summary["rows"]) == 25
    dumped = json.dumps(summary)
    for t in trials:
        assert t["real_seth"] not in dumped
        assert t["ai_response"] not in dumped
        assert t["context"] not in dumped


def test_run_with_coinflip_fake_judge_accuracy_near_half():
    """A judge with zero signal (always guesses 'A') should land near 50%
    accuracy over enough trials -- sanity check that judge_trial's correctness
    math isn't secretly biased toward one branch."""
    trials = [
        {"i": f"item_{i:02d}", "context": f"ctx{i}", "real_seth": f"real{i}", "ai_response": f"ai{i}"}
        for i in range(200)
    ]
    summary = lj.run(trials, seed=7, min_trials=20,
                      judge_fn=lambda ctx, a, b: {"choice": "A", "confidence": 5, "reasoning": "x"})
    assert 0.35 < summary["accuracy"] < 0.65


def test_fake_judge_fn_is_deterministic_and_no_network():
    os.environ["HU_JUDGE_FAKE_P_A"] = "1"
    os.environ["HU_JUDGE_FAKE_CONFIDENCE"] = "6"
    try:
        j1 = lj.fake_judge_fn("ctx", "a", "b")
        j2 = lj.fake_judge_fn("ctx", "a", "b")
        assert j1 == j2 == {"choice": "A", "confidence": 6, "reasoning": "fake"}
    finally:
        os.environ.pop("HU_JUDGE_FAKE_P_A", None)
        os.environ.pop("HU_JUDGE_FAKE_CONFIDENCE", None)


def test_main_end_to_end_with_HU_JUDGE_FAKE_env_writes_no_reply_text():
    """Full subprocess run through main(), gated by HU_JUDGE_FAKE=1 so it never
    touches the network or requires ADC -- proves the CLI wiring, the
    min-trials refusal, and the no-reply-text-in-output contract together."""
    d = tempfile.mkdtemp()
    trials_path = os.path.join(d, "trials.json")
    out_path = os.path.join(d, "out.json")
    trials = {"trials": [
        {"i": f"item_{i:02d}", "context": f"a secret context {i}",
         "real_seth": f"a secret real reply {i}", "ai_response": f"a secret ai reply {i}"}
        for i in range(24)
    ]}
    json.dump(trials, open(trials_path, "w"))

    env = dict(os.environ, HU_JUDGE_FAKE="1")
    r = subprocess.run(
        [sys.executable, os.path.join(HERE, "llm_judge_tier.py"),
         "--trials", trials_path, "--out", out_path, "--min-trials", "20"],
        capture_output=True, text=True, env=env, timeout=30,
    )
    assert r.returncode == 0, r.stderr
    assert os.path.exists(out_path)
    written = open(out_path).read()
    for i in range(24):
        assert f"secret context {i}" not in written
        assert f"secret real reply {i}" not in written
        assert f"secret ai reply {i}" not in written
    summary = json.loads(written)
    assert summary["n"] == 24
    assert 0.0 <= summary["accuracy"] <= 1.0
    assert summary["auc"] is None or 0.0 <= summary["auc"] <= 1.0
    assert set(summary["rows"][0].keys()) == {"id", "human_is", "judge_choice", "confidence", "correct"}


def test_main_refuses_below_min_trials_and_writes_nothing():
    d = tempfile.mkdtemp()
    trials_path = os.path.join(d, "trials.json")
    out_path = os.path.join(d, "out.json")
    json.dump({"trials": [{"i": "a", "context": "c", "real_seth": "r", "ai_response": "x"}] * 5},
              open(trials_path, "w"))
    env = dict(os.environ, HU_JUDGE_FAKE="1")
    r = subprocess.run(
        [sys.executable, os.path.join(HERE, "llm_judge_tier.py"),
         "--trials", trials_path, "--out", out_path, "--min-trials", "20"],
        capture_output=True, text=True, env=env, timeout=30,
    )
    assert r.returncode != 0
    assert "REFUSING" in r.stderr
    assert not os.path.exists(out_path)


def test_main_refuses_without_adc_when_not_faked(monkeypatch):
    d = tempfile.mkdtemp()
    trials_path = os.path.join(d, "trials.json")
    out_path = os.path.join(d, "out.json")
    json.dump({"trials": [
        {"i": f"i{i}", "context": "c", "real_seth": "r", "ai_response": "x"} for i in range(25)
    ]}, open(trials_path, "w"))
    env = dict(os.environ)
    env.pop("HU_JUDGE_FAKE", None)
    env.pop("GEMINI_API_KEY", None)
    env["HOME"] = tempfile.mkdtemp()  # no ~/.config/gcloud/application_default_credentials.json here
    r = subprocess.run(
        [sys.executable, os.path.join(HERE, "llm_judge_tier.py"),
         "--trials", trials_path, "--out", out_path],
        capture_output=True, text=True, env=env, timeout=30,
    )
    assert r.returncode != 0
    assert "REFUSING" in r.stderr
    assert not os.path.exists(out_path)


if __name__ == "__main__":
    import pytest
    raise SystemExit(pytest.main([__file__, "-v"]))
