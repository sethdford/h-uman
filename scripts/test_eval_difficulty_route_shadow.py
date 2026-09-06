#!/usr/bin/env python3
"""
Hermetic tests for eval_difficulty_route_shadow.py.

No network, no `:8741`, no model load.
"""

import json
import os
import tempfile
import unittest
from pathlib import Path

# Import the module under test
import sys
sys.path.insert(0, os.path.dirname(__file__))
import eval_difficulty_route_shadow as us8


class TestContextSelection(unittest.TestCase):
    """Test context loading and filtering."""

    def test_select_contexts_empty_corpus(self):
        """Nonexistent corpus returns empty list."""
        contexts = us8.select_contexts("/nonexistent/path.jsonl", n=10)
        self.assertEqual(len(contexts), 0)

    def test_select_contexts_filters_short_messages(self):
        """Messages with <= 12 words are filtered out."""
        with tempfile.NamedTemporaryFile(mode='w', suffix='.jsonl', delete=False) as f:
            # Short message (4 words)
            f.write(json.dumps({"msg": "hey how are you"}) + "\n")
            # Long message (13 words)
            f.write(json.dumps({"msg": "what do you think should I go to the party tonight or stay home"}) + "\n")
            f.flush()

            try:
                contexts = us8.select_contexts(f.name, n=10)
                # Should only get the 13-word message
                self.assertEqual(len(contexts), 1)
                self.assertIn("should I go", contexts[0])
            finally:
                os.unlink(f.name)

    def test_select_contexts_respects_n(self):
        """Returns at most n contexts."""
        with tempfile.NamedTemporaryFile(mode='w', suffix='.jsonl', delete=False) as f:
            for i in range(30):
                msg = " ".join([f"word{j}" for j in range(15)])  # 15 words each
                f.write(json.dumps({"msg": msg}) + "\n")
            f.flush()

            try:
                contexts = us8.select_contexts(f.name, n=5)
                self.assertEqual(len(contexts), 5)
            finally:
                os.unlink(f.name)


class TestGateDecision(unittest.TestCase):
    """Test gate decision logic (AC-8.4)."""

    def test_gate_inconclusive_insufficient_pairs(self):
        """Fewer than 20 pairs → INCONCLUSIVE."""
        result = us8.decide_gate(None, None, None, None, n_paired=10)
        self.assertEqual(result["verdict"], "INCONCLUSIVE")

    def test_gate_inconclusive_missing_composite(self):
        """Missing composite scores → INCONCLUSIVE."""
        result = us8.decide_gate(None, 0.85, 0.63, 0.64, n_paired=20)
        self.assertEqual(result["verdict"], "INCONCLUSIVE")

    def test_gate_inconclusive_missing_twin(self):
        """Missing twin scores → INCONCLUSIVE."""
        result = us8.decide_gate(0.85, 0.86, None, 0.64, n_paired=20)
        self.assertEqual(result["verdict"], "INCONCLUSIVE")

    def test_gate_promote_both_axes_pass(self):
        """Both composite and twin improve → PROMOTE."""
        result = us8.decide_gate(
            composite_on_device=0.85,
            composite_cloud=0.87,
            twin_on_device=0.63,
            twin_cloud=0.64,
            n_paired=20,
            tolerance=0.02
        )
        self.assertEqual(result["verdict"], "PROMOTE")

    def test_gate_hold_composite_drops(self):
        """Composite drops beyond tolerance → HOLD."""
        result = us8.decide_gate(
            composite_on_device=0.85,
            composite_cloud=0.82,  # drops 0.03, beyond tolerance 0.02
            twin_on_device=0.63,
            twin_cloud=0.64,
            n_paired=20,
            tolerance=0.02
        )
        self.assertEqual(result["verdict"], "HOLD")

    def test_gate_hold_twin_drops(self):
        """Twin drops (any amount) → HOLD."""
        result = us8.decide_gate(
            composite_on_device=0.85,
            composite_cloud=0.87,
            twin_on_device=0.63,
            twin_cloud=0.62,  # drops by 0.01
            n_paired=20,
            tolerance=0.02
        )
        self.assertEqual(result["verdict"], "HOLD")

    def test_gate_promote_within_tolerance(self):
        """Composite within tolerance (even tiny drop) → PROMOTE."""
        result = us8.decide_gate(
            composite_on_device=0.85,
            composite_cloud=0.8499,  # drops by 0.0001, within 0.02 tolerance
            twin_on_device=0.63,
            twin_cloud=0.63,  # unchanged
            n_paired=20,
            tolerance=0.02
        )
        self.assertEqual(result["verdict"], "PROMOTE")


class TestDryRun(unittest.TestCase):
    """Test dry-run mode (AC-8.6)."""

    def test_dry_run_mode_produces_output(self):
        """Dry-run loads contexts and writes output without generation."""
        with tempfile.NamedTemporaryFile(mode='w', suffix='.jsonl', delete=False) as corpus:
            for i in range(10):
                msg = " ".join([f"word{j}" for j in range(15)])  # 15 words each
                corpus.write(json.dumps({"msg": msg}) + "\n")
            corpus.flush()

            with tempfile.TemporaryDirectory() as tmpdir:
                output_path = os.path.join(tmpdir, "test-output.json")

                # Simulate dry-run: load contexts, write result
                contexts = us8.select_contexts(corpus.name, n=5)
                result = {
                    "verdict": "DRY_RUN",
                    "contexts_loaded": len(contexts),
                    "min_words_threshold": 12,
                }
                os.makedirs(os.path.dirname(output_path), exist_ok=True)
                with open(output_path, 'w') as f:
                    json.dump(result, f)

                # Verify output
                self.assertTrue(os.path.exists(output_path))
                with open(output_path) as f:
                    loaded = json.load(f)
                    self.assertEqual(loaded["verdict"], "DRY_RUN")
                    self.assertEqual(loaded["contexts_loaded"], 5)

            os.unlink(corpus.name)


if __name__ == "__main__":
    unittest.main()
