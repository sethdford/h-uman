#!/usr/bin/env python3
# scripts/test_mlx_tune_train.py
#
# Contract C6 test suite for scripts/mlx_tune_train.py. Runs entirely inside
# ~/.human/venvs/mlxtune312 (pytest added there test-only -- see
# scripts/mlx_tune_env.txt). NO model weights are loaded by this suite --
# every test either exercises pure config/data-translation logic, or mocks
# the trainer/model boundary exactly as the task requires ("test the config
# translation, the dry-run validation paths, and the adapter_config.json
# writer ... with the trainer mocked").
#
# Run: ~/.human/venvs/mlxtune312/bin/python -m pytest scripts/test_mlx_tune_train.py -v
import json
import sys
from pathlib import Path
from unittest import mock

import pytest

sys.path.insert(0, str(Path(__file__).parent))
import mlx_tune_train as mt


# --------------------------------------------------------------------------
# Config translation
# --------------------------------------------------------------------------


def test_require_lora_parameters_accepts_scale_2():
    lp = mt.require_lora_parameters(
        {"lora_parameters": {"rank": 8, "scale": 2.0, "dropout": 0.0}}
    )
    assert lp == {"rank": 8, "scale": 2.0, "dropout": 0.0}


def test_require_lora_parameters_rejects_non_2_scale():
    """The catastrophic-default class this whole contract exists to avoid --
    mlx_lm defaults to 20.0, mlx-lm-lora to 10.0 (see
    .claude/rules/lora-scale-default-or-die.md). Any scale other than the
    explicit 2.0 this repo's adapters all use must be refused, not silently
    accepted."""
    with pytest.raises(ValueError, match="expected exactly 2.0"):
        mt.require_lora_parameters(
            {"lora_parameters": {"rank": 8, "scale": 20.0, "dropout": 0.0}}
        )


def test_require_lora_parameters_rejects_missing_block():
    with pytest.raises(ValueError, match="lora_parameters"):
        mt.require_lora_parameters({"scale": 2.0})  # flat key -- silently ignored


def test_require_lora_parameters_rejects_missing_key():
    with pytest.raises(ValueError, match="dropout"):
        mt.require_lora_parameters({"lora_parameters": {"rank": 8, "scale": 2.0}})


@pytest.mark.parametrize(
    "rank,scale,expected_alpha",
    [(8, 2.0, 16.0), (16, 2.0, 32.0), (4, 2.0, 8.0)],
)
def test_lora_alpha_from_scale_inverts_mlx_tune_formula(rank, scale, expected_alpha):
    """mlx-tune computes scale = lora_alpha / rank (mlx_tune/model.py
    MLXModelWrapper._apply_lora). This must be the exact inverse, or the
    adapter_config.json mlx-tune writes will NOT show scale=2.0."""
    alpha = mt.lora_alpha_from_scale(rank, scale)
    assert alpha == expected_alpha
    assert alpha / rank == scale  # round-trips through mlx-tune's own formula


def test_load_yaml_config_reads_real_shaped_config(tmp_path):
    cfg_path = tmp_path / "cfg.yaml"
    cfg_path.write_text(
        "model: mlx-community/GLM-4.5-Air-4bit\n"
        "data: /tmp/some-data-dir\n"
        "lora_parameters:\n"
        "  rank: 8\n"
        "  scale: 2.0\n"
        "  dropout: 0.0\n"
    )
    cfg = mt.load_yaml_config(str(cfg_path))
    assert cfg["model"] == "mlx-community/GLM-4.5-Air-4bit"
    assert cfg["lora_parameters"]["scale"] == 2.0


def test_load_yaml_config_missing_file_raises():
    with pytest.raises(FileNotFoundError):
        mt.load_yaml_config("/nonexistent/path/does-not-exist.yaml")


# --------------------------------------------------------------------------
# Data validation
# --------------------------------------------------------------------------


def _write_jsonl(path: Path, rows):
    with open(path, "w") as f:
        for row in rows:
            f.write(json.dumps(row) + "\n")


