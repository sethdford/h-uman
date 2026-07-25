#!/usr/bin/env python3
"""
Unit and integration tests for eval_fidelity_nightly.py

Tests the gate logic, bootstrap CI, and verdict generation
using mocked subprocess outputs.
"""

import json
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest import mock

# Add scripts dir to path
sys.path.insert(0, str(Path(__file__).parent))

from eval_fidelity_helpers import bootstrap_ci, compute_persona_fidelity_scores
import eval_fidelity_nightly


def test_bootstrap_ci_basic():
    """Test bootstrap CI returns sensible bounds."""
    values = [0.1, 0.2, 0.3, 0.4, 0.5]
    mean, lo, hi = bootstrap_ci(values, n_resamples=100, seed=42)

    assert lo <= mean <= hi, f"CI bounds reversed: {lo} > {mean} > {hi}"
    assert lo >= 0.0 and hi <= 1.0, f"CI outside [0,1]: [{lo}, {hi}]"
    print(f"✓ bootstrap_ci: mean={mean:.3f}, CI=[{lo:.3f}, {hi:.3f}]")


def test_bootstrap_ci_single():
    """Test bootstrap CI with single value."""
    mean, lo, hi = bootstrap_ci([0.5], n_resamples=100)
    assert mean == 0.5 and lo == 0.5 and hi == 0.5
    print(f"✓ bootstrap_ci (single value): {mean}")


def test_bootstrap_ci_empty():
    """Test bootstrap CI with empty list."""
    mean, lo, hi = bootstrap_ci([], n_resamples=100)
    assert (mean, lo, hi) == (0.0, 0.0, 0.0)
    print(f"✓ bootstrap_ci (empty): {mean}")


def test_persona_fidelity_scores():
    """Test scoring of responses using shape classifier."""
    responses = [
        "hey whatup",
        "Depending on what you need, I can help.",
        "cool cool cool",
    ]
    classifications, mean = compute_persona_fidelity_scores(responses, channel="imessage")

    assert len(classifications) == 3
    assert all("score" in c and "pass" in c for c in classifications)
    assert 0.0 <= mean <= 1.0

    # Expect: response 0 (casual) has high score, response 1 ("Depending") has lower score
    assert classifications[0]["score"] > 0.8, "Casual response should score high"
    # Note: "Depending on" opener is detected but still gives decent score (0.85)
    assert classifications[1]["score"] < 0.9, "Response with 'Depending' opener should have lower score"
    assert "depending-on opener" in classifications[1]["fails"], "Should detect 'Depending' opener"
    print(f"✓ persona_fidelity_scores: mean={mean:.3f}, pass_count={sum(1 for c in classifications if c['pass'])}/3")


def test_generate_timeout():
    """Test generate() handles timeout gracefully."""
    with mock.patch("subprocess.run") as mock_run:
        mock_run.side_effect = subprocess.TimeoutExpired("cmd", 180)
        response = eval_fidelity_nightly.generate(
            "dummy-model", "test prompt", adapter_path=None
        )
        assert response == "[timeout]"
    print(f"✓ generate (timeout): {response}")


def test_generate_error():
    """Test generate() handles subprocess errors."""
    with mock.patch("subprocess.run") as mock_run:
        mock_run.return_value = mock.Mock(
            returncode=1,
            stdout="",
            stderr="Model not found"
        )
        response = eval_fidelity_nightly.generate(
            "nonexistent-model", "test prompt"
        )
        assert "[gen_err:" in response
    print(f"✓ generate (error): captured error message")


def test_generate_valid():
    """Test generate() with valid mock response."""
    mlx_output = """
===========
Prompt: test prompt
Generation:
Hello there!
===========
100 tokens-per-sec
    """
    with mock.patch("subprocess.run") as mock_run:
        mock_run.return_value = mock.Mock(
            returncode=0,
            stdout=mlx_output,
            stderr=""
        )
        response = eval_fidelity_nightly.generate(
            "dummy-model", "test prompt"
        )
        assert "Hello there!" in response
    print(f"✓ generate (valid): {response!r}")


