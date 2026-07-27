#!/usr/bin/env python3
"""Unit tests for six-axis scoring in score.py.

Tests cover:
  - Likert [1-5] → [0-1] conversion
  - Per-axis aggregation from rater responses
  - Backward compatibility with legacy mean_score field
"""
import unittest
import sys
import os

# Import score functions
sys.path.insert(0, os.path.dirname(__file__))
from score import likert_to_01, score_axes, score_rows


class TestLikertConversion(unittest.TestCase):
    """Test Likert [1-5] → [0-1] conversion."""

    def test_likert_1_maps_to_0(self):
        """Likert 1 should map to 0.0."""
        self.assertAlmostEqual(likert_to_01(1), 0.0)

    def test_likert_3_maps_to_0_5(self):
        """Likert 3 (midpoint) should map to 0.5."""
        self.assertAlmostEqual(likert_to_01(3), 0.5)

    def test_likert_5_maps_to_1(self):
        """Likert 5 should map to 1.0."""
        self.assertAlmostEqual(likert_to_01(5), 1.0)

    def test_likert_2_maps_correctly(self):
        """Likert 2 should map to 0.25."""
        self.assertAlmostEqual(likert_to_01(2), 0.25)

    def test_likert_4_maps_correctly(self):
        """Likert 4 should map to 0.75."""
        self.assertAlmostEqual(likert_to_01(4), 0.75)

    def test_likert_string_1_maps_to_0(self):
        """String "1" should convert and map to 0.0."""
        self.assertAlmostEqual(likert_to_01("1"), 0.0)

    def test_likert_string_3_maps_to_0_5(self):
        """String "3" should convert and map to 0.5."""
        self.assertAlmostEqual(likert_to_01("3"), 0.5)

    def test_empty_string_returns_none(self):
        """Empty string should return None (missing rating)."""
        self.assertIsNone(likert_to_01(""))

    def test_invalid_string_returns_none(self):
        """Invalid string should return None."""
        self.assertIsNone(likert_to_01("invalid"))

    def test_out_of_range_low_returns_none(self):
        """Likert < 1 should return None."""
        self.assertIsNone(likert_to_01(0))
        self.assertIsNone(likert_to_01(0.5))

    def test_out_of_range_high_returns_none(self):
        """Likert > 5 should return None."""
        self.assertIsNone(likert_to_01(6))
        self.assertIsNone(likert_to_01(10))

    def test_none_input_returns_none(self):
        """None input should return None."""
        self.assertIsNone(likert_to_01(None))


