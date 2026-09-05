#!/usr/bin/env python3
# scripts/test_register_v6_adapter.py
#
# Follow-up to contract C6: scripts/register_v6_adapter.py had no case for an
# adapter produced by scripts/mlx_tune_train.py (trainer="mlx_tune"). This
# suite covers: trainer/train_mode/beta/gamma provenance is recorded
# correctly for mlx_tune logs, the mlx_tune train-loss series is parsed from
# its "Step N/M | Loss: X" lines, and the pre-existing scale==2.0 /
# lora_b-non-zero refusals still fire for mlx_tune-produced adapters too.
#
# NO model is loaded (adapters.safetensors fixtures use a real but tiny
# mlx.core.save_safetensors call -- KB-sized, not the 56 GB base) and NO
# production file is ever touched: every test monkeypatches
# register_v6_adapter.record_training to a recording stub instead of calling
# the real adapter_registry.record_training (whose default registry_path is
# the live ~/.human/training-data/adapters/registry.json).
#
# Run: ~/.human/venvs/mlxtune312/bin/python -m pytest scripts/test_register_v6_adapter.py -v
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
import register_v6_adapter as reg


# --------------------------------------------------------------------------
# Fixtures
# --------------------------------------------------------------------------


def _write_manifest(tmp_path: Path) -> Path:
    manifest = {
        "counts": {"total": 42},
        "by_source": {"imessage": 30, "synthetic": 12},
        "targets": ["over-elaboration", "register-mismatch"],
    }
    p = tmp_path / "manifest.json"
    p.write_text(json.dumps(manifest))
    return p


def _write_adapter(tmp_path: Path, scale=2.0, rank=8, dropout=0.0, num_layers=8, b_values=(0.01, 0.02, 0.03)):
    """A real (tiny) mlx-tune/mlx_lm-shaped adapter dir: adapter_config.json
    + adapters.safetensors with actual lora_b/lora_a tensors via
    mlx.core.save_safetensors -- KB-sized, not a loaded model."""
    import mlx.core as mx

    adapter_dir = tmp_path / "adapter"
    adapter_dir.mkdir()
    cfg = {
        "fine_tune_type": "lora",
        "num_layers": num_layers,
        "lora_parameters": {"rank": rank, "scale": scale, "dropout": dropout},
    }
    (adapter_dir / "adapter_config.json").write_text(json.dumps(cfg))

    tensors = {}
    for i, val in enumerate(b_values):
        tensors[f"layers.{i}.mlp.lora_b"] = mx.full((2, 2), val)
        tensors[f"layers.{i}.mlp.lora_a"] = mx.full((2, 2), 0.01)
    mx.save_safetensors(str(adapter_dir / "adapters.safetensors"), tensors)
    return adapter_dir


MLX_TUNE_ORPO_LOG = """\
[train] trainer: mlx_tune --train-mode orpo --beta 0.1 (own venv: /Users/sethford/.human/venvs/mlxtune312/bin/python)
======================================================================
mlx_tune_train.py
======================================================================
[mlx_tune_train] loading base model: mlx-community/GLM-4.5-Air-4bit
ORPOTrainer initialized:
  Beta: 0.1
  Learning rate: 5e-06
  Iterations: 400
Training Mode: orpo
Starting ORPO Training
======================================================================
  Step 10/400 | Loss: 2.3145 | batch_size: 1
  Step 20/400 | Loss: 1.9871 | batch_size: 1
  Step 400/400 | Loss: 0.8123 | batch_size: 1
ORPO Training Complete!
[mlx_tune_train] lora_b non-zero 3/3, max|B| = 3.000e-02
[mlx_tune_train] confirmed: the adapter has real weight movement
[mlx_tune_train] adapter_config.json lora_parameters.scale = 2.0
[mlx_tune_train] DONE. adapter=/Users/sethford/.human/training-data/adapters/seth-glm-air-v63-orpo-20260902-070000
"""

MLX_TUNE_SIMPO_LOG = """\
[mlx_tune_train] loading base model: mlx-community/GLM-4.5-Air-4bit
SimPOTrainer initialized:
  Beta: 2.0, Gamma: 0.5
  Learning rate: 5e-06
  Iterations: 400
Training Mode: simpo
  Step 10/400 | Loss: 1.5000 | batch_size: 1
  Step 400/400 | Loss: 0.4000 | batch_size: 1
[mlx_tune_train] lora_b non-zero 3/3, max|B| = 3.000e-02
"""

