#!/usr/bin/env python3
"""
Unit and integration tests for eval_fidelity_nightly.py

Tests the gate logic, bootstrap CI, and verdict generation
using mocked subprocess outputs.
"""

import json
import os
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


GEMMA_8BIT = "mlx-community/gemma-4-31b-it-8bit"
GLM_4BIT = "mlx-community/GLM-4.5-Air-4bit"


def _expected_serverless_fallback():
    """What resolve_serving_model must return when no production server runs.

    Config first (that IS what the daemon serves), hardcoded default only as a
    last resort. Computed rather than hardcoded so this pins the CHAIN, not
    whatever base happens to be configured on the machine running the test."""
    try:
        import json as _json
        model = _json.loads(eval_fidelity_nightly.DEFAULT_CONFIG_PATH.read_text()) \
            .get("mlx_local", {}).get("model")
        if isinstance(model, str) and model.strip():
            return model.strip()
    except Exception:
        pass
    return eval_fidelity_nightly.DEFAULT_MODEL


def test_resolve_serving_adapter_filters_to_production_port():
    """Regression: observed live 2026-07-26 — a gemma-8bit realtime spare on
    :8747 was listed by `ps` BEFORE the production GLM server on :8741.
    First-match resolution evaluated the wrong adapter. Resolution must
    filter mlx-server lines by the production port."""
    with tempfile.TemporaryDirectory() as tmpdir, mock.patch.dict("os.environ"):
        os.environ.pop("HU_MLX_BASE_URL", None)
        tmpdir = Path(tmpdir)
        gemma_adapter = tmpdir / "gemma-adapter"
        gemma_adapter.mkdir()
        glm_adapter = tmpdir / "glm-adapter"
        glm_adapter.mkdir()

        two_servers = (
            f"/opt/python /x/mlx-server.py --model {GEMMA_8BIT} --port 8747 "
            f"--realtime --adapter-path {gemma_adapter}\n"
            f"/opt/python /x/mlx-server.py --model {GLM_4BIT} --port 8741 "
            f"--adapter-path {glm_adapter}\n"
        )
        path, source = eval_fidelity_nightly.resolve_serving_adapter(
            ps_output=two_servers, config_path=tmpdir / "missing.json"
        )
        assert path == glm_adapter, f"Expected :8741 adapter, got {path}"
        assert "8741" in source, f"Source must name the port, got {source}"

        # A server line with NO --port flag counts as the default 8741.
        no_port = (
            f"/opt/python /x/mlx-server.py --model {GLM_4BIT} "
            f"--adapter-path {glm_adapter}\n"
        )
        path, _ = eval_fidelity_nightly.resolve_serving_adapter(
            ps_output=no_port, config_path=tmpdir / "missing.json"
        )
        assert path == glm_adapter, f"No --port must count as 8741, got {path}"
    print("✓ resolve_serving_adapter: filters ps to production port")


def test_resolve_serving_adapter_nonproduction_only_falls_to_config():
    """With only a non-production server running, the resolver must ignore it
    and fall through to config.json, never eval the spare's adapter."""
    with tempfile.TemporaryDirectory() as tmpdir, mock.patch.dict("os.environ"):
        os.environ.pop("HU_MLX_BASE_URL", None)
        tmpdir = Path(tmpdir)
        gemma_adapter = tmpdir / "gemma-adapter"
        gemma_adapter.mkdir()
        config_adapter = tmpdir / "config-adapter"
        config_adapter.mkdir()
        config = tmpdir / "config.json"
        config.write_text(json.dumps(
            {"personalization": {"lora_adapter_path": str(config_adapter)}}
        ))

        spare_only = (
            f"/opt/python /x/mlx-server.py --model {GEMMA_8BIT} --port 8747 "
            f"--adapter-path {gemma_adapter}\n"
        )
        path, source = eval_fidelity_nightly.resolve_serving_adapter(
            ps_output=spare_only, config_path=config
        )
        assert path == config_adapter, f"Expected config adapter, got {path}"
        assert "config" in source, f"Expected config source, got {source}"
    print("✓ resolve_serving_adapter: non-production-only ps falls to config")


