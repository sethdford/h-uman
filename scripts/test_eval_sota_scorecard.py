#!/usr/bin/env python3
"""
Tests for the personalization-trajectory section of eval_sota_scorecard.py
(Dermot SOTA spec T7 / AC-6).

render_trajectory_lines is pure (dict in, lines out, no DB), so these run
without ~/.human/memory.db.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from eval_sota_scorecard import render_trajectory_lines

PASS_TRAJ = {
    "verdict": "PASS",
    "generations": [
        {"gen": 0, "label": "base", "fidelity_mean": 0.586,
         "fidelity_ci": [0.49, 0.68], "base_capability": 0.94, "train_pairs": 0},
        {"gen": 1, "label": "v4-repair", "fidelity_mean": 0.856,
         "fidelity_ci": [0.78, 0.92], "base_capability": 0.93, "train_pairs": 1963},
    ],
    "gate": {"verdict": "PASS", "details": ["curve non-decreasing within 1 stderr"]},
}

FAIL_TRAJ = {
    "verdict": "FAIL",
    "generations": [
        {"gen": 0, "label": "base", "fidelity_mean": 0.586,
         "fidelity_ci": [0.49, 0.68], "base_capability": 0.94, "train_pairs": 0},
        {"gen": 1, "label": "scale20", "fidelity_mean": 0.90,
         "fidelity_ci": [0.85, 0.95], "base_capability": 0.55, "train_pairs": 500},
    ],
    "gate": {"verdict": "FAIL", "failing_axis": "base_capability", "failing_gen": 1,
             "details": ["gen 1 base_capability 0.550 < floor 0.890"]},
}


def test_plain_pass_section():
    out = "\n".join(render_trajectory_lines(PASS_TRAJ, markdown=False))
    assert "VERDICT: PASS" in out
    assert "base" in out and "v4-repair" in out
    assert "0.586" in out and "0.856" in out
    assert "0.940" in out  # base_capability rendered
    print("✓ plain PASS section renders verdict, both gens, fidelity + base_cap")


def test_markdown_pass_section():
    out = "\n".join(render_trajectory_lines(PASS_TRAJ, markdown=True))
    assert "## Personalization trajectory" in out
    assert "**Verdict: PASS**" in out
    assert "| gen |" in out  # table header
    assert "[0.780, 0.920]" in out  # gen1 CI formatted
    print("✓ markdown PASS section renders header, verdict, table, CI")


def test_fail_section_names_axis():
    out = "\n".join(render_trajectory_lines(FAIL_TRAJ, markdown=False))
    assert "VERDICT: FAIL" in out
    assert "FAILING AXIS: base_capability at gen 1" in out
    assert "0.550" in out  # the collapsed base-capability is visible
    print("✓ FAIL section names the failing axis + gen and shows the collapse")


def test_empty_trajectory_is_safe():
    out = "\n".join(render_trajectory_lines({"verdict": "SKIP", "generations": []}))
    assert "VERDICT: SKIP" in out
    print("✓ empty/SKIP trajectory renders without error")


def test_missing_ci_renders_dash():
    traj = {
        "verdict": "SKIP",
        "generations": [{"gen": 0, "label": "base", "fidelity_mean": 0.5,
                         "base_capability": 0.9, "train_pairs": 0}],
        "gate": {},
    }
    out = "\n".join(render_trajectory_lines(traj, markdown=True))
    assert "—" in out  # missing fidelity_ci → dash, no crash
    print("✓ missing fidelity_ci renders an em-dash, not a crash")


def main():
    tests = [
        test_plain_pass_section,
        test_markdown_pass_section,
        test_fail_section_names_axis,
        test_empty_trajectory_is_safe,
        test_missing_ci_renders_dash,
    ]
    print("=" * 60)
    print("Testing eval_sota_scorecard.py (trajectory section)")
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
