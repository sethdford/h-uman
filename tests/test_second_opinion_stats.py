"""Measurement math for the second-opinion lane (spec §4, §5)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import stats  # noqa: E402


def test_wilson_matches_known_values():
    lo, hi = stats.wilson(5, 10)
    assert abs(lo - 0.2366) < 1e-3 and abs(hi - 0.7634) < 1e-3
    lo, hi = stats.wilson(0, 10)
    assert lo < 1e-3 and abs(hi - 0.2775) < 1e-3
    assert stats.wilson(0, 0) is None


def test_rate_never_reports_a_number_without_a_denominator():
    assert stats.rate(0, 0) == stats.NOT_MEASURED
    assert stats.rate(3, 10, min_n=20) == stats.NOT_MEASURED
    r = stats.rate(3, 10)
    assert r["k"] == 3 and r["n"] == 10 and r["rate"] == 0.3 and len(r["ci95"]) == 2


def test_cohen_kappa_chance_and_perfect():
    assert stats.cohen_kappa([]) is None
    assert stats.cohen_kappa([("A", "A"), ("B", "B")]) == 1.0
    assert abs(stats.cohen_kappa([("A", "A"), ("B", "B"), ("A", "B"), ("B", "A")])) < 1e-9