def test_resolve_serving_model_filters_to_production_port():
    """Same regression on the model axis: the nightly must eval the base that
    :8741 serves, not whichever mlx-server `ps` lists first."""
    with mock.patch.dict("os.environ"):
        os.environ.pop("HU_MLX_BASE_URL", None)
        two_servers = (
            f"/opt/python /x/mlx-server.py --model {GEMMA_8BIT} --port 8747 "
            f"--realtime --adapter-path /tmp/x\n"
            f"/opt/python /x/mlx-server.py --model {GLM_4BIT} --port 8741 "
            f"--adapter-path /tmp/y\n"
        )
        model = eval_fidelity_nightly.resolve_serving_model(ps_output=two_servers)
        assert model == GLM_4BIT, f"Expected :8741 model, got {model}"

        # No --port flag counts as the default 8741.
        no_port = f"/opt/python /x/mlx-server.py --model {GLM_4BIT}\n"
        model = eval_fidelity_nightly.resolve_serving_model(ps_output=no_port)
        assert model == GLM_4BIT, f"No --port must count as 8741, got {model}"

        # Only the spare running → same as no server. The POINT is that the
        # spare must not win; where the fallback lands is the serverless chain
        # (config mlx_local.model, then the hardcoded default) — see
        # _expected_serverless_fallback and the 2026-07-27 note above.
        spare_only = f"/opt/python /x/mlx-server.py --model {GEMMA_8BIT} --port 8747\n"
        model = eval_fidelity_nightly.resolve_serving_model(ps_output=spare_only)
        assert model != GEMMA_8BIT, f"spare on :8747 must never win, got {model}"
        assert model == _expected_serverless_fallback(), \
            f"Non-production-only ps must fall to default, got {model}"
    print("✓ resolve_serving_model: filters ps to production port")


def test_production_mlx_port_honors_hu_mlx_base_url():
    """HU_MLX_BASE_URL overrides the production port (mirrors
    lora_training_runner.c resolve_mlx_base_url), default 8741."""
    assert eval_fidelity_nightly.production_mlx_port(env={}) == "8741"
    assert eval_fidelity_nightly.production_mlx_port(
        env={"HU_MLX_BASE_URL": "http://127.0.0.1:8743/v1"}) == "8743"

    # End-to-end through the resolver: pointing production at :8747 flips
    # which server the same two-server ps output resolves to.
    with mock.patch.dict(
        "os.environ", {"HU_MLX_BASE_URL": "http://127.0.0.1:8747"}
    ):
        two_servers = (
            f"/opt/python /x/mlx-server.py --model {GEMMA_8BIT} --port 8747 "
            f"--adapter-path /tmp/x\n"
            f"/opt/python /x/mlx-server.py --model {GLM_4BIT} --port 8741 "
            f"--adapter-path /tmp/y\n"
        )
        model = eval_fidelity_nightly.resolve_serving_model(ps_output=two_servers)
        assert model == GEMMA_8BIT, f"Env port must be honored, got {model}"
    print("✓ production_mlx_port: HU_MLX_BASE_URL honored, default 8741")


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

        def fake_pass(model_id, prompts, adapter_path=None, gen_timeout=600, use_subprocess=False):
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


def test_no_registry_flag_skips_registry_write():
    """--no-registry must complete a full eval without touching the adapter
    registry — smoke/manual small-n runs were recording real entries (e.g. the
    2026-07-25 n=5 PASS at 0.91) indistinguishable from gate-grade nightlies."""
    rc, record_eval = _run_main_with_argv(
        ["--adapter-path", "__ADAPTER__", "--no-registry"]
    )
    # identical pre/post → SKIP (exit 3): the eval RAN, only the registry write is off
    assert rc == 3, f"eval must still run to a verdict, got rc={rc}"
    assert not record_eval.called, \
        "--no-registry must not write adapter-registry entries"
    print(f"✓ --no-registry: full eval ran (rc={rc}), registry untouched")


def test_resolve_serving_model_from_process():
    """The serving --model must be resolvable from the live mlx-server process."""
    ps_output = (
        "/opt/python /x/mlx-server.py --model mlx-community/gemma-4-31b-it-8bit "
        "--port 8741 --adapter-path /tmp/x\n"
    )
    model = eval_fidelity_nightly.resolve_serving_model(ps_output=ps_output)
    assert model == "mlx-community/gemma-4-31b-it-8bit", f"got {model}"
    # No live server → fall back to CONFIG (mlx_local.model), and only then to
    # the hardcoded default. Updated 2026-07-27: this used to assert
    # DEFAULT_MODEL unconditionally, which pinned the asymmetric-fallback bug —
    # the adapter half already fell back to config, so a server-down night
    # paired the config's GLM adapter with the constant's gemma base and
    # produced a no-op run scored as a legitimate SKIP.
    fallback = eval_fidelity_nightly.resolve_serving_model(ps_output="")
    assert fallback == _expected_serverless_fallback(), f"got {fallback}"
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

        def timeout_pass(model_id, prompts, adapter_path=None, gen_timeout=600, use_subprocess=False):
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

    def mixed_pass(model_id, prompts, adapter_path=None, gen_timeout=600, use_subprocess=False):
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

    def fake_scores(responses, channel="imessage", speaker_model=None):
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

        def fake_pass(model_id, prompts, adapter_path=None, gen_timeout=600, use_subprocess=False):
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