MLX_LM_LORA_ORPO_LOG = """\
[train] trainer: mlx_lm_lora --train-mode orpo --beta 0.15
Training Mode: ORPO
Iter 10: loss 2.100, it/sec 1.2, tokens/sec 300, acc 0.400, margin -0.050
Iter 400: loss 0.900, it/sec 1.1, tokens/sec 290, acc 0.700, margin 0.120
Iter 25: Val loss 1.800 Val accuracy 0.410 Val margin -0.040
"""

MLX_LM_SFT_LOG = """\
Iter 10: Train loss 5.029
Iter 500: Train loss 1.923
Iter 25: Val loss 4.900
"""


class _RecordSink:
    """Stand-in for adapter_registry.record_training -- captures the call
    instead of writing the real (production) registry.json."""

    def __init__(self):
        self.calls = []

    def __call__(self, **kwargs):
        self.calls.append(kwargs)


@pytest.fixture(autouse=True)
def no_real_registry(monkeypatch):
    """Refuse to let ANY test in this file touch the live registry file --
    every call to record_training is captured, never executed for real."""
    sink = _RecordSink()
    monkeypatch.setattr(reg, "record_training", sink)
    return sink


def _run_main(monkeypatch, tmp_path, log_text, adapter_dir=None, smoke=None):
    tmp_path.mkdir(parents=True, exist_ok=True)
    adapter_dir = adapter_dir or _write_adapter(tmp_path)
    log_path = tmp_path / "train.log"
    log_path.write_text(log_text)
    manifest_path = _write_manifest(tmp_path)

    argv = [
        "register_v6_adapter.py",
        "--adapter", str(adapter_dir),
        "--log", str(log_path),
        "--corpus-manifest", str(manifest_path),
    ]
    if smoke:
        argv += ["--smoke", str(smoke)]
    monkeypatch.setattr(sys, "argv", argv)
    reg.main()


# --------------------------------------------------------------------------
# trainer / train_mode / beta / gamma provenance
# --------------------------------------------------------------------------


def test_mlx_tune_orpo_log_records_trainer_and_train_mode(monkeypatch, tmp_path, no_real_registry):
    _run_main(monkeypatch, tmp_path, MLX_TUNE_ORPO_LOG)
    assert len(no_real_registry.calls) == 1
    metrics = no_real_registry.calls[0]["metrics"]
    assert metrics["trainer"] == "mlx_tune"
    assert metrics["train_mode"] == "orpo"
    assert metrics["objective"] == "orpo"
    assert metrics["beta"] == pytest.approx(0.1)
    assert metrics["gamma"] is None


def test_mlx_tune_simpo_log_records_beta_and_gamma(monkeypatch, tmp_path, no_real_registry):
    _run_main(monkeypatch, tmp_path, MLX_TUNE_SIMPO_LOG)
    metrics = no_real_registry.calls[0]["metrics"]
    assert metrics["trainer"] == "mlx_tune"
    assert metrics["train_mode"] == "simpo"
    assert metrics["beta"] == pytest.approx(2.0)
    assert metrics["gamma"] == pytest.approx(0.5)


def test_mlx_tune_orpo_is_distinguished_from_mlx_lm_lora_orpo(monkeypatch, tmp_path, no_real_registry):
    """The critical disambiguation: both trainers can print 'Training Mode:
    orpo' -- only the [mlx_tune_train] marker tells them apart. Regression
    guard for the exact bug this follow-up exists to fix."""
    _run_main(monkeypatch, tmp_path / "a", MLX_TUNE_ORPO_LOG)
    mlx_tune_metrics = no_real_registry.calls[-1]["metrics"]

    _run_main(monkeypatch, tmp_path / "b", MLX_LM_LORA_ORPO_LOG)
    mlx_lm_lora_metrics = no_real_registry.calls[-1]["metrics"]

    assert mlx_tune_metrics["objective"] == mlx_lm_lora_metrics["objective"] == "orpo"
    assert mlx_tune_metrics["trainer"] == "mlx_tune"
    assert mlx_lm_lora_metrics["trainer"] == "mlx_lm_lora"
    assert mlx_tune_metrics["train_mode"] == "orpo"
    assert mlx_lm_lora_metrics["train_mode"] is None  # only set for mlx_tune


