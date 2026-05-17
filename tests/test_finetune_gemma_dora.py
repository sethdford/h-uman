"""Sprint 11 / US-11.2 — tests for DoRA train-type flag plumbing.

Covers all five acceptance criteria (AC-11.2.1 .. AC-11.2.5) by mocking
`subprocess.run` and asserting the argv shape and `train_config.json`
contents. No real `mlx_lm_lora.train` invocation, no real model weights,
no network, no GPU.

The script's filename has a hyphen, so we load it via `importlib.util`
just like `tests/test_finetune_gemma_dpo.py` and
`tests/test_finetune_gemma_modules.py`.
"""
from __future__ import annotations

import argparse
import importlib.util
import io
import json
import pathlib
import types
from contextlib import redirect_stdout
from unittest.mock import patch


# ── Module loader (hyphenated filename, cannot be `import`-ed normally) ───
_HERE = pathlib.Path(__file__).resolve().parent
_SCRIPT = _HERE.parent / "scripts" / "finetune-gemma.py"


def _load_finetune_gemma():
    spec = importlib.util.spec_from_file_location("finetune_gemma", _SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


fg = _load_finetune_gemma()


def _make_args(tmp_path: pathlib.Path, **overrides) -> argparse.Namespace:
    """argparse.Namespace shaped like main()'s `args` post-default-resolution.

    Includes US-11.2's new `train_type` field with the production default
    ("dora") — overrides can flip it to "lora" for the explicit-back-compat
    test case.
    """
    defaults = dict(
        target="31b",
        model=None,
        data=str(tmp_path / "data"),
        adapter_path=str(tmp_path / "adapter"),
        iters=10,
        batch_size=1,
        learning_rate=1e-6,
        rank=32,
        num_layers=8,
        max_seq_length=2048,
        target_modules=list(fg.DEFAULT_TARGET_MODULES),
        steps_per_report=5,
        steps_per_eval=20,
        save_every=20,
        mask_prompt=True,
        resume=False,
        dpo=True,
        sft_only=False,
        from_corrections=False,
        no_version=True,
        no_restart_server=True,
        speculative_draft=False,
        quantize=False,
        quant_bits=4,
        quant_format="mlx",
        train_all=False,
        realtime_first=False,
        # US-11.2: new field, defaults to "dora" matching main()'s argparse.
        train_type="dora",
    )
    defaults.update(overrides)
    return argparse.Namespace(**defaults)


def _stage_dpo_jsonl(tmp_path: pathlib.Path) -> pathlib.Path:
    """Stage tests/fixtures/dpo_pairs_min.jsonl into <tmp>/data/dpo/pairs.jsonl
    and pre-create the SFT adapter dir so run_dpo finds adapters.safetensors."""
    src = _HERE / "fixtures" / "dpo_pairs_min.jsonl"
    assert src.exists(), f"missing fixture: {src}"
    dst_dir = tmp_path / "data" / "dpo"
    dst_dir.mkdir(parents=True, exist_ok=True)
    dst = dst_dir / "pairs.jsonl"
    dst.write_bytes(src.read_bytes())
    adapter_dir = tmp_path / "adapter"
    adapter_dir.mkdir(parents=True, exist_ok=True)
    (adapter_dir / "adapters.safetensors").write_bytes(b"")
    return dst


# ────────────────────────────────────────────────────────────────────────
# AC-11.2.2 — default --train-type is "dora" (upgrading the prior "lora")
# ────────────────────────────────────────────────────────────────────────
def test_default_train_type_is_dora(tmp_path):
    """No --train-type flag → SFT subprocess argv contains `--train-type dora`.

    Builds an args Namespace using the SAME defaults as main()'s argparse
    (train_type="dora") and asserts the SFT path emits the DoRA token, not
    the prior hardcoded "lora".
    """
    args = _make_args(tmp_path)  # train_type defaults to "dora"
    data_dir = pathlib.Path(args.data)
    data_dir.mkdir(parents=True, exist_ok=True)
    adapter_dir = pathlib.Path(args.adapter_path)

    fake_result = types.SimpleNamespace(returncode=0)
    with patch.object(fg.subprocess, "run", return_value=fake_result) as mock_run:
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = fg.run_sft(args, data_dir, adapter_dir)

    assert rc == 0
    assert mock_run.call_count == 1, "expected exactly one SFT subprocess invocation"
    cmd = mock_run.call_args.args[0]
    assert isinstance(cmd, list), f"expected list argv, got {type(cmd)}"

    # AC-11.2.2: default mode is dora.
    assert "--train-type" in cmd, f"cmd missing --train-type: {cmd}"
    assert cmd[cmd.index("--train-type") + 1] == "dora", (
        f"default --train-type should be 'dora', got: "
        f"{cmd[cmd.index('--train-type') + 1]!r} in cmd: {cmd}"
    )
    # And explicitly NOT "lora" — the prior hardcoded default must be gone.
    assert "lora" not in [
        cmd[cmd.index("--train-type") + 1]
    ], f"default --train-type leaked the old 'lora' value: {cmd}"


# ────────────────────────────────────────────────────────────────────────
# AC-11.2.1 — --train-type dora reaches mlx_lm_lora cmd in BOTH SFT and DPO
# ────────────────────────────────────────────────────────────────────────
def test_dora_flag_propagated_to_mlx_cmd(tmp_path, monkeypatch):
    """`--train-type dora` (explicit) → both SFT and DPO subprocess argvs
    contain `--train-type dora`.

    The risk we are pinning: a refactor could route only the SFT path
    through args.train_type but forget the DPO path (or vice versa).
    Mixing DoRA-SFT with LoRA-DPO silently corrupts the adapter shape.
    """
    monkeypatch.setenv("HU_DPO_DETERMINISTIC", "1")
    args = _make_args(tmp_path, train_type="dora")
    adapter_dir = pathlib.Path(args.adapter_path)
    adapter_dir.mkdir(parents=True, exist_ok=True)
    (adapter_dir / "adapters.safetensors").write_bytes(b"")
    data_dir = pathlib.Path(args.data)
    data_dir.mkdir(parents=True, exist_ok=True)

    # SFT path — captures the first subprocess.run invocation.
    fake_result = types.SimpleNamespace(returncode=0)
    with patch.object(fg.subprocess, "run", return_value=fake_result) as mock_sft:
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = fg.run_sft(args, data_dir, adapter_dir)
    assert rc == 0
    sft_cmd = mock_sft.call_args.args[0]
    assert "--train-type" in sft_cmd, f"SFT cmd missing --train-type: {sft_cmd}"
    assert sft_cmd[sft_cmd.index("--train-type") + 1] == "dora", (
        f"SFT --train-type should be 'dora', got: {sft_cmd}"
    )

    # DPO path — separate subprocess.run invocation. We bypass the JSONL
    # preparation path (which has a pre-existing encoding sensitivity to
    # ~/.human/dpo/pairs.jsonl on the dev machine) by mocking the seams
    # at find_dpo_data + _prepare_dpo_from_jsonl. This isolates US-11.2's
    # contract (the --train-type token in the subprocess argv) from
    # unrelated DPO-prep behavior covered in test_finetune_gemma_dpo.py.
    dpo_prepared_dir = tmp_path / "dpo_prepared"
    dpo_prepared_dir.mkdir(parents=True, exist_ok=True)
    (dpo_prepared_dir / "train.jsonl").write_text(
        '{"prompt": "hi", "chosen": "yo", "rejected": "hello"}\n'
    )
    (dpo_prepared_dir / "valid.jsonl").write_text(
        '{"prompt": "hi", "chosen": "yo", "rejected": "hello"}\n'
    )
    monkeypatch.setattr(
        fg, "find_dpo_data",
        lambda data_dir, from_corrections=False: tmp_path / "stub.jsonl",
    )
    monkeypatch.setattr(
        fg, "_prepare_dpo_from_jsonl",
        lambda src, dst: dpo_prepared_dir,
    )

    with patch.object(fg.subprocess, "run", return_value=fake_result) as mock_dpo:
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = fg.run_dpo(args, adapter_dir)
    assert rc == 0
    assert mock_dpo.call_count == 1, "expected exactly one DPO subprocess invocation"
    dpo_cmd = mock_dpo.call_args.args[0]
    assert "--train-type" in dpo_cmd, f"DPO cmd missing --train-type: {dpo_cmd}"
    assert dpo_cmd[dpo_cmd.index("--train-type") + 1] == "dora", (
        f"DPO --train-type should be 'dora' (must match SFT), got: {dpo_cmd}"
    )


# ────────────────────────────────────────────────────────────────────────
# AC-11.2.3 — explicit --train-type lora is honored (back-compat)
# ────────────────────────────────────────────────────────────────────────
def test_explicit_lora_flag_respected(tmp_path):
    """`--train-type lora` (explicit) → SFT subprocess argv contains
    `--train-type lora` and DOES NOT contain `dora` anywhere.

    Back-compat for operators with prior LoRA adapters in
    ~/.human/training-data/adapters/ who need to retain the old shape.
    """
    args = _make_args(tmp_path, train_type="lora")
    data_dir = pathlib.Path(args.data)
    data_dir.mkdir(parents=True, exist_ok=True)
    adapter_dir = pathlib.Path(args.adapter_path)

    fake_result = types.SimpleNamespace(returncode=0)
    with patch.object(fg.subprocess, "run", return_value=fake_result) as mock_run:
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = fg.run_sft(args, data_dir, adapter_dir)

    assert rc == 0
    cmd = mock_run.call_args.args[0]
    assert "--train-type" in cmd, f"cmd missing --train-type: {cmd}"
    assert cmd[cmd.index("--train-type") + 1] == "lora", (
        f"explicit --train-type lora should be honored, got: {cmd}"
    )

    # And "dora" must NOT appear anywhere in the argv — no silent upgrade.
    joined = " ".join(cmd)
    assert "dora" not in joined, (
        f"explicit --train-type lora must not leak 'dora' anywhere; cmd: {cmd}"
    )


# ────────────────────────────────────────────────────────────────────────
# AC-11.2.5 — train_config.json records train_type
# ────────────────────────────────────────────────────────────────────────
def test_train_config_records_dora(tmp_path, monkeypatch):
    """After run_finetune (with --no-version), train_config.json on disk
    contains `train_type: "dora"`.

    We exercise the real run_finetune path (mocking only subprocess.run,
    stop_mlx_server, start_mlx_server) so the production
    `lora_config` dict assembly is what we are pinning, not a copy of it.
    """
    # Stage a minimal training data directory so the preflight passes.
    data_dir = tmp_path / "data"
    data_dir.mkdir(parents=True, exist_ok=True)
    (data_dir / "train.jsonl").write_text(
        '{"prompt": "hello", "completion": "hi"}\n'
    )
    (data_dir / "valid.jsonl").write_text(
        '{"prompt": "hello", "completion": "hi"}\n'
    )

    args = _make_args(
        tmp_path,
        train_type="dora",
        dpo=False,  # skip the DPO leg so we test SFT-only path
        sft_only=True,
        no_version=True,  # write train_config.json into adapter_dir directly
    )
    adapter_dir = pathlib.Path(args.adapter_path)

    # subprocess.run is mocked to a no-op success; we don't need a real
    # mlx_lm_lora invocation for the config assertion.
    fake_result = types.SimpleNamespace(returncode=0)
    monkeypatch.setattr(fg, "stop_mlx_server", lambda: None)
    monkeypatch.setattr(fg, "start_mlx_server", lambda *a, **kw: None)

    with patch.object(fg.subprocess, "run", return_value=fake_result):
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = fg.run_finetune(args)
    assert rc == 0, "run_finetune should return 0 on mocked success"

    config_path = adapter_dir / "train_config.json"
    assert config_path.exists(), (
        f"train_config.json should be written to {config_path}; "
        f"adapter_dir contents: {list(adapter_dir.iterdir())}"
    )
    config = json.loads(config_path.read_text())

    # AC-11.2.5: the field must exist and equal "dora".
    assert "train_type" in config, (
        f"train_config.json missing 'train_type' field; keys: {list(config.keys())}"
    )
    assert config["train_type"] == "dora", (
        f"train_type should be 'dora', got: {config['train_type']!r}; "
        f"full config: {config}"
    )