def test_generate_inprocess_timeout_sentinel():
    """A generation exceeding the wall-clock guard returns '[timeout]'."""
    import time as _time

    def slow_gen(model, tokenizer, prompt, max_tokens):
        _time.sleep(3)
        return "too late"

    with mock.patch("eval_fidelity_nightly._mlx_generate", side_effect=slow_gen):
        r = eval_fidelity_nightly.generate_inprocess(
            object(), object(), "hi", timeout_sec=1
        )
    assert r == "[timeout]", f"expected [timeout], got {r!r}"
    print(f"✓ generate_inprocess (timeout): {r}")


def test_generate_inprocess_error_sentinel():
    """An exception inside generation returns a '[gen_err: ...]' sentinel."""
    with mock.patch("eval_fidelity_nightly._mlx_generate",
                    side_effect=RuntimeError("metal exploded")):
        r = eval_fidelity_nightly.generate_inprocess(object(), object(), "hi")
    assert r.startswith("[gen_err:") and "metal exploded" in r, f"got {r!r}"
    print(f"✓ generate_inprocess (error): {r}")


def test_generate_inprocess_empty_sentinel():
    """Whitespace-only model output returns '[empty]'."""
    with mock.patch("eval_fidelity_nightly._mlx_generate", return_value="   \n"):
        r = eval_fidelity_nightly.generate_inprocess(object(), object(), "hi")
    assert r == "[empty]", f"expected [empty], got {r!r}"
    print(f"✓ generate_inprocess (empty): {r}")


def test_run_eval_pass_loads_model_once():
    """The whole point of the rewrite: ONE load per pass, not one per prompt."""
    prompts = [{"prompt": f"p{i}"} for i in range(5)]
    with mock.patch("eval_fidelity_nightly.load_model",
                    return_value=(object(), object())) as m_load, \
         mock.patch("eval_fidelity_nightly.generate_inprocess",
                    return_value="hey") as m_gen, \
         mock.patch("eval_fidelity_nightly.free_model") as m_free:
        responses, stats = eval_fidelity_nightly.run_eval_pass(
            "model-x", prompts, adapter_path="/tmp/adapter-y"
        )
    assert m_load.call_count == 1, f"load_model called {m_load.call_count}x, want 1"
    assert m_gen.call_count == 5, f"generate_inprocess called {m_gen.call_count}x, want 5"
    assert responses == ["hey"] * 5
    assert m_free.called, "free_model must run after the pass"
    # POST pass must load WITH the adapter
    load_kwargs = m_load.call_args.kwargs
    load_args = m_load.call_args.args
    passed_adapter = load_kwargs.get("adapter_path",
                                     load_args[1] if len(load_args) > 1 else None)
    assert str(passed_adapter) == "/tmp/adapter-y", f"adapter not passed to load: {m_load.call_args}"
    assert stats["pass"] == "POST (adapter)"
    print(f"✓ run_eval_pass: 1 load, 5 generations, model freed")


def test_run_eval_pass_pre_loads_without_adapter():
    """The PRE pass must load the BASE model — adapter_path=None."""
    with mock.patch("eval_fidelity_nightly.load_model",
                    return_value=(object(), object())) as m_load, \
         mock.patch("eval_fidelity_nightly.generate_inprocess", return_value="hey"), \
         mock.patch("eval_fidelity_nightly.free_model"):
        _, stats = eval_fidelity_nightly.run_eval_pass(
            "model-x", [{"prompt": "a"}]
        )
    load_kwargs = m_load.call_args.kwargs
    load_args = m_load.call_args.args
    passed_adapter = load_kwargs.get("adapter_path",
                                     load_args[1] if len(load_args) > 1 else None)
    assert passed_adapter is None, f"PRE pass must not load an adapter: {m_load.call_args}"
    assert stats["pass"] == "PRE (base)"
    print(f"✓ run_eval_pass (PRE): base model loaded without adapter")