def test_mlx_lm_lora_log_unaffected_by_the_new_mlx_tune_handling(monkeypatch, tmp_path, no_real_registry):
    _run_main(monkeypatch, tmp_path, MLX_LM_LORA_ORPO_LOG)
    metrics = no_real_registry.calls[0]["metrics"]
    assert metrics["trainer"] == "mlx_lm_lora"
    assert metrics["train_mode"] is None
    assert metrics["beta"] == pytest.approx(0.05)  # unchanged hardcoded default
    assert metrics["train_loss_first"] == pytest.approx(2.100)
    assert metrics["train_loss_last"] == pytest.approx(0.900)


def test_mlx_lm_sft_log_unaffected_by_the_new_mlx_tune_handling(monkeypatch, tmp_path, no_real_registry):
    _run_main(monkeypatch, tmp_path, MLX_LM_SFT_LOG)
    metrics = no_real_registry.calls[0]["metrics"]
    assert metrics["trainer"] == "mlx_lm"
    assert metrics["objective"] == "sft"
    assert metrics["train_mode"] is None
    assert metrics["beta"] is None


# --------------------------------------------------------------------------
# mlx-tune train-loss series parsing ("Step N/M | Loss: X")
# --------------------------------------------------------------------------


def test_mlx_tune_train_series_parsed_from_step_lines(monkeypatch, tmp_path, no_real_registry):
    _run_main(monkeypatch, tmp_path, MLX_TUNE_ORPO_LOG)
    metrics = no_real_registry.calls[0]["metrics"]
    assert metrics["train_loss_first"] == pytest.approx(2.3145)
    assert metrics["train_loss_last"] == pytest.approx(0.8123)
    assert metrics["iters"] == 400
    assert len(metrics["train_series"]) == 3


def test_mlx_tune_log_with_no_step_lines_yields_no_train_series(monkeypatch, tmp_path, no_real_registry):
    sparse_log = "[mlx_tune_train] loading base model: x\nTraining Mode: kto\n"
    _run_main(monkeypatch, tmp_path, sparse_log)
    metrics = no_real_registry.calls[0]["metrics"]
    assert metrics["trainer"] == "mlx_tune"
    assert metrics["train_mode"] == "kto"
    assert metrics["train_series"] is None
    assert metrics["train_loss_first"] is None


# --------------------------------------------------------------------------
# The scale==2.0 / lora_b-non-zero refusals still fire for mlx_tune adapters
# --------------------------------------------------------------------------


def test_mlx_tune_adapter_with_wrong_scale_is_refused(monkeypatch, tmp_path, no_real_registry):
    adapter_dir = _write_adapter(tmp_path, scale=20.0)
    with pytest.raises(SystemExit, match="expected 2.0"):
        _run_main(monkeypatch, tmp_path, MLX_TUNE_ORPO_LOG, adapter_dir=adapter_dir)
    assert no_real_registry.calls == []  # refused BEFORE any record_training call


def test_mlx_tune_adapter_with_all_zero_lora_b_is_refused(monkeypatch, tmp_path, no_real_registry):
    adapter_dir = _write_adapter(tmp_path, b_values=(0.0, 0.0, 0.0))
    with pytest.raises(SystemExit, match="no-op"):
        _run_main(monkeypatch, tmp_path, MLX_TUNE_ORPO_LOG, adapter_dir=adapter_dir)
    assert no_real_registry.calls == []


def test_mlx_tune_adapter_with_healthy_weights_registers_cleanly(monkeypatch, tmp_path, no_real_registry):
    adapter_dir = _write_adapter(tmp_path, b_values=(0.01, 0.02, 0.03))
    _run_main(monkeypatch, tmp_path, MLX_TUNE_ORPO_LOG, adapter_dir=adapter_dir)
    metrics = no_real_registry.calls[0]["metrics"]
    assert metrics["weights"]["lora_b_nonzero"] == "3/3"
    assert metrics["lora_scale"] == 2.0


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))


# --------------------------------------------------------------------------
# --retire: first-class lifecycle status on registry entries
# --------------------------------------------------------------------------


