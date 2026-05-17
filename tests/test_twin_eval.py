"""Sprint 11 / US-11.10 — `scripts/twin_eval.py` test suite.

Covers all AC for the Twin-2K-500 forced-choice evaluator:

  AC-11.10.1: Output schema {n_questions, adapter_accuracy, base_accuracy,
              delta_accuracy, stderr}.
  AC-11.10.2: Accuracy computation — 4/5 adapter vs 2/5 base → delta=+0.40.
  AC-11.10.3: Fixture schema validation; malformed rows rejected with
              line number.
  AC-11.10.4: HU_IS_TEST guard — no real inference; mock seam is exercised.
  AC-11.10.5: tests/fixtures/twin2k_synthetic_10q.jsonl loads and works in CI.
  AC-11.10.6: HU_TWIN2K_HOLDOUT env var routing + synthetic-10 fallback.
  AC-11.10.7: Sprint 8 broken adapter (uniform/no-signal) FAILs the gate.

All tests are deterministic, offline, and require no real model weights.
The MLX inference path is gated behind a NotImplementedError so the
forced-choice + aggregation + gate logic is verified end-to-end against
pre-recorded per-option log-prob mocks.
"""
from __future__ import annotations

import importlib.util
import json
import math
import os
import pathlib
import subprocess
import sys
from contextlib import contextmanager
from typing import Iterator

import pytest


_HERE = pathlib.Path(__file__).resolve().parent
_REPO = _HERE.parent
_SCRIPT = _REPO / "scripts" / "twin_eval.py"
_FIXTURES = _HERE / "fixtures"

_SYNTHETIC_10Q = _FIXTURES / "twin2k_synthetic_10q.jsonl"
_SYNTHETIC_5Q = _FIXTURES / "twin2k_synthetic_5q.jsonl"
_GOOD_LOG = _FIXTURES / "twin2k_good_adapter_log.jsonl"
_BROKEN_LOG = _FIXTURES / "sprint8_broken_twin2k_log.jsonl"
_BASE_VS_BASE_LOG = _FIXTURES / "twin2k_base_vs_base_log.jsonl"
_SYNTHETIC_5Q_LOG = _FIXTURES / "twin2k_synthetic_5q_log.jsonl"


# ── Module loader ─────────────────────────────────────────────────────────


