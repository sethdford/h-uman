#!/usr/bin/env python3
"""Tests for scripts/dpo_results.py"""

import json
import sys
from datetime import datetime, timedelta
from pathlib import Path
from unittest.mock import patch

import pytest

# Add scripts directory to path
sys.path.insert(0, str(Path(__file__).parent.parent / 'scripts'))

import dpo_results


class TestDegenerateLoss:
    """Test degenerate loss detection (random baseline signature)."""

    def test_exact_degenerate_loss(self):
        """Loss exactly 0.6931 is degenerate."""
        assert dpo_results.is_degenerate_loss(0.6931) is True

    def test_near_degenerate_loss_within_threshold(self):
        """Loss within 0.001 of 0.6931 is degenerate."""
        assert dpo_results.is_degenerate_loss(0.6930) is True
        assert dpo_results.is_degenerate_loss(0.6932) is True
        assert dpo_results.is_degenerate_loss(0.69309) is True

    def test_non_degenerate_loss(self):
        """Loss far from 0.6931 is not degenerate."""
        assert dpo_results.is_degenerate_loss(0.5) is False
        assert dpo_results.is_degenerate_loss(0.7) is False
        assert dpo_results.is_degenerate_loss(1.0) is False

    def test_custom_threshold(self):
        """Custom threshold applies."""
        assert dpo_results.is_degenerate_loss(0.705, threshold=0.001) is False
        assert dpo_results.is_degenerate_loss(0.705, threshold=0.02) is True


class TestLoadRecent:
    """Test loading recent results from JSONL file."""

    def test_load_empty_file(self, tmp_path):
        """Load from non-existent file returns empty list."""
        results_file = tmp_path / "nonexistent.jsonl"
        assert dpo_results.load_recent(results_file) == []

    def test_load_single_record(self, tmp_path):
        """Load single valid record."""
        results_file = tmp_path / "results.jsonl"
        now = datetime.now()
        record = {
            'timestamp': now.isoformat(),
            'adapter_id': 'test-adapter',
            'val_loss': 0.5,
        }
        results_file.write_text(json.dumps(record) + '\n')

        loaded = dpo_results.load_recent(results_file)
        assert len(loaded) == 1
        assert loaded[0]['adapter_id'] == 'test-adapter'
        assert loaded[0]['val_loss'] == 0.5

    def test_load_skips_malformed_lines(self, tmp_path):
        """Malformed lines are skipped without crashing."""
        results_file = tmp_path / "results.jsonl"
        now = datetime.now()
        valid_record = {
            'timestamp': now.isoformat(),
            'adapter_id': 'valid',
            'val_loss': 0.5,
        }
        results_file.write_text(
            json.dumps(valid_record) + '\n'
            + 'invalid json line\n'
            + json.dumps(valid_record) + '\n'
        )

        loaded = dpo_results.load_recent(results_file)
        assert len(loaded) == 2
        assert all(r['adapter_id'] == 'valid' for r in loaded)

    def test_load_filters_by_days_back(self, tmp_path):
        """Only records within days_back are loaded."""
        results_file = tmp_path / "results.jsonl"

        now = datetime.now()
        old = (now - timedelta(days=40)).isoformat()
        recent = (now - timedelta(days=7)).isoformat()

        records = [
            {'timestamp': old, 'adapter_id': 'old'},
            {'timestamp': recent, 'adapter_id': 'recent'},
        ]
        results_file.write_text('\n'.join(json.dumps(r) for r in records) + '\n')

        loaded = dpo_results.load_recent(results_file, days_back=28)
        assert len(loaded) == 1
        assert loaded[0]['adapter_id'] == 'recent'


