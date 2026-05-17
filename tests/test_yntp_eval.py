"""Sprint 11 / US-11.6 — `scripts/yntp_eval.py` test suite.

Covers the AC for the held-out next-utterance log-likelihood evaluator:

  - End-to-end on synthetic-5 fixture (CI fallback path)
  - Schema validation: malformed rows rejected
  - HU_YNTP_HOLDOUT env var routing works
  - Env-unset fallback to synthetic-5
  - AC-11.6.3 adversarial: broken-adapter mock → negative/zero delta and/or
    pad_rate ≥ threshold → gate FAIL
  - Honest baseline: base-vs-base → delta ≈ 0

All tests are deterministic, offline, and require no real model weights.
The MLX inference path is gated behind a NotImplementedError so the
aggregation + comparison + gate logic is verified end-to-end with
pre-recorded log-prob mocks.
"""
from __future__ import annotations

import importlib.util
import io
import json
import os
import pathlib
import subprocess
import sys
from contextlib import contextmanager
from typing import Iterator

import pytest


_HERE = pathlib.Path(__file__).resolve().parent
_REPO = _HERE.parent
_SCRIPT = _REPO / "scripts" / "yntp_eval.py"
_FIXTURES = _HERE / "fixtures"

_SYNTHETIC_5 = _FIXTURES / "yntp_synthetic_5.jsonl"
_GOOD_LOG = _FIXTURES / "yntp_good_adapter_log.jsonl"
_BROKEN_LOG = _FIXTURES / "sprint8_broken_yntp_log.jsonl"
_BASE_VS_BASE_LOG = _FIXTURES / "yntp_base_vs_base_log.jsonl"


# ── Module loader ─────────────────────────────────────────────────────────


