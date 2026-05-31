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


if __name__ == "__main__":
    unittest.main()