class TestRegressionVerdict:
    """Test regression verdict logic."""

    def test_first_run_no_history(self):
        """No history → FIRST_RUN."""
        result = {'val_loss': 0.5}
        verdict = dpo_results.regression_verdict([], result)
        assert verdict == 'FIRST_RUN'

    def test_pass_within_threshold(self):
        """Val loss within threshold of best → PASS."""
        history = [
            {'val_loss': 0.5},
            {'val_loss': 0.55},
        ]
        result = {'val_loss': 0.55}  # at best + 0.05
        verdict = dpo_results.regression_verdict(history, result)
        assert verdict == 'PASS'

    def test_fail_exceeds_threshold(self):
        """Val loss exceeds best + threshold → FAIL."""
        history = [{'val_loss': 0.5}]
        result = {'val_loss': 0.65}  # best + 0.15 > threshold of 0.1
        verdict = dpo_results.regression_verdict(history, result)
        assert verdict == 'FAIL'

    def test_fail_degenerate_signature(self):
        """Degenerate signature → FAIL even with good history."""
        history = [{'val_loss': 0.5}]
        result = {'val_loss': 0.6931}
        verdict = dpo_results.regression_verdict(history, result)
        assert verdict == 'FAIL'

    def test_pass_no_val_loss(self):
        """No val loss → PASS (can't judge)."""
        result = {'val_loss': None}
        verdict = dpo_results.regression_verdict([], result)
        assert verdict == 'PASS'

    def test_custom_threshold_delta(self):
        """Custom threshold delta applies."""
        history = [{'val_loss': 0.5}]
        result = {'val_loss': 0.55}

        # With default threshold (0.1), 0.55 is at best + 0.05 = PASS
        assert dpo_results.regression_verdict(history, result) == 'PASS'

        # With threshold 0.01, 0.55 is at best + 0.05 > 0.01 = FAIL
        assert dpo_results.regression_verdict(history, result, loss_threshold_delta=0.01) == 'FAIL'


class TestAppendResult:
    """Test appending results to JSONL."""

    def test_append_creates_file(self, tmp_path):
        """Appending creates the file if it doesn't exist."""
        results_file = tmp_path / "results.jsonl"

        dpo_results.append_result(
            results_file,
            datetime.now().isoformat(),
            'adapter-1',
            {'source_a': 100},
            0.4,
            0.5,
            0.85,
            2.0,
            100,
            'abc123'
        )

        assert results_file.exists()

        with open(results_file) as f:
            record = json.loads(f.read().strip())

        assert record['adapter_id'] == 'adapter-1'
        assert record['n_pairs_by_source'] == {'source_a': 100}
        assert record['train_loss'] == 0.4
        assert record['val_loss'] == 0.5

    def test_append_creates_parent_dirs(self, tmp_path):
        """Appending creates parent directories."""
        results_file = tmp_path / "deep" / "nested" / "results.jsonl"

        dpo_results.append_result(
            results_file,
            datetime.now().isoformat(),
            'adapter-1',
            {},
            None,
            0.5,
            None,
            2.0,
            100,
            'abc123'
        )

        assert results_file.exists()

    def test_append_multiple_records(self, tmp_path):
        """Multiple appends create multiple lines."""
        results_file = tmp_path / "results.jsonl"

        for i in range(3):
            dpo_results.append_result(
                results_file,
                datetime.now().isoformat(),
                f'adapter-{i}',
                {},
                None,
                0.5 + i * 0.1,
                None,
                2.0,
                100 + i,
                f'commit-{i}'
            )

        lines = results_file.read_text().strip().split('\n')
        assert len(lines) == 3

        for i, line in enumerate(lines):
            record = json.loads(line)
            assert record['adapter_id'] == f'adapter-{i}'
            assert record['iters'] == 100 + i