def _load_module():
    spec = importlib.util.spec_from_file_location("yntp_eval", _SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules["yntp_eval"] = mod
    spec.loader.exec_module(mod)
    return mod


yntp_eval = _load_module()


# ── Env-var helper ────────────────────────────────────────────────────────


@contextmanager
def _env(**overrides: str) -> Iterator[None]:
    """Temporarily set env vars; restore on exit. None deletes the var."""
    sentinel = object()
    prior: dict = {}
    try:
        for k, v in overrides.items():
            prior[k] = os.environ.get(k, sentinel)
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        yield
    finally:
        for k, v in prior.items():
            if v is sentinel:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v  # type: ignore[assignment]


# ── Schema + loader tests ─────────────────────────────────────────────────


def test_synthetic_fixture_loads_5_rows():
    """The committed synthetic-5 fixture must always be 5 valid rows."""
    rows = yntp_eval.load_fixture(_SYNTHETIC_5)
    assert len(rows) == 5
    for row in rows:
        assert row.prompt
        assert row.continuation
        assert isinstance(row.metadata, dict)


def test_load_fixture_rejects_missing_prompt(tmp_path):
    """Missing required key → ValueError."""
    bad = tmp_path / "bad.jsonl"
    bad.write_text('{"continuation": "x"}\n', encoding="utf-8")
    with pytest.raises(ValueError, match="missing required key 'prompt'"):
        yntp_eval.load_fixture(bad)


def test_load_fixture_rejects_missing_continuation(tmp_path):
    bad = tmp_path / "bad.jsonl"
    bad.write_text('{"prompt": "x"}\n', encoding="utf-8")
    with pytest.raises(ValueError, match="missing required key 'continuation'"):
        yntp_eval.load_fixture(bad)


def test_load_fixture_rejects_empty_string(tmp_path):
    bad = tmp_path / "bad.jsonl"
    bad.write_text('{"prompt": "", "continuation": "x"}\n', encoding="utf-8")
    with pytest.raises(ValueError, match="non-empty string"):
        yntp_eval.load_fixture(bad)


def test_load_fixture_rejects_invalid_json(tmp_path):
    bad = tmp_path / "bad.jsonl"
    bad.write_text("{not-json\n", encoding="utf-8")
    with pytest.raises(ValueError, match="invalid JSON"):
        yntp_eval.load_fixture(bad)


def test_load_fixture_rejects_empty_file(tmp_path):
    bad = tmp_path / "bad.jsonl"
    bad.write_text("", encoding="utf-8")
    with pytest.raises(ValueError, match="fixture is empty"):
        yntp_eval.load_fixture(bad)


def test_load_fixture_missing_file_raises(tmp_path):
    with pytest.raises(FileNotFoundError):
        yntp_eval.load_fixture(tmp_path / "nope.jsonl")


# ── Fixture-resolution / D1 hybrid policy ─────────────────────────────────


def test_resolve_fixture_explicit_wins(tmp_path):
    """--fixture flag overrides everything."""
    p = tmp_path / "explicit.jsonl"
    p.write_text('{"prompt": "p", "continuation": "c"}\n', encoding="utf-8")
    with _env(HU_YNTP_HOLDOUT="/should/not/be/used"):
        resolved = yntp_eval.resolve_fixture(str(p))
    assert resolved == p


def test_resolve_fixture_env_var_routes():
    """HU_YNTP_HOLDOUT env var picks the production fixture."""
    with _env(HU_YNTP_HOLDOUT="/some/private/path.jsonl"):
        resolved = yntp_eval.resolve_fixture(None)
    assert str(resolved) == "/some/private/path.jsonl"


def test_resolve_fixture_falls_back_to_synthetic():
    """No flag + no env var → synthetic-5 in tests/fixtures/."""
    with _env(HU_YNTP_HOLDOUT=None):
        resolved = yntp_eval.resolve_fixture(None)
    assert resolved == _SYNTHETIC_5
    assert resolved.exists()


# ── Aggregation + gate unit tests ─────────────────────────────────────────


def test_aggregate_per_token_mean():
    """mean_ll = total_logprob / total_tokens (not row-mean)."""
    # row1: 5 tokens, sum -10 → -2/tok
    # row2: 10 tokens, sum -30 → -3/tok
    # weighted mean: -40/15 = -2.6667
    per_row = [(-10.0, 5, False), (-30.0, 10, False)]
    mean_ll, pad_rate = yntp_eval.aggregate(per_row, "test")
    assert mean_ll == pytest.approx(-40.0 / 15.0, abs=1e-9)
    assert pad_rate == 0.0


def test_aggregate_pad_rate():
    per_row = [(-10.0, 5, True), (-30.0, 10, False), (-15.0, 5, True)]
    _, pad_rate = yntp_eval.aggregate(per_row, "test")
    assert pad_rate == pytest.approx(2 / 3)


def test_aggregate_empty_returns_zero():
    mean_ll, pad_rate = yntp_eval.aggregate([], "test")
    assert mean_ll == 0.0
    assert pad_rate == 0.0


def test_decide_gate_pass():
    decision, notes = yntp_eval.decide_gate(
        base_mean_ll=-3.0, adapter_mean_ll=-2.5, adapter_pad_rate=0.1
    )
    assert decision == "PASS"
    assert any("delta_ll=0.5" in n for n in notes)


def test_decide_gate_fail_on_negative_delta():
    decision, notes = yntp_eval.decide_gate(
        base_mean_ll=-2.5, adapter_mean_ll=-3.0, adapter_pad_rate=0.0
    )
    assert decision == "FAIL"


def test_decide_gate_fail_on_zero_delta():
    """Zero delta is not improvement; must fail."""
    decision, _ = yntp_eval.decide_gate(
        base_mean_ll=-3.0, adapter_mean_ll=-3.0, adapter_pad_rate=0.0
    )
    assert decision == "FAIL"


def test_decide_gate_fail_on_pad_rate_overrides_positive_delta():
    """AC-11.6.3 regression guard: pad_rate veto overrides positive NLL delta."""
    decision, notes = yntp_eval.decide_gate(
        base_mean_ll=-3.0, adapter_mean_ll=-2.0, adapter_pad_rate=0.6
    )
    assert decision == "FAIL"
    assert any("pad_rate=0.600" in n for n in notes)


# ── End-to-end evaluator tests (CI path via mock log) ─────────────────────


def test_evaluate_good_adapter_passes_gate():
    """Good adapter mock → positive delta_ll, zero pad rate, gate PASS."""
    result = yntp_eval.evaluate(
        fixture_path=_SYNTHETIC_5,
        mock_log_path=_GOOD_LOG,
        adapter_path="/fake/adapter",
        base_model="gemma-4-e2b",
    )
    assert result.n_pairs == 5
    assert result.delta_ll > 0
    assert result.pad_rate == 0.0
    assert result.gate_decision == "PASS"
    assert result.adapter_mean_ll > result.base_mean_ll


def test_evaluate_broken_adapter_fails_gate_ac_11_6_3():
    """AC-11.6.3 — Sprint 8 broken adapter MUST fail YNTP gate.

    The fixture simulates the iter-200 collapse: coherent prompts get
    pad-token gibberish from the adapter, which means:
      - adapter_mean_ll is much worse than base (negative delta), AND
      - pad_rate is high (every row marked has_pad=True).
    Either failure mode alone is enough to fail the gate; this fixture
    triggers BOTH for defense in depth.
    """
    result = yntp_eval.evaluate(
        fixture_path=_SYNTHETIC_5,
        mock_log_path=_BROKEN_LOG,
        adapter_path="/fake/broken/adapter",
        base_model="gemma-4-e2b",
    )
    assert result.n_pairs == 5
    # Either condition is sufficient per the AC; the broken fixture trips both.
    assert result.delta_ll <= 0 or result.pad_rate >= yntp_eval.PAD_RATE_FAIL_THRESHOLD
    # And specifically: the gate decides FAIL.
    assert result.gate_decision == "FAIL"
    # Pad rate must be the dominant signal (since the broken adapter sprays pads).
    assert result.pad_rate >= yntp_eval.PAD_RATE_FAIL_THRESHOLD


def test_evaluate_base_vs_base_honest_baseline_zero_delta():
    """Sanity: base scored against base → delta_ll ≈ 0 → gate FAIL.

    Identical models must produce identical log-probs; this is the honest
    null-result. The gate fails because there's no improvement, which is
    correct: no adapter, no win.
    """
    result = yntp_eval.evaluate(
        fixture_path=_SYNTHETIC_5,
        mock_log_path=_BASE_VS_BASE_LOG,
        adapter_path="/no/adapter",
        base_model="gemma-4-e2b",
    )
    assert result.delta_ll == pytest.approx(0.0, abs=1e-9)
    assert result.pad_rate == 0.0
    assert result.gate_decision == "FAIL"


def test_evaluate_mock_log_row_count_mismatch_raises(tmp_path):
    """Mock log shorter than fixture → ValueError, no silent truncation."""
    short = tmp_path / "short.jsonl"
    short.write_text(
        '{"row_id": 1, "base_logprob_sum": -1.0, "base_n_tokens": 1, '
        '"adapter_logprob_sum": -1.0, "adapter_n_tokens": 1, "has_pad": false}\n',
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="Counts must match"):
        yntp_eval.evaluate(
            fixture_path=_SYNTHETIC_5,
            mock_log_path=short,
            adapter_path="/fake",
            base_model="gemma-4-e2b",
        )


# ── End-to-end CLI invocation tests ───────────────────────────────────────


def _run_cli(*args: str, env: dict = None) -> subprocess.CompletedProcess:
    """Run the script as a subprocess and return CompletedProcess.

    Doesn't raise on non-zero exit; tests assert on returncode explicitly.
    """
    full_env = os.environ.copy()
    if env:
        for k, v in env.items():
            if v is None:
                full_env.pop(k, None)
            else:
                full_env[k] = v
    return subprocess.run(
        [sys.executable, str(_SCRIPT), *args],
        capture_output=True,
        text=True,
        env=full_env,
    )


def test_cli_good_adapter_exits_zero():
    """CLI returns 0 when gate is PASS."""
    proc = _run_cli(
        "--mock-from-jsonl",
        str(_GOOD_LOG),
        "--fixture",
        str(_SYNTHETIC_5),
        env={"HU_YNTP_HOLDOUT": None},
    )
    assert proc.returncode == 0, f"stderr: {proc.stderr}"
    parsed = json.loads(proc.stdout)
    assert parsed["gate_decision"] == "PASS"
    assert parsed["delta_ll"] > 0
    assert parsed["n_pairs"] == 5


def test_cli_broken_adapter_exits_nonzero():
    """CLI returns 1 when gate is FAIL (Sprint 8 regression guard)."""
    proc = _run_cli(
        "--mock-from-jsonl",
        str(_BROKEN_LOG),
        "--fixture",
        str(_SYNTHETIC_5),
        env={"HU_YNTP_HOLDOUT": None},
    )
    assert proc.returncode == 1, f"stderr: {proc.stderr}"
    parsed = json.loads(proc.stdout)
    assert parsed["gate_decision"] == "FAIL"
    assert parsed["pad_rate"] >= yntp_eval.PAD_RATE_FAIL_THRESHOLD


def test_cli_output_schema_has_required_keys():
    """JSON output must contain the AC-11.6.1-required keys."""
    proc = _run_cli(
        "--mock-from-jsonl",
        str(_GOOD_LOG),
        "--fixture",
        str(_SYNTHETIC_5),
        env={"HU_YNTP_HOLDOUT": None},
    )
    parsed = json.loads(proc.stdout)
    required = {
        "base_mean_ll",
        "adapter_mean_ll",
        "delta_ll",
        "n_pairs",
        "pad_rate",
        "fixture_path",
        "gate_decision",
    }
    assert required.issubset(parsed.keys())


def test_cli_env_var_routing_works(tmp_path):
    """HU_YNTP_HOLDOUT routes CLI to the env-supplied fixture."""
    # Build a 1-row override fixture + a matching 1-row mock log.
    fx = tmp_path / "env_routed.jsonl"
    fx.write_text(
        '{"prompt": "p", "continuation": "c", "metadata": {"env_routed": true}}\n',
        encoding="utf-8",
    )
    log = tmp_path / "log.jsonl"
    log.write_text(
        '{"row_id": 1, "base_logprob_sum": -10.0, "base_n_tokens": 5, '
        '"adapter_logprob_sum": -5.0, "adapter_n_tokens": 5, "has_pad": false}\n',
        encoding="utf-8",
    )
    proc = _run_cli(
        "--mock-from-jsonl",
        str(log),
        env={"HU_YNTP_HOLDOUT": str(fx)},
    )
    assert proc.returncode == 0, f"stderr: {proc.stderr}"
    parsed = json.loads(proc.stdout)
    assert parsed["fixture_path"] == str(fx)
    assert parsed["n_pairs"] == 1


def test_cli_env_unset_falls_back_to_synthetic():
    """When HU_YNTP_HOLDOUT is unset, CLI uses synthetic-5 by default."""
    proc = _run_cli(
        "--mock-from-jsonl",
        str(_GOOD_LOG),
        env={"HU_YNTP_HOLDOUT": None},
    )
    assert proc.returncode == 0, f"stderr: {proc.stderr}"
    parsed = json.loads(proc.stdout)
    # The resolved fixture_path must be the synthetic-5 one.
    assert pathlib.Path(parsed["fixture_path"]).name == "yntp_synthetic_5.jsonl"


def test_cli_requires_adapter_or_mock():
    """Argparse-level error if neither --adapter nor --mock-from-jsonl set."""
    proc = _run_cli(env={"HU_YNTP_HOLDOUT": None})
    # argparse parser.error exits with code 2.
    assert proc.returncode == 2
    assert "must specify either --adapter" in proc.stderr


# ── No-real-weights enforcement ───────────────────────────────────────────


def test_real_mlx_path_not_implemented_in_ci():
    """Honest guard: the real MLX path is not wired in this story.

    When --adapter is passed without --mock-from-jsonl, we route to
    _real_compute_logprob which either ImportErrors (no mlx_lm) or
    NotImplementedErrors (bridge not built yet). Tests prove the test
    path doesn't accidentally hit MLX.
    """
    proc = _run_cli(
        "--adapter",
        "/fake/adapter",
        "--fixture",
        str(_SYNTHETIC_5),
        env={"HU_YNTP_HOLDOUT": None},
    )
    # Either exit code 3 (RuntimeError from mlx_lm import miss) or
    # 4 (NotImplementedError from the deferred bridge). Both are non-zero
    # and ensure no real weights were loaded.
    assert proc.returncode in (3, 4)


# ── PII / git-ignore enforcement ──────────────────────────────────────────


def test_private_holdout_is_gitignored():
    """The .gitignore must list the private-holdout location so it can
    never accidentally land in the repo per decisions.md D1."""
    gi = (_REPO / ".gitignore").read_text(encoding="utf-8")
    # `.human/private/` (matching at any depth) is the source-of-truth
    # gitignore entry the policy depends on.
    assert (
        ".human/private/" in gi or "**/.human/private/" in gi
    ), "expected `.human/private/` entry in .gitignore for D1 fixture policy"


def test_yntp_holdout_30_not_in_repo():
    """Sanity guard: the production-tier private fixture must never be
    present in the repo. If it is, fail loudly — D1 violates."""
    found = list(_REPO.rglob("yntp_holdout_30.jsonl"))
    # We allow no copies of this filename anywhere in the repo.
    assert found == [], (
        f"yntp_holdout_30.jsonl found in repo at {found}; this file MUST "
        "stay in ~/.human/private/ per decisions.md D1."
    )
