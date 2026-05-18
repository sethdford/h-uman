"""Sprint 7 / US-7.4 — tests for LoRA rank + target-modules CLI surface.

Covers all five acceptance criteria (AC-7.4.1 .. AC-7.4.5). The shell-gate
ACs (AC-7.4.3, AC-7.4.4) are exercised separately in CI via
`bash scripts/check-lora-ab.sh` and `bash scripts/check-lora-baseline.sh`;
this file pins the *interface contract* those gates rely on (the JSON
shape of the `--lora-parameters` token and the recorded `train_config.json`
fields) so the gates can rely on stable inputs.

Mocks `subprocess.run` — no real `mlx_lm_lora.train` invocation, no real
model weights, no network. The script's filename has a hyphen, so we
load it via `importlib.util` just like `tests/test_finetune_gemma_dpo.py`.
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
    """argparse.Namespace shaped like main()'s `args` post-default-resolution."""
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
    )
    defaults.update(overrides)
    return argparse.Namespace(**defaults)


def _extract_lora_parameters_json(cmd: list[str]) -> dict:
    """Find `--lora-parameters` in argv and return the parsed JSON dict.

    The flag is emitted as TWO argv slots: `["--lora-parameters", "<json>"]`.
    Asserting on the indexed slot also pins the design's risk #3 (any
    refactor that joins the cmd into a shell string fails this lookup).
    """
    assert "--lora-parameters" in cmd, (
        f"--lora-parameters not found in cmd: {cmd}"
    )
    idx = cmd.index("--lora-parameters")
    assert idx + 1 < len(cmd), (
        f"--lora-parameters appeared as the last argv slot with no value: {cmd}"
    )
    json_str = cmd[idx + 1]
    return json.loads(json_str)


def _stage_jsonl_dpo(tmp_path: pathlib.Path) -> pathlib.Path:
    """Stage a real {prompt, chosen, rejected} JSONL into <tmp>/data/dpo/pairs.jsonl
    and pre-create the SFT adapter dir so `run_dpo` finds adapters.safetensors."""
    dst_dir = tmp_path / "data" / "dpo"
    dst_dir.mkdir(parents=True, exist_ok=True)
    dst = dst_dir / "pairs.jsonl"
    dst.write_text(
        '{"prompt": "hey", "chosen": "yo", "rejected": "Hello there."}\n'
        '{"prompt": "lunch?", "chosen": "ya", "rejected": "Yes, certainly."}\n'
        '{"prompt": "ok?", "chosen": "k", "rejected": "Acknowledged."}\n'
    )
    adapter_dir = tmp_path / "adapter"
    adapter_dir.mkdir(parents=True, exist_ok=True)
    (adapter_dir / "adapters.safetensors").write_bytes(b"")
    return dst


# ────────────────────────────────────────────────────────────────────────
# AC-7.4.1 — --target-modules / --rank propagate to the mlx_lm cmd
# ────────────────────────────────────────────────────────────────────────
def test_target_modules_propagated_to_mlx_cmd(tmp_path):
    """GIVEN --rank 64 --target-modules q,k,v,o,gate,up,down, WHEN run_sft
    fires, THEN --lora-parameters JSON carries rank=64 and the 7 keys."""
    seven = ["q_proj", "k_proj", "v_proj", "o_proj",
             "gate_proj", "up_proj", "down_proj"]
    args = _make_args(tmp_path, rank=64, target_modules=seven)
    data_dir = tmp_path / "data"
    data_dir.mkdir(parents=True, exist_ok=True)
    adapter_dir = tmp_path / "adapter"

    fake_result = types.SimpleNamespace(returncode=0)
    with patch.object(fg.subprocess, "run", return_value=fake_result) as mock_run:
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = fg.run_sft(args, data_dir, adapter_dir)

    assert rc == 0
    assert mock_run.call_count == 1, "expected exactly one SFT subprocess invocation"
    cmd = mock_run.call_args.args[0]
    assert isinstance(cmd, list)

    params = _extract_lora_parameters_json(cmd)
    assert params["rank"] == 64, params
    assert params["keys"] == seven, params
    # Schema invariants we promised to mlx-lm via design §3:
    assert params["dropout"] == 0.0
    assert params["scale"] == 10.0