def _seed_registry(tmp_path: Path, entries: dict) -> Path:
    """A throwaway registry.json in the exact production shape."""
    p = tmp_path / "registry.json"
    p.write_text(json.dumps({"schema_version": 1, "timestamp": "t", "adapters": entries}))
    return p


def _seed_config(tmp_path: Path, served: str) -> Path:
    p = tmp_path / "config.json"
    p.write_text(json.dumps({"personalization": {
        "lora_adapter_path": f"/x/adapters/{served}",
        "lora_adapter_id": served}}))
    return p


def _run_retire(monkeypatch, registry, config, name, reason="lora_b 0/80", extra=()):
    argv = ["register_v6_adapter.py", "--retire", name, "--reason", reason,
            "--registry", str(registry), "--config", str(config), *extra]
    monkeypatch.setattr(sys, "argv", argv)
    reg.main()


def test_retire_marks_entry_and_leaves_history_intact(monkeypatch, tmp_path):
    registry = _seed_registry(tmp_path, {
        "noop": {"created": "c", "training": [{"timestamp": "t", "metrics": {"promoted": False}}], "eval": []},
    })
    config = _seed_config(tmp_path, served="live")
    _run_retire(monkeypatch, registry, config, "noop", reason="every lora_b is 0.0")

    entry = json.loads(registry.read_text())["adapters"]["noop"]
    assert entry["status"] == "retired"
    assert entry["retired_reason"] == "every lora_b is 0.0"
    assert entry["retired_at"]
    assert entry["training"][0]["metrics"] == {"promoted": False}


def test_retire_as_rejected(monkeypatch, tmp_path):
    registry = _seed_registry(tmp_path, {"noop": {"created": "c", "training": [], "eval": []}})
    config = _seed_config(tmp_path, served="live")
    _run_retire(monkeypatch, registry, config, "noop", extra=["--status", "rejected"])
    assert json.loads(registry.read_text())["adapters"]["noop"]["status"] == "rejected"


def test_retire_refuses_the_served_adapter_even_with_force(monkeypatch, tmp_path):
    """The served adapter is what :8741 answers with. Retiring its registry row
    would make every reader hide the thing that is actually in production."""
    registry = _seed_registry(tmp_path, {"live": {"created": "c", "training": [], "eval": []}})
    config = _seed_config(tmp_path, served="live")
    with pytest.raises(SystemExit, match="served"):
        _run_retire(monkeypatch, registry, config, "live", extra=["--force"])
    assert "status" not in json.loads(registry.read_text())["adapters"]["live"]


def test_retire_refuses_a_promoted_adapter_without_force(monkeypatch, tmp_path):
    registry = _seed_registry(tmp_path, {
        "was-good": {"created": "c", "eval": [],
                     "training": [{"timestamp": "t", "metrics": {"promoted": True}}]},
    })
    config = _seed_config(tmp_path, served="live")
    with pytest.raises(SystemExit, match="promoted"):
        _run_retire(monkeypatch, registry, config, "was-good")
    assert "status" not in json.loads(registry.read_text())["adapters"]["was-good"]


def test_retire_treats_a_promotion_record_as_promoted(monkeypatch, tmp_path):
    """m3_promote.py writes a top-level `promotion` block, not metrics.promoted --
    both spellings mean the adapter once served and need --force."""
    registry = _seed_registry(tmp_path, {
        "was-good": {"created": "c", "training": [], "eval": [],
                     "promotion": {"timestamp": "t", "evidence": "blind-a-b-gate-pass"}},
    })
    config = _seed_config(tmp_path, served="live")
    with pytest.raises(SystemExit, match="promoted"):
        _run_retire(monkeypatch, registry, config, "was-good")


def test_retire_promoted_adapter_with_force(monkeypatch, tmp_path):
    registry = _seed_registry(tmp_path, {
        "was-good": {"created": "c", "eval": [],
                     "training": [{"timestamp": "t", "metrics": {"promoted": True}}]},
    })
    config = _seed_config(tmp_path, served="live")
    _run_retire(monkeypatch, registry, config, "was-good", reason="superseded by v7", extra=["--force"])
    assert json.loads(registry.read_text())["adapters"]["was-good"]["status"] == "retired"