class TestParseMlxLosses:
    """Test parsing of mlx-lm training output to extract losses."""

    def test_parse_both_losses(self):
        """Parse both training and validation losses from output."""
        output = """
Iter 1: loss 0.8234, chosen_r 10.234, rejected_r 10.456, acc 0.500
Iter 2: loss 0.7923, chosen_r 11.234, rejected_r 10.856, acc 0.600
Iter 5: loss 0.5234, chosen_r 15.234, rejected_r 14.856, acc 0.800
Val loss 0.4567
Iter 10: loss 0.4123, chosen_r 16.234, rejected_r 16.856, acc 0.900
        """
        train_loss, val_loss = dpo_results.parse_mlx_losses(output)
        assert train_loss == 0.4123  # Last training loss
        assert val_loss == 0.4567    # Validation loss

    def test_parse_train_loss_only(self):
        """Handle output with only training loss."""
        output = """
Iter 1: loss 0.8234, chosen_r 10.234, acc 0.500
Iter 5: loss 0.5234, chosen_r 15.234, acc 0.800
        """
        train_loss, val_loss = dpo_results.parse_mlx_losses(output)
        assert train_loss == 0.5234
        assert val_loss is None

    def test_parse_empty_output(self):
        """Handle empty or no-loss output."""
        train_loss, val_loss = dpo_results.parse_mlx_losses("")
        assert train_loss is None
        assert val_loss is None

    def test_parse_takes_last_occurrence(self):
        """Verify parser takes LAST occurrence of each loss type."""
        output = """
Iter 1: loss 0.9000
Iter 2: loss 0.8000
Val loss 0.5000
Iter 3: loss 0.7000
Val loss 0.4000
Iter 4: loss 0.6000
        """
        train_loss, val_loss = dpo_results.parse_mlx_losses(output)
        assert train_loss == 0.6000  # Last training
        assert val_loss == 0.4000    # Last validation

    def test_parse_scientific_notation(self):
        """Handle scientific notation in losses."""
        output = "Iter 1: loss 1.234e-3\nVal loss 5.67e-2"
        train_loss, val_loss = dpo_results.parse_mlx_losses(output)
        assert abs(train_loss - 0.001234) < 1e-6
        assert abs(val_loss - 0.0567) < 1e-4


class TestEndToEnd:
    """End-to-end test of the full workflow."""

    def test_train_run_workflow(self, tmp_path):
        """Simulate a complete training run and verdict workflow."""
        results_file = tmp_path / "results.jsonl"

        # First run (baseline)
        dpo_results.append_result(
            results_file,
            (datetime.now() - timedelta(days=7)).isoformat(),
            'adapter-v1',
            {'imessage': 150},
            0.45,
            0.50,
            0.88,
            2.0,
            100,
            'commit1'
        )

        # Second run (good improvement)
        dpo_results.append_result(
            results_file,
            (datetime.now() - timedelta(days=1)).isoformat(),
            'adapter-v2',
            {'imessage': 160},
            0.42,
            0.48,
            0.90,
            2.0,
            100,
            'commit2'
        )

        history = dpo_results.load_recent(results_file)
        assert len(history) == 2

        # New run should pass (0.49 < 0.48 + 0.1)
        new_result = {'val_loss': 0.49}
        verdict = dpo_results.regression_verdict(history, new_result)
        assert verdict == 'PASS'

        # New run should fail (0.60 > 0.48 + 0.1)
        new_result = {'val_loss': 0.60}
        verdict = dpo_results.regression_verdict(history, new_result)
        assert verdict == 'FAIL'

        # Degenerate run should fail
        new_result = {'val_loss': 0.6931}
        verdict = dpo_results.regression_verdict(history, new_result)
        assert verdict == 'FAIL'

    def test_parsed_losses_flow_to_fail_verdict(self, tmp_path):
        """Simulate training_loop.py: parse losses → record → check verdict."""
        results_file = tmp_path / "results.jsonl"

        # Establish baseline: val_loss=0.50
        dpo_results.append_result(
            results_file,
            (datetime.now() - timedelta(days=7)).isoformat(),
            'adapter-baseline',
            {'outcomes': 42},
            0.45,
            0.50,
            0.88,
            2.0,
            500,
            'commit-baseline'
        )

        # Simulate mlx_lm output with worse val_loss
        mlx_output = """
Iter 1: loss 0.8234, chosen_r 10.234, acc 0.500
Iter 100: loss 0.5234, chosen_r 15.234, acc 0.800
Val loss 0.6200
        """

        # Parse like training_loop.py does
        train_loss, val_loss = dpo_results.parse_mlx_losses(mlx_output)
        assert train_loss == 0.5234
        assert val_loss == 0.6200

        # Record the result (as training_loop.py quality gate does)
        dpo_results.append_result(
            results_file,
            datetime.now().isoformat(),
            'adapter-worse',
            {'outcomes': 48},
            train_loss,
            val_loss,
            None,
            2.0,
            500,
            'commit-new'
        )

        # Check regression verdict
        history = dpo_results.load_recent(results_file)
        verdict = dpo_results.regression_verdict(history, {'val_loss': val_loss})

        # Should FAIL because 0.6200 > 0.50 + 0.1
        assert verdict == 'FAIL'


if __name__ == '__main__':
    pytest.main([__file__, '-v'])
