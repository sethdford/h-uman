"""Sprint 11 / US-11.7 — 4-stage Pareto gate cascade tests.

Covers the AC for the cascade orchestrator + each stage independently.
All tests are deterministic, offline, and never invoke a real Gemma model.
Test seams used:

  - Stage 1: cascade fixture JSON (adapter_ppl/base_ppl) or
    HU_CASCADE_STAGE1_MOCK_PPL env var.
  - Stage 2: HU_CASCADE_STAGE2_MOCK env var (JSON) or cascade fixture
    coherence_scores/coherence_pad_outputs arrays.
  - Stage 3: --stage3-stub CLI flag (or stage3_stub kwarg in run_cascade).

Test naming follows `subject_expected_behavior` per testing standards.
"""
from __future__ import annotations

import importlib.util
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
_FIXTURES = _HERE / "fixtures" / "cascade"

_F_SPRINT8 = _FIXTURES / "sprint8_iter200.json"
_F_DIRTY = _FIXTURES / "iter60_dirty.json"
_F_PADFIX = _FIXTURES / "iter60_padfix.json"

_SCRIPT_CASCADE = _REPO / "scripts" / "stage_cascade.py"
_SCRIPT_PARETO = _REPO / "scripts" / "pareto_picker.py"


# ── Module loader ─────────────────────────────────────────────────────────