# ────────────────────────────────────────────────────────────────────────
# AC-7.4.2 — defaults: 31b => rank=32, modules=QKVO
# ────────────────────────────────────────────────────────────────────────
def test_31b_default_rank_32_default_modules_qkvo(tmp_path):
    """GIVEN no --rank and no --target-modules, WHEN main() resolves args
    for target=31b, THEN the rank materialises as 32 and target_modules as
    the canonical QKVO set; AND the JSON helper emits the same."""
    # AC anchor: the MODEL_TARGETS entry itself was bumped.
    assert fg.MODEL_TARGETS["31b"]["default_rank"] == 32, (
        f"31b default_rank must be 32 post-Sprint-7, got "
        f"{fg.MODEL_TARGETS['31b']['default_rank']}"
    )

    # And the helper materialises the JSON shape that mlx-lm sees on the wire.
    args = _make_args(tmp_path)  # rank=32, target_modules=DEFAULT (QKVO)
    json_str = fg._build_lora_parameters_json(args)
    params = json.loads(json_str)
    assert params["rank"] == 32, params
    assert params["keys"] == ["q_proj", "k_proj", "v_proj", "o_proj"], params

    # And: when --target-modules is omitted via the CSV path, main()'s
    # default-resolution falls through to DEFAULT_TARGET_MODULES. Simulate
    # the post-default-resolution state (an empty CSV cleared by main()).
    args_none = _make_args(tmp_path)
    # Re-set to None and re-resolve as main() does, to prove the fallback.
    args_none.target_modules = None
    json_none = fg._build_lora_parameters_json(args_none)
    assert json.loads(json_none)["keys"] == ["q_proj", "k_proj", "v_proj", "o_proj"]


# ────────────────────────────────────────────────────────────────────────
# AC-7.4.5 — train_config.json records target_modules + rank
# ────────────────────────────────────────────────────────────────────────
def test_train_config_records_target_modules(tmp_path):
    """GIVEN a versioned adapter is written, WHEN train_config.json is
    inspected, THEN it contains BOTH `target_modules` (as a list) AND
    `rank` (the new 32 default)."""
    adapter_dir = tmp_path / "adapter"
    adapter_dir.mkdir(parents=True, exist_ok=True)
    # version_adapter copies the tree, so populate a small marker.
    (adapter_dir / "adapters.safetensors").write_bytes(b"")

    lora_config = {
        "target": "31b",
        "model": "mlx-community/gemma-4-31b-it-4bit",
        "adapter_path": str(adapter_dir),
        "rank": 32,
        "target_modules": ["q_proj", "k_proj", "v_proj", "o_proj"],
        "iters": 10,
        "batch_size": 1,
        "learning_rate": 1e-6,
        "max_seq_length": 2048,
        "dpo": True,
        "data": str(tmp_path / "data"),
        "train_examples": 0,
        "timestamp": "2026-05-16 12:00:00",
    }

    buf = io.StringIO()
    with redirect_stdout(buf):
        versioned = fg.version_adapter(adapter_dir, lora_config)

    cfg_path = versioned / "train_config.json"
    assert cfg_path.exists(), f"missing train_config.json at {cfg_path}"
    parsed = json.loads(cfg_path.read_text())
    assert parsed.get("rank") == 32, parsed
    assert parsed.get("target_modules") == ["q_proj", "k_proj", "v_proj", "o_proj"], parsed
    # version_adapter is also expected to stamp version metadata.
    assert "version" in parsed and "versioned_path" in parsed