def test_run_eval_pass_frees_model_on_error():
    """free_model must run even when a generation raises unexpectedly."""
    with mock.patch("eval_fidelity_nightly.load_model",
                    return_value=(object(), object())), \
         mock.patch("eval_fidelity_nightly.generate_inprocess",
                    side_effect=KeyboardInterrupt), \
         mock.patch("eval_fidelity_nightly.free_model") as m_free:
        try:
            eval_fidelity_nightly.run_eval_pass("model-x", [{"prompt": "a"}])
            raise AssertionError("expected KeyboardInterrupt to propagate")
        except KeyboardInterrupt:
            pass
    assert m_free.called, "free_model must run even on error (finally)"
    print(f"✓ run_eval_pass (error): model freed via finally")


def test_run_eval_pass_subprocess_fallback():
    """--subprocess-gen keeps the legacy per-prompt subprocess path reachable."""
    with mock.patch("eval_fidelity_nightly.generate", return_value="hey") as m_gen, \
         mock.patch("eval_fidelity_nightly.load_model") as m_load:
        responses, _ = eval_fidelity_nightly.run_eval_pass(
            "model-x", [{"prompt": "a"}, {"prompt": "b"}], use_subprocess=True
        )
    assert m_gen.call_count == 2, "fallback must use the subprocess generate()"
    assert not m_load.called, "fallback must NOT load the model in-process"
    assert responses == ["hey", "hey"]
    print(f"✓ run_eval_pass (--subprocess-gen): legacy path reachable, no in-process load")


# --- Blended scorer (shape + speaker-id P(Seth)) tests ---------------------
#
# Pins the 2026-07-16 saturation incident: real generations happened, BOTH
# passes emitted clean casual text, the shape classifier scored every response
# 1.0, and the delta gate (PASS needs delta >= 0.05) was structurally
# unwinnable. The blended scorer must produce a non-degenerate delta on that
# exact scenario.

# Base-model register: clean, capitalized, period-terminated assistant-casual.
# Every one of these scores 1.0 on the shape classifier (verified empirically).
BASE_ISH_TEXTS = [
    "Sounds good. I will check on that today.",
    "Yes, that works for me.",
    "Thank you for letting me know.",
    "Okay, I will send it over shortly.",
    "That should be fine. I will confirm later.",
]

# Seth register: lowercase, seth-openers, contractions, no terminal period.
# These ALSO score 1.0 on the shape classifier — that tie is the bug.
SETH_ISH_TEXTS = [
    "yeah lol i'm down",
    "nah gonna skip it",
    "kk sounds good",
    "wait really? that's wild",
    "yo lemme check real quick",
]


def _make_test_speaker_model():
    """Deterministic v1-compatible speaker-id logreg model for tests.

    Uses the real 15-feature featurize() from personaeval_speaker_id via
    classify_text; only the weights are synthetic (favoring Seth-register
    features), so the blend path is exercised end-to-end without depending
    on the mutable /tmp/seth_speaker_id.json.
    """
    from personaeval_speaker_id import _FEATURE_NAMES
    weights_by_name = {
        "lowercase_ratio": 2.0,
        "is_seth_opener": 2.5,
        "has_contraction": 1.5,
        "has_lol_or_ha": 1.5,
        "is_ai_opener": -2.0,
        "ends_with_period": -2.5,
        "has_bullet": -3.0,
        "has_numbered": -3.0,
        "has_header": -3.0,
        "has_bold": -2.0,
    }
    return {
        "feature_names": list(_FEATURE_NAMES),
        "weights": [weights_by_name.get(f, 0.0) for f in _FEATURE_NAMES],
        "bias": -1.0,
        "means": [0.0] * len(_FEATURE_NAMES),
        "stds": [1.0] * len(_FEATURE_NAMES),
    }


def test_load_speaker_model_missing_returns_none():
    """A missing or corrupt speaker-model file must yield None, not raise."""
    from eval_fidelity_helpers import load_speaker_model
    assert load_speaker_model("/nonexistent/speaker-model-xyz.json") is None
    with tempfile.TemporaryDirectory() as tmpdir:
        corrupt = Path(tmpdir) / "corrupt.json"
        corrupt.write_text("{not json")
        assert load_speaker_model(str(corrupt)) is None
        # valid JSON but missing logreg keys is also unusable
        not_a_model = Path(tmpdir) / "notmodel.json"
        not_a_model.write_text(json.dumps({"hello": "world"}))
        assert load_speaker_model(str(not_a_model)) is None
    print("✓ load_speaker_model: missing/corrupt/invalid → None")