def test_validate_data_dir_happy_path(tmp_path):
    good_pairs = [{"prompt": "hi", "chosen": "yo", "rejected": "Certainly!"}] * 5
    _write_jsonl(tmp_path / "train.jsonl", good_pairs)
    _write_jsonl(tmp_path / "valid.jsonl", good_pairs[:2])

    report = mt.validate_data_dir(str(tmp_path))
    assert report["ok"] is True
    assert report["train"]["count"] == 5
    assert report["valid"]["count"] == 2
    assert report["train"]["bad_line_total"] == 0


def test_validate_data_dir_flags_missing_keys(tmp_path):
    _write_jsonl(
        tmp_path / "train.jsonl",
        [
            {"prompt": "hi", "chosen": "yo", "rejected": "Certainly!"},
            {"prompt": "hi", "chosen": "yo"},  # missing "rejected"
        ],
    )
    report = mt.validate_data_dir(str(tmp_path))
    assert report["ok"] is False
    assert report["train"]["bad_line_total"] == 1
    assert "rejected" in report["train"]["bad_lines"][0][1]


def test_validate_data_dir_flags_empty_train(tmp_path):
    (tmp_path / "train.jsonl").write_text("")
    report = mt.validate_data_dir(str(tmp_path))
    assert report["ok"] is False
    assert report["train"]["count"] == 0


def test_validate_data_dir_flags_missing_file(tmp_path):
    report = mt.validate_data_dir(str(tmp_path))
    assert report["ok"] is False
    assert report["train"]["exists"] is False


# --------------------------------------------------------------------------
# KTO dataset-shape translation
# --------------------------------------------------------------------------


def test_to_kto_examples_translates_pairs_to_trl_format():
    """mlx-tune's KTOTrainer.train() only recognizes {prompt, completion,
    label} (TRL) or {text, label} (legacy) -- passing {prompt, chosen,
    rejected} straight through hits its `continue` branch for every sample.
    This is the translation that makes --train-mode kto actually train on
    our corpus instead of silently training on nothing."""
    pairs = [
        {"prompt": "hi", "chosen": "yo whats up", "rejected": "Certainly! How may I help?"},
    ]
    examples = mt.to_kto_examples(pairs)
    assert examples == [
        {"prompt": "hi", "completion": "yo whats up", "label": True},
        {"prompt": "hi", "completion": "Certainly! How may I help?", "label": False},
    ]


def test_to_kto_examples_doubles_count_and_preserves_prompts():
    pairs = [
        {"prompt": f"p{i}", "chosen": f"c{i}", "rejected": f"r{i}"} for i in range(7)
    ]
    examples = mt.to_kto_examples(pairs)
    assert len(examples) == 14
    prompts = {e["prompt"] for e in examples}
    assert prompts == {f"p{i}" for i in range(7)}
    labels = [e["label"] for e in examples]
    assert labels.count(True) == 7
    assert labels.count(False) == 7


# --------------------------------------------------------------------------
# adapter_config.json writer -- trainer/model mocked (no weights involved)
# --------------------------------------------------------------------------


def test_write_or_verify_adapter_config_creates_when_missing(tmp_path):
    result = mt.write_or_verify_adapter_config(tmp_path, rank=8, scale=2.0, dropout=0.0, num_layers=8)
    assert result["lora_parameters"] == {"rank": 8, "scale": 2.0, "dropout": 0.0}
    on_disk = json.loads((tmp_path / "adapter_config.json").read_text())
    assert on_disk["lora_parameters"]["scale"] == 2.0
    assert on_disk["num_layers"] == 8


def test_write_or_verify_adapter_config_rewrites_wrong_scale(tmp_path):
    """Simulates mlx-tune's own _save_adapters_and_config() having written a
    file with the WRONG scale (e.g. a future mlx-tune version changes its
    alpha/r convention, or a caller passed the wrong lora_alpha) -- this
    function must correct it, not trust it."""
    (tmp_path / "adapter_config.json").write_text(
        json.dumps({"fine_tune_type": "lora", "lora_parameters": {"rank": 8, "scale": 20.0, "dropout": 0.0}})
    )
    result = mt.write_or_verify_adapter_config(tmp_path, rank=8, scale=2.0, dropout=0.0, num_layers=8)
    assert result["lora_parameters"]["scale"] == 2.0