class TestAxisAggregation(unittest.TestCase):
    """Test per-axis score aggregation."""

    def test_all_neutral_ratings(self):
        """All ratings of 3 (midpoint) should aggregate to 0.5."""
        rows = [
            {
                "axis_opinion": 3,
                "axis_memory": 3,
                "axis_reasoning": 3,
                "axis_lexical": 3,
                "axis_tone": 3,
                "axis_syntax": 3,
                "_rater": "r1",
            },
            {
                "axis_opinion": 3,
                "axis_memory": 3,
                "axis_reasoning": 3,
                "axis_lexical": 3,
                "axis_tone": 3,
                "axis_syntax": 3,
                "_rater": "r2",
            },
        ]
        axes = score_axes(rows)
        for ax in ["opinion", "memory", "reasoning", "lexical", "tone", "syntax"]:
            self.assertAlmostEqual(axes[ax], 0.5, places=5)

    def test_all_perfect_ratings(self):
        """All ratings of 5 should aggregate to 1.0."""
        rows = [
            {
                "axis_opinion": 5,
                "axis_memory": 5,
                "axis_reasoning": 5,
                "axis_lexical": 5,
                "axis_tone": 5,
                "axis_syntax": 5,
                "_rater": "r1",
            },
            {
                "axis_opinion": 5,
                "axis_memory": 5,
                "axis_reasoning": 5,
                "axis_lexical": 5,
                "axis_tone": 5,
                "axis_syntax": 5,
                "_rater": "r2",
            },
        ]
        axes = score_axes(rows)
        for ax in ["opinion", "memory", "reasoning", "lexical", "tone", "syntax"]:
            self.assertAlmostEqual(axes[ax], 1.0, places=5)

    def test_all_worst_ratings(self):
        """All ratings of 1 should aggregate to 0.0."""
        rows = [
            {
                "axis_opinion": 1,
                "axis_memory": 1,
                "axis_reasoning": 1,
                "axis_lexical": 1,
                "axis_tone": 1,
                "axis_syntax": 1,
                "_rater": "r1",
            },
        ]
        axes = score_axes(rows)
        for ax in ["opinion", "memory", "reasoning", "lexical", "tone", "syntax"]:
            self.assertAlmostEqual(axes[ax], 0.0, places=5)

    def test_mixed_ratings_mean(self):
        """Mixed ratings should average correctly."""
        rows = [
            {
                "axis_opinion": 5,
                "axis_memory": 1,
                "axis_reasoning": 3,
                "axis_lexical": 5,
                "axis_tone": 5,
                "axis_syntax": 3,
                "_rater": "r1",
            },
            {
                "axis_opinion": 5,
                "axis_memory": 1,
                "axis_reasoning": 3,
                "axis_lexical": 5,
                "axis_tone": 5,
                "axis_syntax": 3,
                "_rater": "r2",
            },
        ]
        axes = score_axes(rows)
        self.assertAlmostEqual(axes["opinion"], 1.0)  # (5+5)/2 → 1.0
        self.assertAlmostEqual(axes["memory"], 0.0)   # (1+1)/2 → 0.0
        self.assertAlmostEqual(axes["reasoning"], 0.5)  # (3+3)/2 → 0.5
        self.assertAlmostEqual(axes["lexical"], 1.0)  # (5+5)/2 → 1.0
        self.assertAlmostEqual(axes["tone"], 1.0)    # (5+5)/2 → 1.0
        self.assertAlmostEqual(axes["syntax"], 0.5)  # (3+3)/2 → 0.5

    def test_missing_axis_columns_ignored(self):
        """Rows with missing axis columns should not crash."""
        rows = [
            {
                "axis_opinion": 5,
                # memory is missing
                "axis_reasoning": 4,
                "axis_lexical": 5,
                "axis_tone": 5,
                "axis_syntax": 4,
                "_rater": "r1",
            },
            {
                "axis_opinion": 5,
                "axis_memory": 3,
                "axis_reasoning": 4,
                "axis_lexical": 5,
                "axis_tone": 5,
                "axis_syntax": 4,
                "_rater": "r2",
            },
        ]
        axes = score_axes(rows)
        # Should not crash; opinion, reasoning, lexical, tone, syntax aggregated normally
        self.assertAlmostEqual(axes["opinion"], 1.0)
        self.assertAlmostEqual(axes["reasoning"], 0.75)  # (4+4)/2 → 0.75
        # memory has only one rating (3), so mean = 0.5
        self.assertAlmostEqual(axes["memory"], 0.5)

    def test_blank_axis_values_skipped(self):
        """Blank/empty axis values should be skipped."""
        rows = [
            {
                "axis_opinion": 5,
                "axis_memory": "",  # blank
                "axis_reasoning": 4,
                "axis_lexical": 5,
                "axis_tone": 5,
                "axis_syntax": 4,
                "_rater": "r1",
            },
        ]
        axes = score_axes(rows)
        # opinion, reasoning, lexical, tone, syntax should have values
        self.assertAlmostEqual(axes["opinion"], 1.0)
        self.assertAlmostEqual(axes["reasoning"], 0.75)
        # memory has no valid rating, defaults to 0.0 (or count=0)
        # The implementation returns 0.0 when count=0
        self.assertEqual(axes["memory"], 0.0)