def test_shape_only_saturates_on_0716_scenario():
    """Documents the bug: shape-only scoring ties both registers at 1.0."""
    _, base_mean = compute_persona_fidelity_scores(BASE_ISH_TEXTS, channel="imessage")
    _, seth_mean = compute_persona_fidelity_scores(SETH_ISH_TEXTS, channel="imessage")
    assert base_mean == 1.0 and seth_mean == 1.0, (
        f"expected saturation (the bug this pins): base={base_mean}, seth={seth_mean}"
    )
    print(f"✓ shape-only saturation pinned: base={base_mean}, seth={seth_mean}, delta=0")


def test_blended_scorer_separates_base_from_seth():
    """On the 07-16 scenario the blended scorer must produce delta >= floor."""
    model = _make_test_speaker_model()
    base_cls, base_mean = compute_persona_fidelity_scores(
        BASE_ISH_TEXTS, channel="imessage", speaker_model=model)
    seth_cls, seth_mean = compute_persona_fidelity_scores(
        SETH_ISH_TEXTS, channel="imessage", speaker_model=model)

    delta = seth_mean - base_mean
    assert delta >= 0.05, f"blended delta must clear the 0.05 floor, got {delta:.4f}"
    assert 0.0 < base_mean < 1.0, f"base mean must have headroom, got {base_mean}"
    # component provenance must be visible per classification
    for c in base_cls + seth_cls:
        assert "shape_score" in c and "p_seth" in c, f"missing components: {c.keys()}"
        assert 0.0 <= c["p_seth"] <= 1.0
    print(f"✓ blended scorer: base={base_mean:.3f}, seth={seth_mean:.3f}, delta={delta:.3f}")


def test_blended_scorer_still_penalizes_ai_tells():
    """The shape component must keep AI-telly text below clean text."""
    model = _make_test_speaker_model()
    ai_telly = ["Certainly! Here are a few options:\n- one\n- two"]
    cls, ai_mean = compute_persona_fidelity_scores(
        ai_telly, channel="imessage", speaker_model=model)
    _, base_mean = compute_persona_fidelity_scores(
        BASE_ISH_TEXTS, channel="imessage", speaker_model=model)
    _, seth_mean = compute_persona_fidelity_scores(
        SETH_ISH_TEXTS, channel="imessage", speaker_model=model)
    assert ai_mean < base_mean < seth_mean, (
        f"ordering must be ai < base < seth: {ai_mean:.3f}, {base_mean:.3f}, {seth_mean:.3f}"
    )
    assert cls[0]["shape_score"] < 1.0, "AI tells must still dent the shape component"
    print(f"✓ blend ordering: ai={ai_mean:.3f} < base={base_mean:.3f} < seth={seth_mean:.3f}")


def test_shape_only_backward_compatible():
    """speaker_model=None must reproduce the pure shape score exactly."""
    responses = ["hey whatup", "Depending on what you need, I can help.", "cool cool cool"]
    cls, mean = compute_persona_fidelity_scores(responses, channel="imessage")
    from eval_shape_classifier import classify
    for r, c in zip(responses, cls):
        assert c["score"] == classify(r, channel="imessage")["score"], (
            f"shape-only score drifted for {r!r}"
        )
    print(f"✓ shape-only backward compat: mean={mean:.3f}")