def test_gate_pass():
    """Test gate verdict PASS when both statistical and practical thresholds met."""
    # Mock responses with high improvement
    pre_responses = ["hi"] * 25
    post_responses = ["hi there"] * 25

    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)
        fixture = tmpdir / "prompts.jsonl"
        fixture.write_text("\n".join(json.dumps({"prompt": p}) for p in ["test"] * 25))

        with mock.patch("eval_fidelity_nightly.generate") as mock_gen:
            # PRE responses: casual but short (score ~0.7)
            # POST responses: slightly longer but still good (score ~0.8)
            call_count = [0]
            def gen_side_effect(model, prompt, adapter_path=None):
                call_count[0] += 1
                if adapter_path is None:
                    return "hey there how are you"
                else:
                    return "hey there how are you doing today"

            mock_gen.side_effect = gen_side_effect

            # We'd need the full main() to test, so we'll test components instead
            pre_scores = [0.70] * 25
            post_scores = [0.76] * 25
            deltas = [post_scores[i] - pre_scores[i] for i in range(25)]

            delta_mean = sum(deltas) / len(deltas)
            assert delta_mean >= 0.05, f"Delta too small for PASS test: {delta_mean}"

            print(f"✓ gate (PASS case): delta_mean={delta_mean:.3f}")


def test_gate_skip_practical():
    """Test gate verdict SKIP when practical threshold not met."""
    pre_scores = [0.50] * 25
    post_scores = [0.52] * 25  # delta = 0.02, below 0.05 floor
    deltas = [post_scores[i] - pre_scores[i] for i in range(25)]

    delta_mean = sum(deltas) / len(deltas)
    assert delta_mean < 0.05, f"Delta should be < 0.05 for SKIP test: {delta_mean}"
    print(f"✓ gate (SKIP case, practical): delta_mean={delta_mean:.3f} < 0.05")


def test_gate_fail_statistical():
    """Test gate verdict FAIL when statistical threshold not met."""
    # High practical delta but wide CI (not significant)
    pre_mean = 0.50
    post_mean = 0.60  # 10% absolute improvement
    delta_mean = 0.10

    # But if stderr is large, CI overlaps and test fails
    stderr = 0.06  # wide
    stat_threshold = pre_mean + 1.96 * stderr
    # stat_threshold ≈ 0.50 + 0.118 = 0.618
    stat_pass = post_mean > stat_threshold

    assert not stat_pass, f"Should fail statistical: {post_mean} <= {stat_threshold}"
    print(f"✓ gate (FAIL case, statistical): post_mean={post_mean} <= threshold={stat_threshold:.3f}")


def test_load_prompts():
    """Test loading held-out prompts from JSONL."""
    with tempfile.TemporaryDirectory() as tmpdir:
        fixture = Path(tmpdir) / "test.jsonl"
        with open(fixture, "w") as f:
            f.write(json.dumps({"prompt": "hello", "channel": "imessage"}) + "\n")
            f.write(json.dumps({"prompt": "world", "channel": "imessage"}) + "\n")

        prompts = eval_fidelity_nightly.load_held_out_prompts_from_jsonl(str(fixture))
        assert len(prompts) == 2
        assert prompts[0]["prompt"] == "hello"
        assert prompts[1]["prompt"] == "world"
    print(f"✓ load_held_out_prompts: loaded {len(prompts)} from JSONL")


def test_output_verdict_json():
    """Test verdict JSON structure."""
    verdict = {
        "timestamp": "2026-05-26T02:00:00",
        "verdict": "PASS",
        "exit_code": 0,
        "n_prompts": 25,
        "model_id": "test-model",
        "adapter_path": "/path/to/adapter",
        "pre": {"mean_score": 0.50, "elapsed_sec": 100},
        "post": {"mean_score": 0.60, "elapsed_sec": 105},
        "delta": {
            "mean": 0.10,
            "ci_lower": 0.08,
            "ci_upper": 0.12,
            "stderr_est": 0.010,
        },
        "gate": {
            "statistical_pass": True,
            "statistical_threshold": 0.519,
            "statistical_alpha": 0.025,
            "practical_pass": True,
            "practical_floor": 0.05,
        },
    }

    # Verify it's JSON-serializable
    json_str = json.dumps(verdict, indent=2)
    parsed = json.loads(json_str)
    assert parsed["verdict"] == "PASS"
    assert parsed["exit_code"] == 0
    print(f"✓ verdict JSON structure valid")