def test_retire_refuses_unknown_adapter(monkeypatch, tmp_path):
    registry = _seed_registry(tmp_path, {"a": {"created": "c", "training": [], "eval": []}})
    config = _seed_config(tmp_path, served="live")
    with pytest.raises(SystemExit, match="not in registry"):
        _run_retire(monkeypatch, registry, config, "ghost")
    assert set(json.loads(registry.read_text())["adapters"]) == {"a"}


def test_retire_requires_a_reason(monkeypatch, tmp_path):
    registry = _seed_registry(tmp_path, {"a": {"created": "c", "training": [], "eval": []}})
    config = _seed_config(tmp_path, served="live")
    monkeypatch.setattr(sys, "argv", ["register_v6_adapter.py", "--retire", "a",
                                      "--registry", str(registry), "--config", str(config)])
    with pytest.raises(SystemExit):
        reg.main()
    assert "status" not in json.loads(registry.read_text())["adapters"]["a"]


# --------------------------------------------------------------------------
# US-2: authorship_gate annotation (never blocking -- see
# _read_authorship_gate_for's docstring). Monkeypatches the gate functions
# reg imported from authorship_promotion_gate directly, the same pattern
# no_real_registry uses for record_training -- hermetic, no real
# ~/.human/logs glob involved.
# --------------------------------------------------------------------------


def test_register_v6_adapter_annotates_absent(monkeypatch, tmp_path, no_real_registry):
    """No matching candidate-authorship-*.json on disk -- annotation is
    {"status": "NOT_RUN"} and registration still succeeds (annotation is
    informational, never blocking)."""
    monkeypatch.setattr(reg, "_find_latest_score_json", lambda adapter_path: None)
    _run_main(monkeypatch, tmp_path, MLX_LM_LORA_ORPO_LOG)
    assert len(no_real_registry.calls) == 1
    metrics = no_real_registry.calls[0]["metrics"]
    assert metrics["authorship_gate"] == {"status": "NOT_RUN"}
    assert metrics["promoted"] is False


def test_register_v6_adapter_annotates_block(monkeypatch, tmp_path, no_real_registry):
    """A matching regressed-shaped fixture is present -- the BLOCK verdict is
    recorded AND registration still succeeds with promoted: False unchanged
    (proves annotation never flips this script's own promoted flag, which
    was already always False -- guards against a future edit accidentally
    wiring this into a block)."""
    monkeypatch.setattr(reg, "_find_latest_score_json", lambda adapter_path: "/fake/candidate-authorship-2026-09-05.json")
    monkeypatch.setattr(
        reg, "load_gate_inputs_from_score_json",
        lambda path: {"candidate_twin": 0.625, "serving_twin": 0.70, "floor": 0.62})
    _run_main(monkeypatch, tmp_path, MLX_LM_LORA_ORPO_LOG)
    assert len(no_real_registry.calls) == 1
    metrics = no_real_registry.calls[0]["metrics"]
    assert metrics["authorship_gate"]["status"] == "RAN"
    assert metrics["authorship_gate"]["verdict"] == "BLOCK"
    assert metrics["authorship_gate"]["reason"] == "regression_vs_prior"
    # Never flips this script's own promoted flag.
    assert metrics["promoted"] is False


def test_register_v6_adapter_annotates_inconclusive_on_bad_measurement(monkeypatch, tmp_path, no_real_registry):
    """A matching score JSON exists but the loader refuses (malformed/missing
    measurement) -- annotation is INCONCLUSIVE, not a fabricated verdict,
    and registration still succeeds."""
    monkeypatch.setattr(reg, "_find_latest_score_json", lambda adapter_path: "/fake/candidate-authorship-2026-09-05.json")

    def _raise(path):
        raise SystemExit("INCONCLUSIVE: missing twin_candidate; nothing to gate on")

    monkeypatch.setattr(reg, "load_gate_inputs_from_score_json", _raise)
    _run_main(monkeypatch, tmp_path, MLX_LM_LORA_ORPO_LOG)
    metrics = no_real_registry.calls[0]["metrics"]
    assert metrics["authorship_gate"]["status"] == "INCONCLUSIVE"
    assert "INCONCLUSIVE" in metrics["authorship_gate"]["reason"]
    assert metrics["promoted"] is False
