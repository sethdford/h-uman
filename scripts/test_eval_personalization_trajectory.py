#!/usr/bin/env python3
"""
Tests for eval_personalization_trajectory.py — the orchestrator.

Inference is injected (a recording stub), so NO model runs. The gate's verdict
logic is exhaustively covered by test_trajectory_gate.py; these tests prove the
ORCHESTRATION: output schema, per-generation caching, cache-key composition,
measurement→gate wiring, and exit-code mapping.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import eval_personalization_trajectory as traj
from trajectory_gate import evaluate_trajectory_gate

# Tiny synthetic inputs — run_trajectory is agnostic to prompt/probe count
# (the 20-prompt floor lives in main(), not here).
PROMPTS = [{"prompt": "hey"}, {"prompt": "sup"}, {"prompt": "yo what's good"}]
PROBES = [
    {"id": "a", "prompt": "2+2?", "check": {"type": "numeric_equals", "value": 4}},
    {"id": "b", "prompt": "3+3?", "check": {"type": "numeric_equals", "value": 6}},
]
FIX_SHA = "fixturesha"
PROBE_SHA = "probesha"

GEN0 = {"gen": 0, "label": "base", "adapter_path": None, "train_pairs": 0}
GEN1 = {"gen": 1, "label": "v4", "adapter_path": "/adapters/v4", "train_pairs": 100}


class RecordingStub:
    """Injectable generate_fn that records calls and answers probes correctly so
    base_capability is deterministic. Fidelity text is casual (the shape
    classifier scores it however it scores it — we don't assert exact values)."""

    def __init__(self, probe_answers: dict | None = None):
        self.calls: list[tuple[str, str | None]] = []
        # default: answer both probes correctly → base_capability 1.0
        self.probe_answers = probe_answers or {"2+2?": "4", "3+3?": "6"}

    def __call__(self, model_id, prompt, adapter_path):
        self.calls.append((prompt, adapter_path))
        if prompt in self.probe_answers:
            return self.probe_answers[prompt]
        return "haha yeah for sure, what's up"

    def adapters_called(self):
        return {a for _, a in self.calls}


def test_trajectory_schema():
    stub = RecordingStub()
    trajectory, _ = traj.run_trajectory(
        [GEN0, GEN1], PROMPTS, PROBES, FIX_SHA, PROBE_SHA, "test-model", stub
    )
    for key in ("timestamp", "model_id", "fixture_sha", "probeset_sha",
                "generations", "gate", "verdict", "exit_code"):
        assert key in trajectory, f"missing top-level key {key}"
    assert trajectory["fixture_sha"] == FIX_SHA
    assert trajectory["probeset_sha"] == PROBE_SHA
    assert len(trajectory["generations"]) == 2
    for g in trajectory["generations"]:
        for f in ("gen", "label", "adapter_path", "train_pairs",
                  "fidelity_mean", "fidelity_ci", "base_capability"):
            assert f in g, f"gen row missing field {f}"
        assert 0.0 <= g["fidelity_mean"] <= 1.0
        assert 0.0 <= g["base_capability"] <= 1.0
        assert isinstance(g["fidelity_ci"], list) and len(g["fidelity_ci"]) == 2
    print("✓ trajectory + per-generation schema complete")


def test_base_capability_deterministic_from_probes():
    """Both probes answered correctly → base_capability 1.0; both wrong → 0.0."""
    good, _ = traj.run_trajectory([GEN0], PROMPTS, PROBES, FIX_SHA, PROBE_SHA,
                                  "m", RecordingStub())
    assert good["generations"][0]["base_capability"] == 1.0
    bad_stub = RecordingStub(probe_answers={"2+2?": "5", "3+3?": "7"})
    bad, _ = traj.run_trajectory([GEN0], PROMPTS, PROBES, FIX_SHA, PROBE_SHA,
                                 "m", bad_stub)
    assert bad["generations"][0]["base_capability"] == 0.0
    print("✓ base_capability flows deterministically from probe answers (1.0 / 0.0)")


def test_caching_skips_measured_generation():
    """A pre-seeded cache entry for gen0 means the stub is NEVER called for the
    base adapter (None); gen1 (uncached) IS measured."""
    seeded = {
        traj.cache_key(None, FIX_SHA, PROBE_SHA): {
            "gen": 0, "label": "base", "adapter_path": None, "train_pairs": 0,
            "ts": None, "fidelity_mean": 0.50, "fidelity_ci": [0.45, 0.55],
            "base_capability": 0.94,
        }
    }
    stub = RecordingStub()
    trajectory, new_cache = traj.run_trajectory(
        [GEN0, GEN1], PROMPTS, PROBES, FIX_SHA, PROBE_SHA, "m", stub, cache=seeded
    )
    rows = {g["gen"]: g for g in trajectory["generations"]}
    assert rows[0]["cached"] is True, "gen0 should be served from cache"
    assert rows[0]["fidelity_mean"] == 0.50, "cached gen0 value must be reused"
    assert rows[1]["cached"] is False, "gen1 should be freshly measured"
    # The stub must NOT have been called with the base adapter (None) ...
    assert None not in stub.adapters_called(), "cached gen0 must not invoke inference"
    # ... but MUST have been called for gen1's adapter.
    assert "/adapters/v4" in stub.adapters_called()
    # gen1 now persisted in the returned cache.
    assert traj.cache_key("/adapters/v4", FIX_SHA, PROBE_SHA) in new_cache
    print("✓ caching: gen0 reused without inference, gen1 measured + persisted")


def test_verdict_consistent_with_gate():
    """The embedded verdict equals evaluate_trajectory_gate re-run on the same
    measured generations (orchestrator wires measurement→gate faithfully)."""
    stub = RecordingStub()
    trajectory, _ = traj.run_trajectory(
        [GEN0, GEN1], PROMPTS, PROBES, FIX_SHA, PROBE_SHA, "m", stub
    )
    recomputed = evaluate_trajectory_gate(trajectory["generations"])
    assert recomputed.verdict == trajectory["verdict"], (
        f"verdict drift: embedded {trajectory['verdict']} vs recomputed {recomputed.verdict}"
    )
    print(f"✓ embedded verdict matches recomputed gate ({trajectory['verdict']})")


def test_single_generation_skips_exit_zero():
    stub = RecordingStub()
    trajectory, _ = traj.run_trajectory(
        [GEN0], PROMPTS, PROBES, FIX_SHA, PROBE_SHA, "m", stub
    )
    assert trajectory["verdict"] == "SKIP"
    assert trajectory["exit_code"] == 0
    print("✓ single generation → SKIP, exit_code 0")


def test_cache_key_composition():
    base = traj.cache_key(None, "f1", "p1")
    assert traj.cache_key(None, "f1", "p1") == base, "stable for same inputs"
    assert traj.cache_key("/a", "f1", "p1") != base, "adapter changes key"
    assert traj.cache_key(None, "f2", "p1") != base, "fixture sha changes key"
    assert traj.cache_key(None, "f1", "p2") != base, "probeset sha changes key"
    print("✓ cache_key composes over (adapter, fixture_sha, probeset_sha)")


def test_out_of_order_manifest_sorted():
    stub = RecordingStub()
    trajectory, _ = traj.run_trajectory(
        [GEN1, GEN0], PROMPTS, PROBES, FIX_SHA, PROBE_SHA, "m", stub
    )
    gens = [g["gen"] for g in trajectory["generations"]]
    assert gens == [0, 1], f"generations must be ordered by gen index, got {gens}"
    print("✓ out-of-order manifest is sorted by gen index")


def main():
    tests = [
        test_trajectory_schema,
        test_base_capability_deterministic_from_probes,
        test_caching_skips_measured_generation,
        test_verdict_consistent_with_gate,
        test_single_generation_skips_exit_zero,
        test_cache_key_composition,
        test_out_of_order_manifest_sorted,
    ]
    print("=" * 60)
    print("Testing eval_personalization_trajectory.py")
    print("=" * 60)
    passed = failed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"✗ {t.__name__}: {e}")
            failed += 1
        except Exception as e:
            print(f"✗ {t.__name__}: {type(e).__name__}: {e}")
            failed += 1
    print("=" * 60)
    print(f"Results: {passed} passed, {failed} failed")
    print("=" * 60)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