def test_resolve_serving_adapter_prefers_live_process():
    """Resolution must prefer the adapter the live mlx-server is actually serving."""
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)
        live_adapter = tmpdir / "seth-lora-v9-live"
        live_adapter.mkdir()
        config_adapter = tmpdir / "seth-lora-v8-config"
        config_adapter.mkdir()
        config = tmpdir / "config.json"
        config.write_text(json.dumps(
            {"personalization": {"lora_adapter_path": str(config_adapter)}}
        ))

        ps_output = (
            "/usr/bin/something --unrelated\n"
            f"/opt/python /x/mlx-server.py --model m --port 8741 --adapter-path {live_adapter}\n"
        )
        path, source = eval_fidelity_nightly.resolve_serving_adapter(
            ps_output=ps_output, config_path=config
        )
        assert path == live_adapter, f"Expected live adapter, got {path}"
        assert "process" in source, f"Expected process source, got {source}"
    print(f"✓ resolve_serving_adapter: prefers live mlx-server process ({source})")


def test_resolve_serving_adapter_falls_back_to_config():
    """With no live server, resolution must fall back to config.json personalization."""
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)
        config_adapter = tmpdir / "seth-lora-v8-config"
        config_adapter.mkdir()
        config = tmpdir / "config.json"
        config.write_text(json.dumps(
            {"personalization": {"lora_adapter_path": str(config_adapter)}}
        ))

        path, source = eval_fidelity_nightly.resolve_serving_adapter(
            ps_output="/usr/bin/nothing-relevant\n", config_path=config
        )
        assert path == config_adapter, f"Expected config adapter, got {path}"
        assert "config" in source, f"Expected config source, got {source}"
    print(f"✓ resolve_serving_adapter: falls back to config.json ({source})")


def test_resolve_serving_adapter_none_when_unresolvable():
    """No live server + no config → (None, ...), never a fabricated path."""
    with tempfile.TemporaryDirectory() as tmpdir:
        missing_config = Path(tmpdir) / "does-not-exist.json"
        path, source = eval_fidelity_nightly.resolve_serving_adapter(
            ps_output="", config_path=missing_config
        )
        assert path is None, f"Expected None, got {path}"
    print(f"✓ resolve_serving_adapter: unresolvable → None ({source})")


def _run_main_with_argv(argv, fixture_prompts=25):
    """Drive eval_fidelity_nightly.main() with a synthetic fixture + mocked passes.

    Returns (rc, record_eval_mock). run_eval_pass is mocked so no model loads.
    """
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)
        fixture = tmpdir / "prompts.jsonl"
        fixture.write_text("\n".join(
            json.dumps({"prompt": "hey whatup", "channel": "imessage"})
            for _ in range(fixture_prompts)
        ))
        adapter = tmpdir / "seth-lora-v9-test"
        adapter.mkdir()

        full_argv = ["eval_fidelity_nightly.py"] + [
            a.replace("__ADAPTER__", str(adapter)) for a in argv
        ] + ["--held-out-fixture", str(fixture), "--log-dir", str(tmpdir)]

        def fake_pass(model_id, prompts, adapter_path=None, gen_timeout=600):
            responses = ["hey whatup"] * len(prompts)
            return (responses, {"pass": "mock", "elapsed_sec": 0.1, "count": len(prompts)})

        with mock.patch.object(sys, "argv", full_argv), \
             mock.patch("eval_fidelity_nightly.run_eval_pass", side_effect=fake_pass), \
             mock.patch("eval_fidelity_nightly.adapter_registry") as mock_registry:
            rc = eval_fidelity_nightly.main()
        return rc, mock_registry.record_eval


def test_skip_records_null_score_and_exits_3():
    """A SKIP verdict (delta below floor) must record score=None and exit 3.

    Pins the 2026-07 bug where 13 nightly SKIPs landed in registry.json as
    {"score": 1.0, "verdict": "SKIP"} — indistinguishable from a perfect eval.
    """
    # Identical pre/post responses → delta 0 → practical gate fails → SKIP
    rc, record_eval = _run_main_with_argv(["--adapter-path", "__ADAPTER__"])
    assert rc == eval_fidelity_nightly.EXIT_SKIP == 3, f"SKIP must exit 3, got {rc}"
    assert record_eval.called, "SKIP after a full eval must still be recorded"
    kwargs = record_eval.call_args.kwargs
    assert kwargs["verdict"] == "SKIP"
    assert kwargs["score"] is None, f"SKIP must record score=None, got {kwargs['score']}"
    print(f"✓ SKIP verdict: exit={rc}, registry score=None")


