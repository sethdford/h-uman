#!/usr/bin/env python3
"""
Tests for the adapter experiment registry.

Tests the roundtrip of recording/loading adapter training, eval, and promotion
events, plus the status reporting that flags stale evaluations and never-promoted
live adapters.
"""

import json
import os
import sys
import tempfile
from datetime import datetime, timedelta
from pathlib import Path

# Add scripts to path so we can import adapter_registry
sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))

import adapter_registry


def test_record_and_load_training():
    """Test recording and loading training results."""
    with tempfile.TemporaryDirectory() as tmpdir:
        registry_path = Path(tmpdir) / "registry.json"

        # Record a training result
        adapter_registry.record_training(
            registry_path=registry_path,
            adapter_id="test-adapter-v1",
            metrics={
                "n_pairs": 100,
                "train_loss": 0.45,
                "val_loss": 0.52,
                "timestamp": datetime.now().isoformat(),
            }
        )

        # Load and verify
        registry = adapter_registry.load_registry(registry_path)
        assert "test-adapter-v1" in registry["adapters"]
        assert "training" in registry["adapters"]["test-adapter-v1"]
        assert len(registry["adapters"]["test-adapter-v1"]["training"]) == 1

        training = registry["adapters"]["test-adapter-v1"]["training"][0]
        assert training["metrics"]["n_pairs"] == 100
        assert training["metrics"]["train_loss"] == 0.45


def test_record_and_load_eval():
    """Test recording and loading eval results."""
    with tempfile.TemporaryDirectory() as tmpdir:
        registry_path = Path(tmpdir) / "registry.json"

        # Record an eval result
        adapter_registry.record_eval(
            registry_path=registry_path,
            adapter_id="test-adapter-v1",
            eval_name="fidelity-nightly",
            score=0.78,
            verdict="PASS",
            timestamp=datetime.now().isoformat()
        )

        # Load and verify
        registry = adapter_registry.load_registry(registry_path)
        assert "test-adapter-v1" in registry["adapters"]
        assert "eval" in registry["adapters"]["test-adapter-v1"]
        assert len(registry["adapters"]["test-adapter-v1"]["eval"]) == 1

        eval_record = registry["adapters"]["test-adapter-v1"]["eval"][0]
        assert eval_record["eval_name"] == "fidelity-nightly"
        assert eval_record["score"] == 0.78
        assert eval_record["verdict"] == "PASS"


def test_record_and_load_promotion():
    """Test recording and loading promotion records."""
    with tempfile.TemporaryDirectory() as tmpdir:
        registry_path = Path(tmpdir) / "registry.json"

        # Record a promotion
        adapter_registry.record_promotion(
            registry_path=registry_path,
            adapter_id="test-adapter-v1",
            evidence="dpo-queue-gate-pass",
            timestamp=datetime.now().isoformat()
        )

        # Load and verify
        registry = adapter_registry.load_registry(registry_path)
        assert "test-adapter-v1" in registry["adapters"]
        assert "promotion" in registry["adapters"]["test-adapter-v1"]

        promotion = registry["adapters"]["test-adapter-v1"]["promotion"]
        assert promotion["evidence"] == "dpo-queue-gate-pass"


def test_record_demotion():
    """Test recording demotion."""
    with tempfile.TemporaryDirectory() as tmpdir:
        registry_path = Path(tmpdir) / "registry.json"

        # Record a demotion
        adapter_registry.record_demotion(
            registry_path=registry_path,
            adapter_id="test-adapter-v1",
            reason="eval-regression",
            timestamp=datetime.now().isoformat()
        )

        # Load and verify
        registry = adapter_registry.load_registry(registry_path)
        assert "test-adapter-v1" in registry["adapters"]

        demotion = registry["adapters"]["test-adapter-v1"]["demotion"]
        assert demotion["reason"] == "eval-regression"