def _load(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


stage_cascade = _load("stage_cascade", _SCRIPT_CASCADE)
pareto_picker = _load("pareto_picker", _SCRIPT_PARETO)
stage1_ppl = _load(
    "cascade_stages.stage1_ppl", _REPO / "scripts" / "cascade_stages" / "stage1_ppl.py"
)
stage2_coherence = _load(
    "cascade_stages.stage2_coherence",
    _REPO / "scripts" / "cascade_stages" / "stage2_coherence.py",
)
stage3_prm_stub = _load(
    "cascade_stages.stage3_prm_stub",
    _REPO / "scripts" / "cascade_stages" / "stage3_prm_stub.py",
)


# ── Env-var helper ────────────────────────────────────────────────────────


@contextmanager
def _env(**overrides) -> Iterator[None]:
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
                os.environ[k] = v


# ── Stage 1 — PPL floor (AC-11.7.1) ───────────────────────────────────────


def test_ppl_floor_rejects_high_ppl():
    """AC-11.7.1: adapter_ppl = 4 * base_ppl => REJECT at Stage 1."""
    result = stage1_ppl.run(fixture_path=str(_F_SPRINT8))
    assert result["stage"] == 1
    assert result["status"] == "REJECT"
    assert result["details"]["ratio"] == pytest.approx(4.0)
    assert result["details"]["floor_ratio"] == pytest.approx(3.0)
    assert "AC-11.7.1" in result["reason"]


def test_ppl_floor_passes_clean_adapter():
    """1.05× base PPL is well under 3× floor → PASS."""
    result = stage1_ppl.run(fixture_path=str(_F_PADFIX))
    assert result["status"] == "PASS"
    assert result["score"] is not None
    assert result["score"] > 0.0


def test_ppl_floor_env_mock_takes_precedence():
    """HU_CASCADE_STAGE1_MOCK_PPL overrides cascade fixture content."""
    with _env(HU_CASCADE_STAGE1_MOCK_PPL="100.0,10.0"):
        # ratio = 10x → REJECT regardless of which fixture path is used.
        result = stage1_ppl.run(fixture_path=str(_F_PADFIX))
    assert result["status"] == "REJECT"
    assert result["details"]["ratio"] == pytest.approx(10.0)
    assert result["details"]["source"] == "env:HU_CASCADE_STAGE1_MOCK_PPL"


def test_ppl_floor_abstains_without_source():
    """No env mock, no fixture → ABSTAIN (real MLX deferred)."""
    with _env(HU_CASCADE_STAGE1_MOCK_PPL=None):
        result = stage1_ppl.run(fixture_path=None)
    assert result["status"] == "ABSTAIN"


# ── Stage 2 — coherence (AC-11.7.2) ───────────────────────────────────────


def test_coherence_judge_rejects_pad_outputs():
    """AC-11.7.2: pad_rate >= 50% on coherence prompts → REJECT at Stage 2."""
    # 5/5 prompts emit pads; scores nominally high but pad gate fires first.
    mock = json.dumps({
        "scores": [0.85, 0.86, 0.84, 0.87, 0.85],
        "pad_outputs": [True, True, True, True, True],
    })
    with _env(HU_CASCADE_STAGE2_MOCK=mock):
        result = stage2_coherence.run()
    assert result["stage"] == 2
    assert result["status"] == "REJECT"
    assert "AC-11.7.2" in result["reason"]
    assert result["details"]["pad_rate"] == pytest.approx(1.0)


def test_coherence_judge_rejects_mismatched_pad_outputs_length():
    """PR #115 Bugbot HIGH regression guard.

    Previously `pad_rate` was computed as `count(True in pad_outputs) / n`
    where `n = len(scores)`. If `pad_outputs` had fewer entries than
    `scores`, the denominator was inflated and `pad_rate` was silently
    diluted below the 50% REJECT threshold — letting an adapter with
    heavy pad-token leakage slip past the exact Sprint 8 regression
    guard this stage is supposed to enforce.

    Contract: mismatched arrays must ABSTAIN, not silently compute a
    wrong rate. (Empty pad_outputs is still legitimate → pad_rate=0.0,
    no signal available — covered by other tests.)
    """
    # 5 scores, only 2 pad_outputs (both True). Under the old bug:
    # pad_rate = 2/5 = 0.40 < 0.50 → would NOT trigger REJECT despite
    # 100% of available pad signal indicating leakage. New behavior:
    # ABSTAIN because the arrays disagree on cardinality.
    mock = json.dumps({
        "scores": [0.85, 0.86, 0.84, 0.87, 0.85],
        "pad_outputs": [True, True],
    })
    with _env(HU_CASCADE_STAGE2_MOCK=mock):
        result = stage2_coherence.run()
    assert result["status"] == "ABSTAIN", (
        f"Mismatched pad_outputs length should ABSTAIN, got {result['status']!r}. "
        f"Reason: {result.get('reason')!r}"
    )
    assert "mismatched" in result["reason"].lower() or "length" in result["reason"].lower()
    assert result["details"]["n_scores"] == 5
    assert result["details"]["n_pad_outputs"] == 2


def test_coherence_judge_rejects_low_mean_score():
    """Mean coherence < 0.70 → REJECT (design §6 OQ-4)."""
    mock = json.dumps({"scores": [0.3, 0.4, 0.5, 0.4, 0.3], "pad_outputs": [False] * 5})
    with _env(HU_CASCADE_STAGE2_MOCK=mock):
        result = stage2_coherence.run()
    assert result["status"] == "REJECT"
    assert "coherence floor" in result["reason"].lower() or "coherence" in result["reason"]


def test_coherence_judge_passes_clean_adapter():
    """High mean score + low pad rate → PASS with mean score."""
    mock = json.dumps({"scores": [0.82, 0.85, 0.80, 0.83, 0.81], "pad_outputs": [False] * 5})
    with _env(HU_CASCADE_STAGE2_MOCK=mock):
        result = stage2_coherence.run()
    assert result["status"] == "PASS"
    assert result["score"] == pytest.approx(0.822)


def test_stage2_abstain_on_judge_failure():
    """No env mock + no fixture → ABSTAIN (judge unavailable)."""
    with _env(HU_CASCADE_STAGE2_MOCK=None):
        result = stage2_coherence.run(fixture_path=None)
    assert result["status"] == "ABSTAIN"
    assert result["score"] is None


# ── Stage 3 — PRM stub (AC-11.7.4) ────────────────────────────────────────


def test_stage3_stub_configurable():
    """AC-11.7.4: --stage3-stub <f> injects the score; default is SKIP."""
    # Default: SKIP, null score, never promotes.
    default_result = stage3_prm_stub.run()
    assert default_result["status"] == "SKIP"
    assert default_result["score"] is None
    assert "Sprint 12" in default_result["reason"]

    # Stubbed: PASS with injected score for Stage 4 testing.
    stubbed = stage3_prm_stub.run(stage3_stub=0.5)
    assert stubbed["status"] == "PASS"
    assert stubbed["score"] == pytest.approx(0.5)


def test_stage3_skip_does_not_promote_alone():
    """Dormancy invariant (Risk 3): null Stage 3 cannot promote without
    other stages. Run a synthetic ensemble where every other contributing
    judge is at the DEFER boundary and Stage 3 is null — final verdict must
    not be PROMOTE."""
    agg = pareto_picker.ensemble_min_aggregate({
        "lexical": 0.01,    # exactly at DEFER floor, not PROMOTE
        "coherence": 0.70,  # exactly at DEFER floor, not PROMOTE
        "prm": None,        # dormant
    })
    assert agg["verdict"] == "DEFER"


def test_stage3_null_excluded_from_min_aggregation():
    """Stage 3 null must not pull a verdict up OR down. Verify by
    comparing the verdict with and without prm=null."""
    with_null = pareto_picker.ensemble_min_aggregate({
        "lexical": 0.05,
        "coherence": 0.85,
        "prm": None,
    })
    without_prm = pareto_picker.ensemble_min_aggregate({
        "lexical": 0.05,
        "coherence": 0.85,
    })
    assert with_null["verdict"] == without_prm["verdict"] == "PROMOTE"


# ── Stage 4 — ensemble min-aggregation (AC-11.7.5) ────────────────────────


def test_ensemble_min_aggregation():
    """AC-11.7.5: three judge scores {lexical: PROMOTE, coherence: PROMOTE,
    nll: DEFER} → final = DEFER (min, not majority)."""
    agg = pareto_picker.ensemble_min_aggregate({
        "lexical": 0.05,    # PROMOTE (>= 0.03)
        "coherence": 0.85,  # PROMOTE (>= 0.80)
        "nll": 0.005,       # DEFER (>= 0, < 0.02)
    })
    assert agg["verdict"] == "DEFER"
    assert agg["min_judge"] == "nll"
    assert agg["n_contributing"] == 3


def test_ensemble_rejects_when_any_judge_rejects():
    """One judge below DEFER floor sinks the whole verdict."""
    agg = pareto_picker.ensemble_min_aggregate({
        "lexical": 0.05,    # PROMOTE
        "coherence": 0.85,  # PROMOTE
        "nll": -0.01,       # REJECT (signed delta_ll, negative = worse)
    })
    assert agg["verdict"] == "REJECT"
    assert agg["min_judge"] == "nll"


def test_ensemble_all_skip_returns_defer():
    """If every judge is null, the cron must NOT promote (Sprint 7 D3)."""
    agg = pareto_picker.ensemble_min_aggregate({
        "lexical": None,
        "coherence": None,
        "prm": None,
    })
    assert agg["verdict"] == "DEFER"
    assert agg["n_contributing"] == 0
    assert agg["n_null"] == 3


# ── AC-11.7.3 — Sprint 8 regression guard (CRITICAL) ──────────────────────


def test_sprint8_iter200_rejected_by_gate():
    """AC-11.7.3: the Sprint 8 iter-200 fixture (pad=80%, delta=+0.046
    lexical, PPL=4×base) MUST be REJECTED at Stage 1 (not just somewhere
    in the cascade — a re-ordering bug must fail this AC)."""
    result = stage_cascade.run_cascade(
        adapter_path=None, fixture_path=str(_F_SPRINT8)
    )
    assert result["final_verdict"] == "REJECT"
    assert result["exit_code"] == 2

    # Specifically at Stage 1.
    s1 = result["stages"][0]
    assert s1["stage"] == 1
    assert s1["name"] == "ppl_floor"
    assert s1["status"] == "REJECT", (
        f"Sprint 8 regression guard requires Stage 1 REJECT specifically; "
        f"got {s1['status']}. Cascade order may be wrong."
    )

    # Subsequent stages MUST NOT have been invoked (short-circuit semantics).
    for downstream in result["stages"][1:]:
        assert downstream["status"] == "skipped_due_to_short_circuit", (
            f"Stage {downstream['stage']} was invoked after Stage 1 REJECT; "
            "fail-fast contract violated."
        )


def test_stage1_short_circuits_stage2_not_invoked():
    """AC-11.7.1 strict: when Stage 1 REJECTs, Stage 2 must NOT have been
    invoked. Assert via the per-stage 'skipped' marker — this is the
    re-ordering tripwire."""
    result = stage_cascade.run_cascade(
        adapter_path=None, fixture_path=str(_F_SPRINT8)
    )
    s2 = result["stages"][1]
    assert s2["stage"] == 2
    assert s2["status"] == "skipped_due_to_short_circuit"


# ── End-to-end scenarios from design §3 ───────────────────────────────────


def test_iter60_dirty_scenario_defers():
    """Design §3 scenario: pad_rate=0.40, PPL=1.2× → PASS Stages 1+2,
    DEFER at Stage 4 (pad_rate above PROMOTION_PAD_CEILING=0.10 means
    Pareto / lexical signal sits in DEFER band)."""
    result = stage_cascade.run_cascade(
        adapter_path=None, fixture_path=str(_F_DIRTY)
    )
    assert result["final_verdict"] == "DEFER"
    assert result["exit_code"] == 1
    assert result["stages"][0]["status"] == "PASS"
    assert result["stages"][1]["status"] == "PASS"
    assert result["stages"][2]["status"] == "SKIP"  # dormant


def test_iter60_padfix_scenario_promotes():
    """Design §3 scenario: post-pad-masking-fix, all stages PASS → PROMOTE."""
    result = stage_cascade.run_cascade(
        adapter_path=None, fixture_path=str(_F_PADFIX)
    )
    assert result["final_verdict"] == "PROMOTE"
    assert result["exit_code"] == 0


def test_stage2_abstain_caps_at_defer():
    """Risk 2 mitigation: Stage 2 ABSTAIN must cap final verdict at DEFER
    even when other judges would promote.

    Sprint 11 / US-11.7 critic-HIGH #1 fix: previously asserted
    `verdict in ("DEFER", "REJECT")` which would silently accept a regression
    that mapped Stage 2 ABSTAIN to REJECT. The exact value matters — DEFER is
    the contract, REJECT would punish the operator for a judge crash they
    didn't cause."""
    # Use a fixture where Stage 1 PASSES (adapter_ppl ≈ base_ppl, well under
    # the 3× floor) but Stage 2 ABSTAINS (no coherence_scores). Synthesize
    # a tiny ad-hoc fixture.
    import tempfile

    ad_hoc = {
        "adapter_ppl": 12.6,
        "base_ppl": 12.0,
        # no coherence_scores -> Stage 2 ABSTAIN
    }
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as fp:
        json.dump(ad_hoc, fp)
        path = fp.name
    try:
        with _env(HU_CASCADE_STAGE2_MOCK=None):
            result = stage_cascade.run_cascade(
                adapter_path=None, fixture_path=path
            )
        # Stage 1 PASS, Stage 2 ABSTAIN, Stage 3 SKIP, Stage 4 capped at DEFER.
        assert result["stages"][0]["status"] == "PASS"
        assert result["stages"][1]["status"] == "ABSTAIN"
        # TIGHT assertion — exactly DEFER, not REJECT, not PROMOTE.
        assert result["final_verdict"] == "DEFER", (
            f"Stage 2 ABSTAIN must cap at DEFER (got {result['final_verdict']!r}); "
            f"REJECT would mis-punish operator for a judge crash, "
            f"PROMOTE would bypass the gate entirely."
        )
        assert result["exit_code"] == 1
    finally:
        os.unlink(path)


def test_stage1_abstain_rejects_no_ppl_evidence():
    """Sprint 11 / US-11.7 critic-CRITICAL #1 regression guard.

    The gate MUST refuse to promote when Stage 1 has no PPL evidence. Before
    the critic-CRITICAL fix, a malformed fixture with no `adapter_ppl`/
    `base_ppl` fields but valid coherence_scores ≥ 0.80 produced a final
    verdict of PROMOTE — Stage 1 ABSTAINed, fell through to Stage 2, and
    Stage 4's min-aggregation didn't know to refuse a stage that never ran.

    PPL is the cheapest and most deterministic guard against the Sprint 8
    pad-token collapse mode. If it cannot run, we MUST treat that as a hard
    REJECT, not an opportunity for the other judges to PROMOTE on
    incomplete evidence.
    """
    import tempfile

    # Stage 1 ABSTAIN (no PPL fields) + Stage 2 would otherwise PROMOTE.
    ad_hoc = {
        "coherence_scores": [0.85, 0.85, 0.85, 0.85, 0.85],  # well above 0.80 PROMOTE
        # no adapter_ppl / base_ppl -> Stage 1 ABSTAIN
    }
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as fp:
        json.dump(ad_hoc, fp)
        path = fp.name
    try:
        with _env(HU_CASCADE_STAGE2_MOCK=None):
            result = stage_cascade.run_cascade(
                adapter_path=None, fixture_path=path
            )

        # Stage 1 must ABSTAIN explicitly (no PPL evidence).
        assert result["stages"][0]["status"] == "ABSTAIN", (
            f"Stage 1 should ABSTAIN when no PPL evidence is present "
            f"(got {result['stages'][0]['status']!r})"
        )
        # Stages 2, 3, 4 must be marked as skipped — the fix elevates Stage 1
        # ABSTAIN to a hard short-circuit equivalent to REJECT.
        for i in (1, 2, 3):
            assert result["stages"][i]["status"] == "skipped_due_to_short_circuit", (
                f"Stage {i + 1} must be marked skipped_due_to_short_circuit when "
                f"Stage 1 ABSTAINs (got {result['stages'][i]['status']!r})"
            )
        # Final verdict must be REJECT — gate refuses to promote without PPL evidence.
        assert result["final_verdict"] == "REJECT", (
            f"Stage 1 ABSTAIN must produce REJECT, not "
            f"{result['final_verdict']!r}. Promoting past a stage that never ran "
            f"is exactly the silent-failure mode this gate is supposed to refuse."
        )
        assert result["exit_code"] == 2
    finally:
        os.unlink(path)


# ── Exit codes via CLI subprocess (end-to-end) ────────────────────────────


def test_cli_exit_code_promote():
    """CLI exit 0 for PROMOTE."""
    rc = subprocess.run(
        [sys.executable, str(_SCRIPT_CASCADE), "--fixture", str(_F_PADFIX)],
        capture_output=True,
        check=False,
    ).returncode
    assert rc == 0


def test_cli_exit_code_defer():
    """CLI exit 1 for DEFER."""
    rc = subprocess.run(
        [sys.executable, str(_SCRIPT_CASCADE), "--fixture", str(_F_DIRTY)],
        capture_output=True,
        check=False,
    ).returncode
    assert rc == 1


def test_cli_exit_code_reject():
    """CLI exit 2 for REJECT (Sprint 8 fixture)."""
    rc = subprocess.run(
        [sys.executable, str(_SCRIPT_CASCADE), "--fixture", str(_F_SPRINT8)],
        capture_output=True,
        check=False,
    ).returncode
    assert rc == 2


def test_cli_emits_per_stage_json():
    """Output is a single JSON object with one entry per stage (AC-11.7.6 shape)."""
    proc = subprocess.run(
        [sys.executable, str(_SCRIPT_CASCADE), "--fixture", str(_F_PADFIX)],
        capture_output=True,
        check=False,
        text=True,
    )
    payload = json.loads(proc.stdout)
    assert "stages" in payload
    assert len(payload["stages"]) == 4
    names = [s["name"] for s in payload["stages"]]
    assert names == ["ppl_floor", "coherence", "prm_stub", "ensemble"]
    assert "final_verdict" in payload
    assert "exit_code" in payload


def test_pareto_picker_stage_scores_cli():
    """pareto_picker.py --stage-scores produces the ensemble verdict."""
    scores = json.dumps({
        "lexical": 0.05,
        "coherence": 0.85,
        "nll": 0.005,  # DEFER (>= 0, < 0.02)
    })
    proc = subprocess.run(
        [sys.executable, str(_SCRIPT_PARETO), "--stage-scores", scores],
        capture_output=True,
        check=False,
        text=True,
    )
    assert proc.returncode == 1  # DEFER
    assert "DEFER" in proc.stdout