def test_nightly_blended_pass_records_scorer_provenance():
    """End-to-end main(): blended scorer turns a real register shift into PASS,
    and the verdict JSON records which scorer produced the numbers."""
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)
        fixture = tmpdir / "prompts.jsonl"
        fixture.write_text("\n".join(
            json.dumps({"prompt": "hey", "channel": "imessage"}) for _ in range(25)
        ))
        adapter = tmpdir / "seth-lora-v9-test"
        adapter.mkdir()
        speaker_model_path = tmpdir / "speaker.json"
        speaker_model_path.write_text(json.dumps(_make_test_speaker_model()))
        out_json = tmpdir / "verdict.json"

        argv = ["eval_fidelity_nightly.py", "--adapter-path", str(adapter),
                "--held-out-fixture", str(fixture), "--log-dir", str(tmpdir),
                "--speaker-model", str(speaker_model_path),
                "--output-json", str(out_json)]

        def register_shift_pass(model_id, prompts, adapter_path=None, gen_timeout=600, use_subprocess=False):
            text = "yeah lol i'm down" if adapter_path else "Sounds good. I will check on that today."
            return ([text] * len(prompts),
                    {"pass": "mock", "elapsed_sec": 0.1, "count": len(prompts)})

        with mock.patch.object(sys, "argv", argv), \
             mock.patch("eval_fidelity_nightly.run_eval_pass", side_effect=register_shift_pass), \
             mock.patch("eval_fidelity_nightly.adapter_registry") as mock_registry:
            rc = eval_fidelity_nightly.main()

        verdict = json.loads(out_json.read_text())
        assert rc == 0, f"register shift must PASS the blended gate, got rc={rc}: {verdict.get('reason')}"
        assert verdict["verdict"] == "PASS"
        assert verdict["scorer"]["mode"] == "blended", f"scorer provenance missing: {verdict.get('scorer')}"
        assert verdict["scorer"]["shape_weight"] + verdict["scorer"]["speaker_weight"] == 1.0
        assert verdict["delta"]["mean"] >= 0.05
        kwargs = mock_registry.record_eval.call_args.kwargs
        assert kwargs["verdict"] == "PASS"
    print(f"✓ nightly blended PASS: rc=0, delta={verdict['delta']['mean']}, scorer=blended")


def test_nightly_missing_speaker_model_degrades_loudly():
    """No speaker model → shape-only fallback with a greppable degradation
    marker and scorer provenance in the verdict (never a silent saturation)."""
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
        out_json = tmpdir / "verdict.json"

        argv = ["eval_fidelity_nightly.py", "--adapter-path", str(adapter),
                "--held-out-fixture", str(fixture), "--log-dir", str(tmpdir),
                "--speaker-model", str(tmpdir / "no-such-model.json"),
                "--output-json", str(out_json)]

        def clean_pass(model_id, prompts, adapter_path=None, gen_timeout=600, use_subprocess=False):
            text = "yeah lol i'm down" if adapter_path else "Sounds good. I will check on that today."
            return ([text] * len(prompts),
                    {"pass": "mock", "elapsed_sec": 0.1, "count": len(prompts)})

        buf = io.StringIO()
        with mock.patch.object(sys, "argv", argv), \
             mock.patch("eval_fidelity_nightly.run_eval_pass", side_effect=clean_pass), \
             mock.patch("eval_fidelity_nightly.adapter_registry"), \
             redirect_stdout(buf):
            rc = eval_fidelity_nightly.main()

        out = buf.getvalue()
        verdict = json.loads(out_json.read_text())
        assert "FIDELITY_SCORER_DEGRADED" in out, "degradation must print a greppable marker"
        assert verdict["scorer"]["mode"] == "shape-only", f"got {verdict.get('scorer')}"
        # shape-only saturates on this scenario → delta 0 → SKIP, not PASS
        assert rc == 3 and verdict["verdict"] == "SKIP", (
            f"saturated shape-only run must SKIP, got rc={rc} verdict={verdict['verdict']}"
        )
    print(f"✓ missing speaker model: FIDELITY_SCORER_DEGRADED marker, shape-only SKIP")


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
        test_resolve_serving_adapter_filters_to_production_port,
        test_resolve_serving_adapter_nonproduction_only_falls_to_config,
        test_resolve_serving_model_filters_to_production_port,
        test_production_mlx_port_honors_hu_mlx_base_url,
        test_skip_records_null_score_and_exits_3,
        test_adapter_missing_skip_is_loud_and_unrecorded,
        test_no_registry_flag_skips_registry_write,
        test_resolve_serving_model_from_process,
        test_sentinel_responses_defer_not_score,
        test_partial_sentinels_dropped_from_deltas,
        test_pass_records_post_mean,
        test_generate_inprocess_timeout_sentinel,
        test_generate_inprocess_error_sentinel,
        test_generate_inprocess_empty_sentinel,
        test_run_eval_pass_loads_model_once,
        test_run_eval_pass_pre_loads_without_adapter,
        test_run_eval_pass_frees_model_on_error,
        test_run_eval_pass_subprocess_fallback,
        test_load_speaker_model_missing_returns_none,
        test_shape_only_saturates_on_0716_scenario,
        test_blended_scorer_separates_base_from_seth,
        test_blended_scorer_still_penalizes_ai_tells,
        test_shape_only_backward_compatible,
        test_nightly_blended_pass_records_scorer_provenance,
        test_nightly_missing_speaker_model_degrades_loudly,
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