def test_atomicity_rename():
    """Test that writes are atomic (tmp+rename)."""
    with tempfile.TemporaryDirectory() as tmpdir:
        registry_path = Path(tmpdir) / "registry.json"

        # Write first record
        adapter_registry.record_training(
            registry_path=registry_path,
            adapter_id="adapter-1",
            metrics={"n_pairs": 100}
        )

        # Verify file exists
        assert registry_path.exists()
        original_content = registry_path.read_text()

        # Write second record (should use atomic rename)
        adapter_registry.record_eval(
            registry_path=registry_path,
            adapter_id="adapter-1",
            eval_name="test",
            score=0.5,
            verdict="PASS"
        )

        # File should exist and contain both records
        assert registry_path.exists()
        registry = adapter_registry.load_registry(registry_path)
        assert len(registry["adapters"]["adapter-1"]["training"]) == 1
        assert len(registry["adapters"]["adapter-1"]["eval"]) == 1


def test_status_flags_stale_eval():
    """Test that status() flags adapters with stale evals."""
    with tempfile.TemporaryDirectory() as tmpdir:
        registry_path = Path(tmpdir) / "registry.json"

        # Record an eval from 8 days ago
        old_time = (datetime.now() - timedelta(days=8)).isoformat()
        adapter_registry.record_eval(
            registry_path=registry_path,
            adapter_id="stale-adapter",
            eval_name="fidelity-nightly",
            score=0.50,
            verdict="PASS",
            timestamp=old_time
        )

        # Check status — should flag as stale
        status_output = adapter_registry.status(
            registry_path=registry_path,
            live_adapter_id=None
        )
        assert "stale" in status_output.lower() or "8 days" in status_output or "7 days" in status_output


def test_status_flags_never_promoted():
    """Test that status() flags live adapters with no promotion record."""
    with tempfile.TemporaryDirectory() as tmpdir:
        registry_path = Path(tmpdir) / "registry.json"

        # Record training but no promotion
        adapter_registry.record_training(
            registry_path=registry_path,
            adapter_id="live-adapter",
            metrics={"n_pairs": 100}
        )

        # Check status for this as the live adapter
        status_output = adapter_registry.status(
            registry_path=registry_path,
            live_adapter_id="live-adapter"
        )
        # Should warn that the live adapter has no promotion record
        assert "live adapter with no promotion record" in status_output.lower() or "warning" in status_output.lower()


def test_status_shows_promotion_evidence():
    """Test that status() shows promotion evidence when present."""
    with tempfile.TemporaryDirectory() as tmpdir:
        registry_path = Path(tmpdir) / "registry.json"

        # Record promotion with evidence
        adapter_registry.record_promotion(
            registry_path=registry_path,
            adapter_id="good-adapter",
            evidence="blind-a-b-gate-pass",
            timestamp=datetime.now().isoformat()
        )

        # Check status
        status_output = adapter_registry.status(
            registry_path=registry_path,
            live_adapter_id="good-adapter"
        )
        # Should show the promotion evidence
        assert "blind-a-b-gate-pass" in status_output or "promotion" in status_output.lower()


def test_schema_version():
    """Test that registry includes a schema_version."""
    with tempfile.TemporaryDirectory() as tmpdir:
        registry_path = Path(tmpdir) / "registry.json"

        adapter_registry.record_training(
            registry_path=registry_path,
            adapter_id="test",
            metrics={"n_pairs": 1}
        )

        registry = adapter_registry.load_registry(registry_path)
        assert "schema_version" in registry
        assert registry["schema_version"] >= 1