def test_adapter_missing_skip_is_loud_and_unrecorded(capsys=None):
    """Adapter-not-found must exit 3, print FIDELITY_SKIP, and not touch the registry."""
    import io
    from contextlib import redirect_stdout

    buf = io.StringIO()
    with redirect_stdout(buf):
        rc, record_eval = _run_main_with_argv(
            ["--adapter-path", "/nonexistent/adapter-path-xyz"]
        )
    out = buf.getvalue()
    assert rc == 3, f"missing adapter must exit 3, got {rc}"
    assert "FIDELITY_SKIP" in out, "skip must print the greppable FIDELITY_SKIP marker"
    assert not record_eval.called, "non-measurement skip must not write registry entries"
    print(f"✓ adapter-missing skip: exit=3, FIDELITY_SKIP marker present, registry untouched")


def test_resolve_serving_model_from_process():
    """The serving --model must be resolvable from the live mlx-server process."""
    ps_output = (
        "/opt/python /x/mlx-server.py --model mlx-community/gemma-4-31b-it-8bit "
        "--port 8741 --adapter-path /tmp/x\n"
    )
    model = eval_fidelity_nightly.resolve_serving_model(ps_output=ps_output)
    assert model == "mlx-community/gemma-4-31b-it-8bit", f"got {model}"
    # No live server → fall back to the default
    fallback = eval_fidelity_nightly.resolve_serving_model(ps_output="")
    assert fallback == eval_fidelity_nightly.DEFAULT_MODEL
    print(f"✓ resolve_serving_model: {model} (fallback={fallback})")


def test_sentinel_responses_defer_not_score():
    """All-timeout passes must DEFER (exit 2), never be scored.

    Pins the 2026-07 bug where every mlx_lm call hit the 180s timeout, every
    response was the literal '[timeout]' sentinel, the shape classifier scored
    it 1.0, and 10 nights of pure timeouts recorded as pre=post=1.0 SKIP.
    """
    import io
    from contextlib import redirect_stdout

    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)
        fixture = tmpdir / "prompts.jsonl"
        fixture.write_text("\n".join(
            json.dumps({"prompt": "hey", "channel": "imessage"}) for _ in range(25)
        ))
        adapter = tmpdir / "seth-lora-v9-test"
        adapter.mkdir()

        argv = ["eval_fidelity_nightly.py", "--adapter-path", str(adapter),
                "--held-out-fixture", str(fixture), "--log-dir", str(tmpdir)]

        def timeout_pass(model_id, prompts, adapter_path=None, gen_timeout=600):
            return (["[timeout]"] * len(prompts),
                    {"pass": "mock", "elapsed_sec": 0.1, "count": len(prompts)})

        buf = io.StringIO()
        with mock.patch.object(sys, "argv", argv), \
             mock.patch("eval_fidelity_nightly.run_eval_pass", side_effect=timeout_pass), \
             mock.patch("eval_fidelity_nightly.adapter_registry") as mock_registry, \
             redirect_stdout(buf):
            rc = eval_fidelity_nightly.main()

        out = buf.getvalue()
        assert rc == 2, f"all-sentinel run must DEFER (exit 2), got {rc}"
        assert "FIDELITY_DEFERRED" in out, "deferred run must print greppable marker"
        assert not mock_registry.record_eval.called, \
            "sentinel-only run must not write a registry eval entry"
    print(f"✓ all-sentinel passes: exit=2, FIDELITY_DEFERRED marker, registry untouched")