def test_write_or_verify_adapter_config_preserves_resolved_keys(tmp_path):
    """mlx-tune's own writer stores the resolved per-expert-MoE target-module
    paths under lora_parameters.keys -- a rewrite must not drop them."""
    (tmp_path / "adapter_config.json").write_text(
        json.dumps(
            {
                "fine_tune_type": "lora",
                "lora_parameters": {
                    "rank": 8,
                    "scale": 20.0,  # wrong -- forces a rewrite
                    "dropout": 0.0,
                    "keys": ["mlp.switch_mlp.gate_proj", "mlp.shared_experts.gate_proj"],
                },
            }
        )
    )
    result = mt.write_or_verify_adapter_config(tmp_path, rank=8, scale=2.0, dropout=0.0, num_layers=8)
    assert result["lora_parameters"]["scale"] == 2.0
    assert result["lora_parameters"]["keys"] == [
        "mlp.switch_mlp.gate_proj",
        "mlp.shared_experts.gate_proj",
    ]


def test_write_or_verify_adapter_config_noop_when_already_correct(tmp_path):
    cfg = {"fine_tune_type": "lora", "lora_parameters": {"rank": 8, "scale": 2.0, "dropout": 0.0}}
    cfg_path = tmp_path / "adapter_config.json"
    cfg_path.write_text(json.dumps(cfg))
    mtime_before = cfg_path.stat().st_mtime_ns
    mt.write_or_verify_adapter_config(tmp_path, rank=8, scale=2.0, dropout=0.0, num_layers=8)
    assert cfg_path.stat().st_mtime_ns == mtime_before  # untouched, no rewrite


# --------------------------------------------------------------------------
# lora_b non-zero guard -- reuses scripts/train-glm-adapter.sh's check
# --------------------------------------------------------------------------


def _save_fake_adapter(path: Path, b_values):
    """Build a minimal adapters.safetensors with the given lora_b max|values|
    (each entry becomes one layer's lora_b tensor, filled with that scalar).
    """
    mx = pytest.importorskip("mlx.core")
    tensors = {}
    for i, val in enumerate(b_values):
        tensors[f"layers.{i}.mlp.lora_b"] = mx.full((2, 2), val)
        tensors[f"layers.{i}.mlp.lora_a"] = mx.full((2, 2), 0.01)
    mx.save_safetensors(str(path / "adapters.safetensors"), tensors)


def test_assert_lora_b_nonzero_passes_when_all_nonzero(tmp_path):
    _save_fake_adapter(tmp_path, [0.01, 0.02, 0.03])
    mt.assert_lora_b_nonzero(tmp_path)  # must not raise


def test_assert_lora_b_nonzero_fails_when_all_zero(tmp_path):
    """The exact no-op signature from .claude/rules/reports-success-does-
    nothing.md and lora-scale-default-or-die.md: B zero-initialized and
    never trained means the adapter is a mathematically exact no-op."""
    _save_fake_adapter(tmp_path, [0.0, 0.0, 0.0])
    with pytest.raises(SystemExit, match="every lora_b is 0.0"):
        mt.assert_lora_b_nonzero(tmp_path)


def test_assert_lora_b_nonzero_fails_when_majority_zero(tmp_path):
    _save_fake_adapter(tmp_path, [0.01, 0.0, 0.0, 0.0, 0.0])
    with pytest.raises(SystemExit, match="partial or corrupt"):
        mt.assert_lora_b_nonzero(tmp_path)


def test_assert_lora_b_nonzero_fails_when_file_missing(tmp_path):
    with pytest.raises(SystemExit, match="no adapters.safetensors"):
        mt.assert_lora_b_nonzero(tmp_path)


# --------------------------------------------------------------------------
# dry-run validation path -- exercised end to end with mlx_lm/huggingface_hub
# mocked, so this test never touches the real HF cache or does network I/O
# --------------------------------------------------------------------------


def test_resolve_architecture_support_reports_unsupported_when_not_cached(monkeypatch):
    monkeypatch.setattr(
        "huggingface_hub.try_to_load_from_cache", lambda repo_id, filename: None
    )
    result = mt.resolve_architecture_support("some/uncached-model")
    assert result["supported"] is False
    assert result["config_source"] is None
    assert "not found in the local HF hub cache" in result["note"]