class TestBackwardCompatibility(unittest.TestCase):
    """Test backward compatibility with legacy score.py schema."""

    def test_legacy_fields_present_in_output(self):
        """score_rows output must contain legacy detection fields."""
        rows = [
            {
                "id": "t1",
                "choice": "A",
                "confidence": 5,
                "axis_opinion": 5,
                "axis_memory": 5,
                "axis_reasoning": 5,
                "axis_lexical": 5,
                "axis_tone": 5,
                "axis_syntax": 5,
                "_rater": "test",
            },
        ]
        key = {"t1": "A"}
        agg = score_rows(rows, key)

        # Check legacy fields
        self.assertIn("detect", agg)
        self.assertIn("n", agg)
        self.assertIn("ci_lo", agg)
        self.assertIn("ci_hi", agg)
        self.assertIn("weighted_detect", agg)
        self.assertIn("per_rater", agg)

    def test_detect_rate_unchanged_by_axes(self):
        """Adding axis columns should not change detection rate."""
        # Without axes
        rows_no_axes = [
            {"id": "t1", "choice": "A", "confidence": 5, "_rater": "r1"},
            {"id": "t2", "choice": "B", "confidence": 4, "_rater": "r1"},
            {"id": "t3", "choice": "A", "confidence": 3, "_rater": "r1"},
        ]
        key = {"t1": "A", "t2": "A", "t3": "B"}
        agg_no_axes = score_rows(rows_no_axes, key)

        # Same data with axes
        rows_with_axes = [
            {
                "id": "t1",
                "choice": "A",
                "confidence": 5,
                "axis_opinion": 4,
                "axis_memory": 3,
                "axis_reasoning": 5,
                "axis_lexical": 4,
                "axis_tone": 5,
                "axis_syntax": 4,
                "_rater": "r1",
            },
            {
                "id": "t2",
                "choice": "B",
                "confidence": 4,
                "axis_opinion": 3,
                "axis_memory": 4,
                "axis_reasoning": 3,
                "axis_lexical": 3,
                "axis_tone": 4,
                "axis_syntax": 3,
                "_rater": "r1",
            },
            {
                "id": "t3",
                "choice": "A",
                "confidence": 3,
                "axis_opinion": 2,
                "axis_memory": 2,
                "axis_reasoning": 2,
                "axis_lexical": 2,
                "axis_tone": 2,
                "axis_syntax": 2,
                "_rater": "r1",
            },
        ]
        agg_with_axes = score_rows(rows_with_axes, key)

        # Detection rates should be identical
        self.assertAlmostEqual(agg_no_axes["detect"], agg_with_axes["detect"])
        self.assertEqual(agg_no_axes["n"], agg_with_axes["n"])