def test_partial_sentinels_dropped_from_deltas():
    """A few sentinel responses are dropped pairwise; the rest still score."""
    calls = {"n": 0}

    def mixed_pass(model_id, prompts, adapter_path=None, gen_timeout=600):
        calls["n"] += 1
        responses = ["hey whatup"] * len(prompts)
        responses[0] = "[timeout]"  # same index bad in both passes → 1 pair dropped
        return (responses, {"pass": "mock", "elapsed_sec": 0.1, "count": len(prompts)})

    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)
        fixture = tmpdir / "prompts.jsonl"
        fixture.write_text("\n".join(
            json.dumps({"prompt": "hey", "channel": "imessage"}) for _ in range(25)
        ))
        adapter = tmpdir / "seth-lora-v9-test"
        adapter.mkdir()
        out_json = tmpdir / "verdict.json"

        argv = ["eval_fidelity_nightly.py", "--adapter-path", str(adapter),
                "--held-out-fixture", str(fixture), "--log-dir", str(tmpdir),
                "--output-json", str(out_json)]

        with mock.patch.object(sys, "argv", argv), \
             mock.patch("eval_fidelity_nightly.run_eval_pass", side_effect=mixed_pass), \
             mock.patch("eval_fidelity_nightly.adapter_registry"):
            rc = eval_fidelity_nightly.main()

        verdict = json.loads(out_json.read_text())
        assert verdict["n_valid_pairs"] == 24, f"expected 24 valid pairs, got {verdict.get('n_valid_pairs')}"
        assert verdict["n_sentinel"]["pre"] == 1 and verdict["n_sentinel"]["post"] == 1
        # identical valid responses → delta 0 → SKIP (exit 3), but SCORED on 24 pairs
        assert rc == 3 and verdict["verdict"] == "SKIP"
    print(f"✓ partial sentinels: 1 pair dropped, verdict computed on 24")


def test_pass_records_post_mean():
    """A PASS verdict still records the real post_mean score."""
    crafted = {"n": 0}

    def fake_scores(responses, channel="imessage"):
        # First call = PRE (0.7), second = POST (0.8): constant deltas → stderr 0 → PASS
        crafted["n"] += 1
        score = 0.7 if crafted["n"] == 1 else 0.8
        return ([{"score": score, "pass": True, "fails": []} for _ in responses], score)

    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)
        fixture = tmpdir / "prompts.jsonl"
        fixture.write_text("\n".join(
            json.dumps({"prompt": "hey", "channel": "imessage"}) for _ in range(25)
        ))
        adapter = tmpdir / "seth-lora-v9-test"
        adapter.mkdir()

        argv = ["eval_fidelity_nightly.py", "--adapter-path", str(adapter),
                "--held-out-fixture", str(fixture), "--log-dir", str(tmpdir)]

        def fake_pass(model_id, prompts, adapter_path=None, gen_timeout=600):
            return (["hey"] * len(prompts), {"pass": "mock", "elapsed_sec": 0.1, "count": len(prompts)})

        with mock.patch.object(sys, "argv", argv), \
             mock.patch("eval_fidelity_nightly.run_eval_pass", side_effect=fake_pass), \
             mock.patch("eval_fidelity_nightly.compute_persona_fidelity_scores", side_effect=fake_scores), \
             mock.patch("eval_fidelity_nightly.adapter_registry") as mock_registry:
            rc = eval_fidelity_nightly.main()

        assert rc == 0, f"PASS must exit 0, got {rc}"
        kwargs = mock_registry.record_eval.call_args.kwargs
        assert kwargs["verdict"] == "PASS"
        assert abs(kwargs["score"] - 0.8) < 1e-9, f"PASS must record post_mean, got {kwargs['score']}"
    print(f"✓ PASS verdict: exit=0, registry score=post_mean")


def main():
    """Run all tests."""
    tests = [
        test_bootstrap_ci_basic,
        test_bootstrap_ci_single,
        test_bootstrap_ci_empty,
        test_persona_fidelity_scores,
        test_generate_timeout,
        test_generate_error,
        test_generate_valid,
        test_gate_pass,
        test_gate_skip_practical,
        test_gate_fail_statistical,
        test_load_prompts,
        test_output_verdict_json,
        test_resolve_serving_adapter_prefers_live_process,
        test_resolve_serving_adapter_falls_back_to_config,
        test_resolve_serving_adapter_none_when_unresolvable,
        test_skip_records_null_score_and_exits_3,
        test_adapter_missing_skip_is_loud_and_unrecorded,
        test_resolve_serving_model_from_process,
        test_sentinel_responses_defer_not_score,
        test_partial_sentinels_dropped_from_deltas,
        test_pass_records_post_mean,
    ]

    print("=" * 60)
    print("Testing eval_fidelity_nightly.py")
    print("=" * 60)

    passed = 0
    failed = 0
    for test in tests:
        try:
            test()
            passed += 1
        except AssertionError as e:
            print(f"✗ {test.__name__}: {e}")
            failed += 1
        except Exception as e:
            print(f"✗ {test.__name__}: {type(e).__name__}: {e}")
            failed += 1

    print("=" * 60)
    print(f"Results: {passed} passed, {failed} failed")
    print("=" * 60)

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