# ────────────────────────────────────────────────────────────────────────
# Regression guard — run_dpo() also emits --lora-parameters with the same JSON
# ────────────────────────────────────────────────────────────────────────
def test_dpo_cmd_also_carries_lora_parameters(tmp_path, monkeypatch):
    """Catches the easy mistake of plumbing --lora-parameters through the
    SFT path only. The DPO cmd built by run_dpo() must also carry the same
    JSON token — one code path, two consumers (stock mlx_lm for SFT,
    mlx-lm-lora fork for DPO)."""
    monkeypatch.setenv("HU_DPO_DETERMINISTIC", "1")
    _stage_jsonl_dpo(tmp_path)

    seven = ["q_proj", "k_proj", "v_proj", "o_proj",
             "gate_proj", "up_proj", "down_proj"]
    args = _make_args(tmp_path, rank=64, target_modules=seven)
    adapter_dir = pathlib.Path(args.adapter_path)

    fake_result = types.SimpleNamespace(returncode=0)
    with patch.object(fg.subprocess, "run", return_value=fake_result) as mock_run:
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = fg.run_dpo(args, adapter_dir)

    assert rc == 0
    assert mock_run.call_count == 1, "expected one DPO subprocess invocation"
    cmd = mock_run.call_args.args[0]

    # Must be the mlx-lm-lora fork path (D1 contract from US-7.1).
    assert "mlx_lm_lora.train" in cmd, f"cmd: {cmd}"

    params = _extract_lora_parameters_json(cmd)
    assert params["rank"] == 64
    assert params["keys"] == seven


# ────────────────────────────────────────────────────────────────────────
# HIGH-1 fix — reject empty --target-modules CSV explicitly
# ────────────────────────────────────────────────────────────────────────
def test_empty_target_modules_csv_errors_out(tmp_path):
    """HIGH-1 fix: a stray-comma CSV (e.g. `--target-modules ,` or
    `--target-modules " , "`) collapses to [] after parsing. main() must
    exit non-zero with a clear error rather than silently falling back to
    QKVO via the helper's internal `or DEFAULT_TARGET_MODULES` guard.

    The internal guard stays in place for synthetic Namespaces built by
    run_train_all / run_speculative_draft_training (which pass
    target_modules=None on purpose); only the CLI surface is hardened.
    """
    import subprocess as _sp

    data_dir = tmp_path / "data"
    data_dir.mkdir(parents=True, exist_ok=True)
    # Materialise a minimal train.jsonl so the script does not bail on
    # missing-data before reaching our validation. (Defensive — main()
    # actually validates target_modules before invoking run_finetune,
    # but pinning the test to ONLY exercise the CSV-empty path keeps
    # the assertion specific.)
    (data_dir / "train.jsonl").write_text('{"messages": []}\n')

    for bad_csv in (",", " , ", ",,,"):
        proc = _sp.run(
            ["python3", str(_SCRIPT),
             "--target", "31b",
             "--data", str(data_dir),
             "--target-modules", bad_csv,
             "--iters", "1"],
            capture_output=True, text=True, timeout=15,
        )
        assert proc.returncode != 0, (
            f"expected non-zero exit for --target-modules={bad_csv!r}; "
            f"stdout={proc.stdout!r} stderr={proc.stderr!r}"
        )
        combined = proc.stdout + proc.stderr
        assert "empty list" in combined, (
            f"expected 'empty list' in error for {bad_csv!r}; got: {combined!r}"
        )


# ────────────────────────────────────────────────────────────────────────
# Regression guard — --lora-parameters is a SINGLE argv slot (design risk #3)
# ────────────────────────────────────────────────────────────────────────
def test_lora_parameters_is_single_argv_slot(tmp_path):
    """If anyone refactors run_sft to `" ".join(cmd)` or `shell=True`, the
    JSON's embedded spaces and braces would break shell parsing. We assert
    the flag value lives in exactly ONE list element so any such refactor
    fails this test immediately."""
    args = _make_args(tmp_path, rank=32)
    data_dir = tmp_path / "data"
    data_dir.mkdir(parents=True, exist_ok=True)
    adapter_dir = tmp_path / "adapter"

    fake_result = types.SimpleNamespace(returncode=0)
    with patch.object(fg.subprocess, "run", return_value=fake_result) as mock_run:
        buf = io.StringIO()
        with redirect_stdout(buf):
            fg.run_sft(args, data_dir, adapter_dir)

    cmd = mock_run.call_args.args[0]
    idx = cmd.index("--lora-parameters")
    json_slot = cmd[idx + 1]
    assert isinstance(json_slot, str), f"expected str argv slot, got {type(json_slot)}"
    # The slot must be valid JSON on its own (no shell-style concatenation
    # like `--lora-parameters={json}` which would also pass argparse but
    # break the indexing contract).
    assert json_slot.startswith("{") and json_slot.endswith("}"), (
        f"JSON slot is not a bare JSON object: {json_slot!r}"
    )
    json.loads(json_slot)  # raises on malformed JSON