class TestRaterGateSeparation(unittest.TestCase):
    """End-to-end pinning tests for the --rater gate-write separation.

    Background (2026-07-26): a synthetic-judge run of score.py (n=160,
    machine rater) overwrote the genuine cycle-3 human verdict (detection
    0.500, n=12) in ~/.human/blind_ab_gate.json because main() wrote the
    "human" key unconditionally. These tests pin the fix: gate writes are
    opt-in via --rater, and synthetic runs must never touch the human key.
    """

    SCORE_PY = os.path.join(os.path.dirname(__file__), "score.py")

    def setUp(self):
        import tempfile
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        d = self.tmp.name
        self.home = os.path.join(d, "home")
        os.makedirs(os.path.join(self.home, ".human"))
        self.home_gate = os.path.join(self.home, ".human", "blind_ab_gate.json")
        self.repo_gate = os.path.join(d, "gate.json")
        self.sheet = os.path.join(d, "sheet.csv")
        self.key = os.path.join(d, "key.json")
        self._write_fixture()

    def _write_fixture(self):
        import csv as _csv
        import json as _json
        with open(self.sheet, "w", newline="") as f:
            w = _csv.writer(f)
            w.writerow(["id", "choice", "confidence"])
            for i in range(4):
                w.writerow([str(i), "A" if i < 2 else "B", "4"])
        with open(self.key, "w") as f:
            _json.dump({str(i): "A" for i in range(4)}, f)

    def _seed_human_verdict(self):
        """Pre-populate a genuine human verdict in both gate files."""
        import json as _json
        genuine = {"verdict": "PASS", "detection": 0.5, "ci_lo": 0.2538,
                   "n": 12, "timestamp": "2026-07-26T04:38:02"}
        with open(self.home_gate, "w") as f:
            _json.dump({"human": dict(genuine)}, f)
        with open(self.repo_gate, "w") as f:
            _json.dump({"schema_version": 1, "commit": None,
                        "proxy": {"verdict": "ADVISORY", "mode": "ADVISORY"},
                        "human": dict(genuine),
                        "effective_verdict": "ADVISORY"}, f)
        return genuine

    def _run_score(self, *extra):
        import subprocess
        env = dict(os.environ, HOME=self.home)
        return subprocess.run(
            [sys.executable, self.SCORE_PY, self.sheet, "--key", self.key]
            + list(extra),
            capture_output=True, text=True, env=env, timeout=60)

    def test_no_rater_writes_no_gate_files(self):
        """Scoring-only invocation (no --rater) must print but write nothing."""
        genuine = self._seed_human_verdict()
        import json as _json
        r = self._run_score()
        self.assertIn(r.returncode, (0, 1), r.stderr)
        self.assertIn("RESULT_blind_ab=", r.stdout)
        with open(self.home_gate) as f:
            data = _json.load(f)
        self.assertEqual(data, {"human": genuine},
                         "no-rater run must not touch the home gate file")

    def test_emit_gate_without_rater_is_an_error(self):
        """--emit-gate without --rater must fail loudly, not write silently."""
        r = self._run_score("--emit-gate", self.repo_gate)
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertFalse(os.path.exists(self.repo_gate))

    def test_synthetic_does_not_touch_human_key(self):
        """rater=synthetic must leave the human verdict byte-identical and
        record its result under a separate 'synthetic' key."""
        genuine = self._seed_human_verdict()
        import json as _json
        r = self._run_score("--rater", "synthetic",
                            "--emit-gate", self.repo_gate)
        self.assertIn(r.returncode, (0, 1), r.stderr)
        # Home gate: human key unchanged, synthetic recorded separately.
        with open(self.home_gate) as f:
            home = _json.load(f)
        self.assertEqual(home["human"], genuine,
                         "synthetic run overwrote the human verdict")
        self.assertIn("synthetic", home)
        self.assertEqual(home["synthetic"]["n"], 4)
        # Repo gate: human half unchanged, synthetic half present, and the
        # effective verdict is still computed from proxy+human only.
        with open(self.repo_gate) as f:
            repo = _json.load(f)
        self.assertEqual(repo["human"], genuine,
                         "synthetic run overwrote the repo human half")
        self.assertIn("synthetic", repo)
        self.assertEqual(repo["synthetic"]["n"], 4)
        self.assertEqual(repo["effective_verdict"], "ADVISORY")

    def _write_stamped_fixture(self):
        """Sheet whose rows carry the judge_api/judge_model provenance stamps
        that synthetic_judge.py writes on every judged row."""
        import csv as _csv
        with open(self.sheet, "w", newline="") as f:
            w = _csv.writer(f)
            w.writerow(["id", "choice", "confidence", "judge_api", "judge_model"])
            for i in range(4):
                w.writerow([str(i), "A" if i < 2 else "B", "4",
                            "openai", "gemma-4-31b-it-8bit"])

    def test_stamped_sheet_vetoes_rater_human(self):
        """A sheet stamped by synthetic_judge.py must refuse --rater human:
        provenance beats the operator's claim (fail safe)."""
        genuine = self._seed_human_verdict()
        import json as _json
        self._write_stamped_fixture()
        r = self._run_score("--rater", "human", "--emit-gate", self.repo_gate)
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        with open(self.home_gate) as f:
            self.assertEqual(_json.load(f), {"human": genuine},
                             "vetoed run must write nothing")

    def test_stamped_sheet_as_synthetic_records_judge_model(self):
        """--rater synthetic on a stamped sheet works and carries the
        judge_model provenance into the synthetic gate record."""
        import json as _json
        self._write_stamped_fixture()
        r = self._run_score("--rater", "synthetic")
        self.assertIn(r.returncode, (0, 1), r.stderr)
        with open(self.home_gate) as f:
            home = _json.load(f)
        self.assertEqual(home["synthetic"]["judge_model"], "gemma-4-31b-it-8bit")

    def test_human_rater_writes_human_key_and_preserves_synthetic(self):
        """rater=human keeps the original behavior and must not clobber a
        previously-recorded synthetic key."""
        import json as _json
        with open(self.home_gate, "w") as f:
            _json.dump({"synthetic": {"verdict": "PASS", "n": 160}}, f)
        r = self._run_score("--rater", "human", "--emit-gate", self.repo_gate)
        self.assertIn(r.returncode, (0, 1), r.stderr)
        with open(self.home_gate) as f:
            home = _json.load(f)
        self.assertEqual(home["human"]["n"], 4)
        self.assertIn(home["human"]["verdict"], ("PASS", "FAIL"))
        self.assertEqual(home["synthetic"], {"verdict": "PASS", "n": 160},
                         "human run clobbered the synthetic record")
        with open(self.repo_gate) as f:
            repo = _json.load(f)
        self.assertEqual(repo["human"]["n"], 4)


if __name__ == "__main__":
    unittest.main()