def main():
    """Run all tests."""
    tests = [
        test_record_and_load_training,
        test_record_and_load_eval,
        test_record_and_load_promotion,
        test_record_demotion,
        test_atomicity_rename,
        test_status_flags_stale_eval,
        test_status_flags_never_promoted,
        test_status_shows_promotion_evidence,
        test_schema_version,
    ]

    passed = 0
    failed = 0

    for test in tests:
        try:
            test()
            print(f"PASS {test.__name__}")
            passed += 1
        except AssertionError as e:
            print(f"FAIL {test.__name__}: {e}")
            failed += 1
        except Exception as e:
            print(f"ERROR {test.__name__}: {e}")
            failed += 1

    print(f"\n{passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())


# ---------------------------------------------------------------------------
# Lifecycle status: active (default) / retired / rejected
# ---------------------------------------------------------------------------


def test_record_retirement_writes_status_fields():
    """record_retirement() stamps a top-level status + reason + timestamp so an
    enumerating reader can skip the entry without parsing free-text metrics."""
    with tempfile.TemporaryDirectory() as tmpdir:
        registry_path = Path(tmpdir) / "registry.json"
        adapter_registry.record_training(
            registry_path=registry_path, adapter_id="noop", metrics={"n_pairs": 1})

        adapter_registry.record_retirement(
            registry_path=registry_path, adapter_id="noop",
            status="rejected", reason="lora_b 0/80 -- adapter == base",
            timestamp="2026-09-05T12:00:00")

        entry = json.loads(registry_path.read_text())["adapters"]["noop"]
        assert entry["status"] == "rejected"
        assert entry["retired_reason"] == "lora_b 0/80 -- adapter == base"
        assert entry["retired_at"] == "2026-09-05T12:00:00"
        # The training history is untouched -- retirement is additive metadata.
        assert len(entry["training"]) == 1


def test_record_retirement_rejects_unknown_status_and_unknown_adapter():
    with tempfile.TemporaryDirectory() as tmpdir:
        registry_path = Path(tmpdir) / "registry.json"
        adapter_registry.record_training(
            registry_path=registry_path, adapter_id="a", metrics={})
        try:
            adapter_registry.record_retirement(
                registry_path=registry_path, adapter_id="a", status="deleted", reason="x")
            assert False, "expected ValueError for status=deleted"
        except ValueError:
            pass
        try:
            adapter_registry.record_retirement(
                registry_path=registry_path, adapter_id="ghost", status="retired", reason="x")
            assert False, "expected KeyError for an adapter not in the registry"
        except KeyError:
            pass
        # Neither refusal created a phantom entry.
        assert set(json.loads(registry_path.read_text())["adapters"]) == {"a"}


def test_adapter_status_defaults_to_active_when_absent():
    assert adapter_registry.adapter_status({"created": "x", "training": []}) == "active"
    assert adapter_registry.adapter_status({"status": "retired"}) == "retired"
    assert adapter_registry.is_active({"status": "rejected"}) is False
    assert adapter_registry.is_active({}) is True


def test_iter_adapters_skips_retired_and_rejected_by_default():
    registry = {"adapters": {
        "live": {"created": "x"},
        "old": {"created": "x", "status": "retired"},
        "noop": {"created": "x", "status": "rejected"},
    }}
    assert [k for k, _ in adapter_registry.iter_adapters(registry)] == ["live"]
    assert sorted(k for k, _ in adapter_registry.iter_adapters(
        registry, include_retired=True)) == ["live", "noop", "old"]


def test_status_report_hides_retired_unless_asked():
    """The status report is the one in-repo reader that enumerates the registry
    (eval_fidelity_nightly / m3_promote only WRITE rows). It must not list a
    retired adapter as if it were a candidate, but must say how many it hid."""
    with tempfile.TemporaryDirectory() as tmpdir:
        registry_path = Path(tmpdir) / "registry.json"
        adapter_registry.record_training(
            registry_path=registry_path, adapter_id="keep-me", metrics={})
        adapter_registry.record_training(
            registry_path=registry_path, adapter_id="dead-noop", metrics={})
        adapter_registry.record_retirement(
            registry_path=registry_path, adapter_id="dead-noop",
            status="rejected", reason="all lora_b zero")

        out = adapter_registry.status(registry_path=registry_path, live_adapter_id=None)
        assert "Adapter: keep-me" in out
        assert "Adapter: dead-noop" not in out
        assert "1 retired/rejected adapter hidden" in out

        out_all = adapter_registry.status(
            registry_path=registry_path, live_adapter_id=None, include_retired=True)
        assert "Adapter: dead-noop [REJECTED]" in out_all
        assert "all lora_b zero" in out_all


def test_status_report_still_warns_when_the_live_adapter_is_retired():
    """Retiring the served adapter is refused by the writer, but if the registry
    ever says the LIVE adapter is retired, hiding it would hide the problem."""
    with tempfile.TemporaryDirectory() as tmpdir:
        registry_path = Path(tmpdir) / "registry.json"
        adapter_registry.record_training(
            registry_path=registry_path, adapter_id="serving", metrics={})
        adapter_registry.record_retirement(
            registry_path=registry_path, adapter_id="serving",
            status="retired", reason="superseded")
        out = adapter_registry.status(registry_path=registry_path, live_adapter_id="serving")
        assert "LIVE" in out and "retired" in out.lower()
        assert "Adapter: serving" in out