def _load_module():
    spec = importlib.util.spec_from_file_location("twin_eval", _SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules["twin_eval"] = mod
    spec.loader.exec_module(mod)
    return mod


twin_eval = _load_module()


# ── Env-var helper ────────────────────────────────────────────────────────


@contextmanager
def _env(**overrides) -> Iterator[None]:
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


# ── AC-11.10.5: synthetic-10 fixture loads cleanly in CI ──────────────────


def test_synthetic_10q_fixture_loads():
    """AC-11.10.5 — committed synthetic-10 fixture is exactly 10 valid rows."""
    rows = twin_eval.load_fixture(_SYNTHETIC_10Q)
    assert len(rows) == 10
    for row in rows:
        assert row.prompt
        assert isinstance(row.options, dict)
        assert len(row.options) >= 2
        assert row.seth_answer in row.options
        assert isinstance(row.metadata, dict)


def test_synthetic_10q_is_pii_free():
    """D2 binding: synthetic fixture must NOT contain Seth's real labels.

    We can't prove a negative, but we can assert the metadata flag is set
    and that the fixture is under tests/fixtures/ (not ~/.human/private/).
    """
    rows = twin_eval.load_fixture(_SYNTHETIC_10Q)
    for row in rows:
        assert row.metadata.get("synthetic") is True, (
            f"row {row.row_id} missing synthetic=true metadata; "
            "fixture must be PII-free per decisions.md D2"
        )
    assert "tests/fixtures" in str(_SYNTHETIC_10Q)


# ── AC-11.10.3: schema validation ─────────────────────────────────────────


def test_load_fixture_rejects_missing_prompt(tmp_path):
    bad = tmp_path / "bad.jsonl"
    bad.write_text(
        '{"options": {"A": "x", "B": "y"}, "seth_answer": "A"}\n',
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="missing required key 'prompt'"):
        twin_eval.load_fixture(bad)


def test_load_fixture_rejects_missing_options(tmp_path):
    bad = tmp_path / "bad.jsonl"
    bad.write_text('{"prompt": "x", "seth_answer": "A"}\n', encoding="utf-8")
    with pytest.raises(ValueError, match="missing required key 'options'"):
        twin_eval.load_fixture(bad)


def test_load_fixture_rejects_missing_seth_answer(tmp_path):
    bad = tmp_path / "bad.jsonl"
    bad.write_text(
        '{"prompt": "x", "options": {"A": "y", "B": "z"}}\n', encoding="utf-8"
    )
    with pytest.raises(ValueError, match="missing required key 'seth_answer'"):
        twin_eval.load_fixture(bad)


def test_load_fixture_rejects_seth_answer_not_in_options(tmp_path):
    """AC-11.10.3 — seth_answer must reference a valid option letter."""
    bad = tmp_path / "bad.jsonl"
    bad.write_text(
        '{"prompt": "x", "options": {"A": "y", "B": "z"}, "seth_answer": "C"}\n',
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="not one of the option letters"):
        twin_eval.load_fixture(bad)


def test_load_fixture_rejects_options_under_2(tmp_path):
    bad = tmp_path / "bad.jsonl"
    bad.write_text(
        '{"prompt": "x", "options": {"A": "y"}, "seth_answer": "A"}\n',
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="must have 2-6 entries"):
        twin_eval.load_fixture(bad)


def test_load_fixture_rejects_options_with_invalid_letter(tmp_path):
    bad = tmp_path / "bad.jsonl"
    bad.write_text(
        '{"prompt": "x", "options": {"A": "y", "Z": "z"}, "seth_answer": "A"}\n',
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="option key 'Z' not in"):
        twin_eval.load_fixture(bad)


def test_load_fixture_rejects_empty_option_text(tmp_path):
    bad = tmp_path / "bad.jsonl"
    bad.write_text(
        '{"prompt": "x", "options": {"A": "y", "B": ""}, "seth_answer": "A"}\n',
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="option 'B' must be non-empty"):
        twin_eval.load_fixture(bad)


def test_load_fixture_rejects_invalid_json(tmp_path):
    bad = tmp_path / "bad.jsonl"
    bad.write_text("{not-json\n", encoding="utf-8")
    with pytest.raises(ValueError, match="invalid JSON"):
        twin_eval.load_fixture(bad)


def test_load_fixture_rejects_empty_file(tmp_path):
    bad = tmp_path / "bad.jsonl"
    bad.write_text("", encoding="utf-8")
    with pytest.raises(ValueError, match="fixture is empty"):
        twin_eval.load_fixture(bad)


def test_load_fixture_missing_file_raises(tmp_path):
    with pytest.raises(FileNotFoundError):
        twin_eval.load_fixture(tmp_path / "nope.jsonl")


def test_load_fixture_reports_line_number(tmp_path):
    """Schema errors must include the offending line number for debug."""
    bad = tmp_path / "bad.jsonl"
    bad.write_text(
        '{"prompt": "ok", "options": {"A": "y", "B": "z"}, "seth_answer": "A"}\n'
        '{"prompt": "bad", "options": {}, "seth_answer": "A"}\n',
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match=":2:"):
        twin_eval.load_fixture(bad)


# ── AC-11.10.6: fixture resolution / env-var routing ─────────────────────


def test_resolve_fixture_explicit_wins(tmp_path):
    """--fixture flag overrides everything else."""
    p = tmp_path / "explicit.jsonl"
    p.write_text(
        '{"prompt": "p", "options": {"A": "a", "B": "b"}, "seth_answer": "A"}\n',
        encoding="utf-8",
    )
    with _env(HU_TWIN2K_HOLDOUT="/should/not/be/used"):
        resolved = twin_eval.resolve_fixture(str(p))
    assert resolved == p


def test_resolve_fixture_env_var_routes():
    """HU_TWIN2K_HOLDOUT env var picks the production fixture."""
    with _env(HU_TWIN2K_HOLDOUT="/some/private/twin2k_seth_50q.jsonl"):
        resolved = twin_eval.resolve_fixture(None)
    assert str(resolved) == "/some/private/twin2k_seth_50q.jsonl"


def test_resolve_fixture_falls_back_to_synthetic_10():
    """No flag + no env var → synthetic-10 in tests/fixtures/."""
    with _env(HU_TWIN2K_HOLDOUT=None):
        resolved = twin_eval.resolve_fixture(None)
    assert resolved == _SYNTHETIC_10Q
    assert resolved.exists()


# ── Forced-choice scorer (pure-function unit tests) ──────────────────────


def test_score_forced_choice_picks_highest_mean_ll():
    """argmax over per-token mean log-prob (length-normalised)."""
    scores = {
        "A": (-20.0, 10),  # mean -2.0
        "B": (-15.0, 10),  # mean -1.5 ← winner
        "C": (-25.0, 10),  # mean -2.5
        "D": (-30.0, 10),  # mean -3.0
    }
    assert twin_eval.score_forced_choice(scores) == "B"


def test_score_forced_choice_length_normalises():
    """Without normalisation, longer-option raw sum would lose."""
    # Option A: 50 tokens, sum -50  → mean -1.0  ← winner (highest)
    # Option B: 5 tokens,  sum -10  → mean -2.0
    scores = {"A": (-50.0, 50), "B": (-10.0, 5)}
    assert twin_eval.score_forced_choice(scores) == "A"


def test_score_forced_choice_tie_break_alphabetical():
    """Deterministic tie-break: earlier letter wins on equal scores."""
    scores = {"A": (-15.0, 10), "B": (-15.0, 10)}
    assert twin_eval.score_forced_choice(scores) == "A"


def test_score_forced_choice_handles_all_positions():
    """Argmax behaviour for each of A/B/C/D winning."""
    for winner in ("A", "B", "C", "D"):
        scores = {
            letter: (-30.0 if letter != winner else -10.0, 10)
            for letter in "ABCD"
        }
        assert twin_eval.score_forced_choice(scores) == winner


def test_score_forced_choice_rejects_empty():
    with pytest.raises(ValueError):
        twin_eval.score_forced_choice({})


# ── Aggregation + gate unit tests ─────────────────────────────────────────


def test_binomial_stderr_known_values():
    """sqrt(p*(1-p)/n) — closed-form."""
    assert twin_eval.binomial_stderr(0.5, 10) == pytest.approx(
        math.sqrt(0.25 / 10), abs=1e-9
    )
    assert twin_eval.binomial_stderr(0.5, 50) == pytest.approx(
        math.sqrt(0.25 / 50), abs=1e-9
    )
    assert twin_eval.binomial_stderr(1.0, 10) == 0.0
    assert twin_eval.binomial_stderr(0.0, 10) == 0.0


def test_binomial_stderr_n_zero_returns_zero():
    assert twin_eval.binomial_stderr(0.5, 0) == 0.0


def test_decide_gate_pass():
    """Adapter at 0.70 with +0.20 delta → PASS."""
    decision, notes = twin_eval.decide_gate(
        adapter_accuracy=0.70, delta_accuracy=0.20
    )
    assert decision == "PASS"
    assert any("0.700" in n for n in notes)


def test_decide_gate_fail_on_low_accuracy():
    """Accuracy below 0.65 floor → FAIL even if delta is positive."""
    decision, notes = twin_eval.decide_gate(
        adapter_accuracy=0.50, delta_accuracy=0.10
    )
    assert decision == "FAIL"
    assert any("0.500" in n for n in notes)


def test_decide_gate_fail_on_zero_delta():
    """Zero delta is not improvement, even with high accuracy → FAIL."""
    decision, _ = twin_eval.decide_gate(
        adapter_accuracy=0.80, delta_accuracy=0.0
    )
    assert decision == "FAIL"


def test_decide_gate_fail_on_negative_delta():
    decision, _ = twin_eval.decide_gate(
        adapter_accuracy=0.80, delta_accuracy=-0.05
    )
    assert decision == "FAIL"


def test_decide_gate_low_accuracy_overrides_positive_delta():
    """AC-11.10.7 broken-adapter regression: low accuracy must win the veto."""
    decision, _ = twin_eval.decide_gate(
        adapter_accuracy=0.55, delta_accuracy=0.30
    )
    assert decision == "FAIL"


# ── compute_accuracy unit tests ──────────────────────────────────────────


def test_compute_accuracy_basic():
    """4/5 correct → 0.80."""
    rows = twin_eval.load_fixture(_SYNTHETIC_5Q)
    # Build per-row scores: pick the correct answer for 4 rows; wrong for last.
    per_row = []
    for i, row in enumerate(rows):
        correct = row.seth_answer
        if i < 4:
            per_row.append(
                {letter: (-10.0 if letter == correct else -30.0, 10)
                 for letter in row.options}
            )
        else:
            wrong = next(l for l in row.options if l != correct)
            per_row.append(
                {letter: (-10.0 if letter == wrong else -30.0, 10)
                 for letter in row.options}
            )
    n_correct, acc = twin_eval.compute_accuracy(rows, per_row)
    assert n_correct == 4
    assert acc == pytest.approx(0.80)


def test_compute_accuracy_row_count_mismatch_raises():
    rows = twin_eval.load_fixture(_SYNTHETIC_5Q)
    with pytest.raises(ValueError, match="score-row count mismatch"):
        twin_eval.compute_accuracy(rows, [])


# ── AC-11.10.1: full output schema ────────────────────────────────────────


def test_output_schema_keys_and_types():
    """AC-11.10.1 — result has all five required keys with correct types."""
    result = twin_eval.evaluate(
        fixture_path=_SYNTHETIC_10Q,
        mock_log_path=_GOOD_LOG,
        adapter_path="/fake/adapter",
        base_model="gemma-4-e2b",
    )
    from dataclasses import asdict

    obj = asdict(result)
    # AC-required keys present
    for key in (
        "n_questions",
        "adapter_accuracy",
        "base_accuracy",
        "delta_accuracy",
        "stderr",
    ):
        assert key in obj, f"missing AC-11.10.1 key '{key}'"
    assert isinstance(obj["n_questions"], int)
    assert isinstance(obj["adapter_accuracy"], float)
    assert isinstance(obj["base_accuracy"], float)
    assert isinstance(obj["delta_accuracy"], float)
    assert isinstance(obj["stderr"], float)
    # Provenance + gate keys
    assert obj["fixture_path"]
    assert obj["gate_decision"] in ("PASS", "FAIL")
    assert isinstance(obj["notes"], list)


# ── AC-11.10.2: accuracy computation (4/5 vs 2/5 → +0.40) ────────────────


def test_accuracy_computation_4_of_5_vs_2_of_5():
    """AC-11.10.2 — adapter 4/5 (0.80) vs base 2/5 (0.40); delta = +0.40."""
    result = twin_eval.evaluate(
        fixture_path=_SYNTHETIC_5Q,
        mock_log_path=_SYNTHETIC_5Q_LOG,
        adapter_path="/fake/adapter",
        base_model="gemma-4-e2b",
    )
    assert result.n_questions == 5
    assert result.n_correct_adapter == 4
    assert result.n_correct_base == 2
    assert result.adapter_accuracy == pytest.approx(0.80)
    assert result.base_accuracy == pytest.approx(0.40)
    assert result.delta_accuracy == pytest.approx(0.40)
    # Stderr at p=0.8, n=5: sqrt(0.16/5) ≈ 0.1789
    assert result.stderr == pytest.approx(math.sqrt(0.16 / 5), abs=1e-4)
    # Gate: adapter_accuracy=0.80 >= 0.65 AND delta=+0.40 > 0 → PASS
    assert result.gate_decision == "PASS"


def test_good_adapter_passes_gate_on_synthetic_10():
    """End-to-end on synthetic-10 with a strong adapter mock → PASS."""
    result = twin_eval.evaluate(
        fixture_path=_SYNTHETIC_10Q,
        mock_log_path=_GOOD_LOG,
        adapter_path="/fake/adapter",
        base_model="gemma-4-e2b",
    )
    assert result.n_questions == 10
    assert result.adapter_accuracy >= twin_eval.ACCURACY_PASS_THRESHOLD
    assert result.delta_accuracy > 0
    assert result.gate_decision == "PASS"


# ── AC-11.10.7: Sprint 8 broken-adapter regression guard ─────────────────


def test_broken_adapter_fails_gate_ac_11_10_7():
    """AC-11.10.7 — Sprint 8 broken adapter MUST fail Twin-2K-500 gate.

    The fixture simulates pad-token spam: near-uniform option scores
    produce essentially-random argmax (~0.5 accuracy). Whether the
    accuracy floor or the delta-veto trips first depends on the random
    seed embedded in the mock; both must fire on at least one path.
    """
    result = twin_eval.evaluate(
        fixture_path=_SYNTHETIC_10Q,
        mock_log_path=_BROKEN_LOG,
        adapter_path="/fake/broken/adapter",
        base_model="gemma-4-e2b",
    )
    assert result.n_questions == 10
    # Broken adapter cannot demonstrate behavioral signal: accuracy near random.
    assert result.adapter_accuracy < twin_eval.ACCURACY_PASS_THRESHOLD
    # And the gate must fire FAIL.
    assert result.gate_decision == "FAIL"
    # Notes must explain why (accuracy floor failure).
    assert any("broken-adapter" in n or "below" in n.lower() for n in result.notes)


def test_base_vs_base_honest_baseline_zero_delta():
    """Sanity: base scored against base → delta=0 → gate FAIL.

    Identical model scores must produce identical predictions; this is the
    honest null result. The gate fails because there's no improvement.
    """
    result = twin_eval.evaluate(
        fixture_path=_SYNTHETIC_10Q,
        mock_log_path=_BASE_VS_BASE_LOG,
        adapter_path="/no/adapter",
        base_model="gemma-4-e2b",
    )
    assert result.delta_accuracy == pytest.approx(0.0, abs=1e-9)
    assert result.n_correct_adapter == result.n_correct_base
    # Either accuracy is below threshold OR delta is zero — both → FAIL.
    assert result.gate_decision == "FAIL"


# ── AC-11.10.4: HU_IS_TEST guard / no real inference ─────────────────────


def test_no_real_inference_without_mock_log(tmp_path):
    """AC-11.10.4 — without --mock-from-jsonl the real path is gated.

    The real `_real_compute_logprob` either imports mlx_lm (raises
    RuntimeError in CI) or raises NotImplementedError. Either way, no
    real weights are loaded; only the mock seam can produce a result.
    """
    fixture = tmp_path / "tiny.jsonl"
    fixture.write_text(
        '{"prompt": "p", "options": {"A": "a", "B": "b"}, "seth_answer": "A"}\n',
        encoding="utf-8",
    )
    with pytest.raises((RuntimeError, NotImplementedError)):
        twin_eval.evaluate(
            fixture_path=fixture,
            mock_log_path=None,
            adapter_path="/fake",
            base_model="gemma-4-e2b",
        )


def test_mock_seam_works_without_mlx_lm():
    """AC-11.10.4 corollary — mock path runs end-to-end without MLX.

    If this passes in CI (which doesn't have mlx_lm installed), we know
    no inference call ever happened.
    """
    result = twin_eval.evaluate(
        fixture_path=_SYNTHETIC_10Q,
        mock_log_path=_GOOD_LOG,
        adapter_path="/fake/adapter",
        base_model="gemma-4-e2b",
    )
    assert result.n_questions == 10
    # If we got here without ImportError, the mock seam fully replaced inference.


def test_mock_log_row_count_mismatch_raises(tmp_path):
    """Mock log shorter than fixture → ValueError, no silent truncation."""
    short = tmp_path / "short.jsonl"
    short.write_text(
        '{"row_id": 1, "base_option_scores": {"A": [-20.0, 10], "B": [-21.0, 10]}, '
        '"adapter_option_scores": {"A": [-15.0, 10], "B": [-20.0, 10]}}\n',
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="Counts must match"):
        twin_eval.evaluate(
            fixture_path=_SYNTHETIC_10Q,
            mock_log_path=short,
            adapter_path="/fake",
            base_model="gemma-4-e2b",
        )


def test_mock_log_missing_option_raises(tmp_path):
    """If the mock log is missing an option the fixture references → error."""
    fixture = tmp_path / "tiny.jsonl"
    fixture.write_text(
        '{"prompt": "p", "options": {"A": "a", "B": "b", "C": "c"}, '
        '"seth_answer": "A"}\n',
        encoding="utf-8",
    )
    short = tmp_path / "short.jsonl"
    short.write_text(
        '{"row_id": 1, "base_option_scores": {"A": [-20.0, 10], "B": [-21.0, 10]}, '
        '"adapter_option_scores": {"A": [-15.0, 10], "B": [-20.0, 10]}}\n',
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="missing option 'C'"):
        twin_eval.evaluate(
            fixture_path=fixture,
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
        str(_SYNTHETIC_10Q),
        env={"HU_TWIN2K_HOLDOUT": None},
    )
    assert proc.returncode == 0, f"stderr: {proc.stderr}"
    parsed = json.loads(proc.stdout)
    assert parsed["gate_decision"] == "PASS"
    assert parsed["delta_accuracy"] > 0
    assert parsed["n_questions"] == 10


def test_cli_broken_adapter_exits_nonzero():
    """CLI returns 1 when gate is FAIL (Sprint 8 regression guard)."""
    proc = _run_cli(
        "--mock-from-jsonl",
        str(_BROKEN_LOG),
        "--fixture",
        str(_SYNTHETIC_10Q),
        env={"HU_TWIN2K_HOLDOUT": None},
    )
    assert proc.returncode == 1, f"stderr: {proc.stderr}"
    parsed = json.loads(proc.stdout)
    assert parsed["gate_decision"] == "FAIL"


def test_cli_requires_adapter_or_mock():
    """CLI errors if neither --adapter nor --mock-from-jsonl is given."""
    proc = _run_cli("--fixture", str(_SYNTHETIC_10Q))
    assert proc.returncode != 0
    assert "must specify either" in proc.stderr


def test_cli_invalid_fixture_returns_2(tmp_path):
    """Bad-fixture (schema fail) → exit 2."""
    bad = tmp_path / "bad.jsonl"
    bad.write_text('{"prompt": "x"}\n', encoding="utf-8")
    proc = _run_cli(
        "--mock-from-jsonl",
        str(_GOOD_LOG),
        "--fixture",
        str(bad),
    )
    assert proc.returncode == 2


def test_cli_protocol_flag_accepts_forced_choice():
    """--protocol forced-choice is the documented mode."""
    proc = _run_cli(
        "--protocol",
        "forced-choice",
        "--mock-from-jsonl",
        str(_GOOD_LOG),
        "--fixture",
        str(_SYNTHETIC_10Q),
        env={"HU_TWIN2K_HOLDOUT": None},
    )
    assert proc.returncode == 0, f"stderr: {proc.stderr}"


def test_cli_protocol_flag_rejects_unknown():
    """Unknown --protocol values are rejected by argparse."""
    proc = _run_cli(
        "--protocol",
        "ynyp-bogus",
        "--mock-from-jsonl",
        str(_GOOD_LOG),
        "--fixture",
        str(_SYNTHETIC_10Q),
    )
    assert proc.returncode != 0


def test_cli_output_file_round_trip(tmp_path):
    """--output writes the same JSON to disk that stdout would emit."""
    out = tmp_path / "result.json"
    proc = _run_cli(
        "--mock-from-jsonl",
        str(_GOOD_LOG),
        "--fixture",
        str(_SYNTHETIC_10Q),
        "--output",
        str(out),
        env={"HU_TWIN2K_HOLDOUT": None},
    )
    assert proc.returncode == 0, f"stderr: {proc.stderr}"
    parsed = json.loads(out.read_text(encoding="utf-8"))
    assert parsed["n_questions"] == 10
    assert parsed["gate_decision"] == "PASS"


# ── Determinism (re-run produces byte-identical output) ──────────────────


def test_evaluator_is_deterministic():
    """Same fixture + same mock → byte-identical result."""
    from dataclasses import asdict

    r1 = twin_eval.evaluate(
        fixture_path=_SYNTHETIC_10Q,
        mock_log_path=_GOOD_LOG,
        adapter_path="/fake/adapter",
        base_model="gemma-4-e2b",
    )
    r2 = twin_eval.evaluate(
        fixture_path=_SYNTHETIC_10Q,
        mock_log_path=_GOOD_LOG,
        adapter_path="/fake/adapter",
        base_model="gemma-4-e2b",
    )
    assert json.dumps(asdict(r1), sort_keys=True) == json.dumps(
        asdict(r2), sort_keys=True
    )