def test_resolve_architecture_support_true_for_moe_architecture(tmp_path, monkeypatch):
    """Mocks the mlx_lm boundary (per the task: 'trainer mocked') to prove
    the MoE-detection logic itself is correct, independent of whether
    GLM-4.5-Air's config happens to be cached on this machine."""
    config_path = tmp_path / "config.json"
    config_path.write_text(json.dumps({"model_type": "glm4_moe"}))
    monkeypatch.setattr(
        "huggingface_hub.try_to_load_from_cache",
        lambda repo_id, filename: str(config_path),
    )

    fake_module = mock.Mock()
    fake_module.__file__ = str(tmp_path / "fake_glm4_moe.py")
    (tmp_path / "fake_glm4_moe.py").write_text(
        "from mlx_lm.models.switch_layers import SwitchLinear, SwitchGLU\n"
    )
    fake_model_cls = mock.Mock(__module__="fake_glm4_moe", __name__="Model")

    monkeypatch.setattr(
        "mlx_lm.utils._get_classes", lambda config: (fake_model_cls, mock.Mock())
    )
    monkeypatch.setattr("importlib.import_module", lambda name: fake_module)

    result = mt.resolve_architecture_support("mlx-community/GLM-4.5-Air-4bit")
    assert result["model_type"] == "glm4_moe"
    assert result["mlx_lm_module_importable"] is True
    assert result["has_switch_linear_moe"] is True
    assert result["supported"] is True


def test_resolve_architecture_support_false_for_unsupported_model_type(tmp_path, monkeypatch):
    config_path = tmp_path / "config.json"
    config_path.write_text(json.dumps({"model_type": "totally_unknown_arch"}))
    monkeypatch.setattr(
        "huggingface_hub.try_to_load_from_cache",
        lambda repo_id, filename: str(config_path),
    )

    def _refuse(config):
        raise ValueError(f"Model type {config['model_type']} not supported.")

    monkeypatch.setattr("mlx_lm.utils._get_classes", _refuse)

    result = mt.resolve_architecture_support("some/unsupported-model")
    assert result["supported"] is False
    assert result["mlx_lm_module_importable"] is False
    assert "not supported" in result["note"]


def test_cmd_dry_run_end_to_end_with_real_glm_config(tmp_path):
    """Full --dry-run path using the REAL glm-v61-orpo-config.yaml shape
    (a local copy, not the live file, so the test is hermetic) and the
    REAL mlx_lm import machinery -- but pointed at a fake model_type so no
    real HF cache lookup or network access is required either way."""
    data_dir = tmp_path / "pref"
    data_dir.mkdir()
    _write_jsonl(
        data_dir / "train.jsonl",
        [{"prompt": "hi", "chosen": "yo", "rejected": "Certainly!"}] * 3,
    )
    _write_jsonl(
        data_dir / "valid.jsonl",
        [{"prompt": "hi", "chosen": "yo", "rejected": "Certainly!"}],
    )
    cfg_path = tmp_path / "cfg.yaml"
    cfg_path.write_text(
        f"model: some/uncached-model-id\n"
        f"data: {data_dir}\n"
        "lora_parameters:\n"
        "  rank: 8\n"
        "  scale: 2.0\n"
        "  dropout: 0.0\n"
    )
    args = mt.build_parser().parse_args(
        ["--dry-run", "--config", str(cfg_path), "--train-mode", "kto"]
    )
    # Uncached model id -> architecture support cannot be determined offline
    # -> dry-run correctly reports FAIL (ok=False), never "PASS" on a guess.
    rc = mt.cmd_dry_run(args)
    assert rc == 1


# --------------------------------------------------------------------------
# CLI surface
# --------------------------------------------------------------------------


def test_main_requires_adapter_out_when_not_dry_run(tmp_path, capsys):
    cfg_path = tmp_path / "cfg.yaml"
    cfg_path.write_text("model: x\ndata: /tmp\nlora_parameters:\n  rank: 8\n  scale: 2.0\n  dropout: 0.0\n")
    rc = mt.main(["--config", str(cfg_path), "--train-mode", "orpo"])
    assert rc == 2
    assert "--adapter-out is required" in capsys.readouterr().out


def test_main_rejects_invalid_train_mode():
    with pytest.raises(SystemExit):
        mt.build_parser().parse_args(["--config", "x.yaml", "--train-mode", "dpo"])


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
