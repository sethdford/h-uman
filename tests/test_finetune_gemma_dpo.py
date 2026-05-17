"""Sprint 7 / US-7.1 — tests for the activated DPO preference pass.

Mocks `subprocess.run` and asserts the argv shape emitted by `run_dpo()`
in `scripts/finetune-gemma.py`. No real `mlx_lm_lora` invocation, no real
model weights, no network. Per Sprint 7 decision D1, AC-7.1.1's literal
`--fine-tune-type dpo` is replaced with `-m mlx_lm_lora.train --train-mode
dpo --train-type lora`.

The script's filename has a hyphen, so we load it via importlib.util.
"""
from __future__ import annotations

import argparse
import importlib.util
import io
import json
import pathlib
import sys
import types
from contextlib import redirect_stdout
from unittest.mock import patch

import pytest


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


# ── argparse.Namespace factory matching the production CLI shape ──────────
def _make_args(tmp_path: pathlib.Path, **overrides) -> argparse.Namespace:
    defaults = dict(
        target="31b",
        model=None,
        data=str(tmp_path / "data"),
        adapter_path=str(tmp_path / "adapter"),
        iters=200,
        batch_size=1,
        learning_rate=1e-5,
        rank=16,
        num_layers=8,
        max_seq_length=2048,
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
        # Sprint 11 / US-11.2: default flipped from "lora" to "dora";
        # pin "lora" here to preserve the Sprint 7/8 contract these tests verify.
        train_type="lora",
        # Sprint 11 / US-11.3: opt out of the early-stop wrapper for tests
        # that assert direct subprocess.run shape.
        early_stopping_signal="none",
        # Sprint 11 / US-11.1: opt out of length normalization to preserve
        # the pre-US-11.1 DPO argv shape this test expects.
        length_normalize=False,
    )
    defaults.update(overrides)
    return argparse.Namespace(**defaults)


def _stage_jsonl_dpo(tmp_path: pathlib.Path) -> pathlib.Path:
    """Stage tests/fixtures/dpo_pairs_min.jsonl into <tmp>/data/dpo/pairs.jsonl."""
    src = _HERE / "fixtures" / "dpo_pairs_min.jsonl"
    assert src.exists(), f"missing fixture: {src}"
    dst_dir = tmp_path / "data" / "dpo"
    dst_dir.mkdir(parents=True, exist_ok=True)
    dst = dst_dir / "pairs.jsonl"
    dst.write_bytes(src.read_bytes())
    # Adapter dir must look pre-existing to mimic post-SFT state.
    adapter_dir = tmp_path / "adapter"
    adapter_dir.mkdir(parents=True, exist_ok=True)
    (adapter_dir / "adapters.safetensors").write_bytes(b"")
    return dst


def _stage_corrections_jsonl(tmp_path: pathlib.Path) -> pathlib.Path:
    """Stage a pairs.jsonl into <HOME>/.human/dpo/pairs.jsonl — the LOCKED
    cross-story path that US-7.2's miner writes to and our --from-corrections
    flag reads from. We synthesise the rows from the JSONL fixture so this
    test does not depend on the gitignored .db fixture."""
    src = _HERE / "fixtures" / "dpo_pairs_min.jsonl"
    assert src.exists(), f"missing fixture: {src}"
    home_dpo = tmp_path / ".human" / "dpo"
    home_dpo.mkdir(parents=True, exist_ok=True)
    dst = home_dpo / "pairs.jsonl"
    dst.write_bytes(src.read_bytes())
    adapter_dir = tmp_path / "adapter"
    adapter_dir.mkdir(parents=True, exist_ok=True)
    (adapter_dir / "adapters.safetensors").write_bytes(b"")
    return dst


# ────────────────────────────────────────────────────────────────────────
# AC-7.1.1 — real DPO subprocess invoked with correct argv tokens
# ────────────────────────────────────────────────────────────────────────
def test_dpo_pass_invokes_real_dpo_subprocess(tmp_path, monkeypatch):
    monkeypatch.setenv("HU_DPO_DETERMINISTIC", "1")
    # Re-home `Path.home()` so `find_dpo_data` does not pick up the
    # user's real `~/.human/dpo/pairs.jsonl` before the staged fixture.
    monkeypatch.setattr(fg.Path, "home", classmethod(lambda cls: tmp_path))
    _stage_jsonl_dpo(tmp_path)
    args = _make_args(tmp_path)
    adapter_dir = pathlib.Path(args.adapter_path)

    fake_result = types.SimpleNamespace(returncode=0)
    with patch.object(fg.subprocess, "run", return_value=fake_result) as mock_run:
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = fg.run_dpo(args, adapter_dir)

    assert rc == 0
    assert mock_run.call_count == 1, "expected exactly one DPO subprocess invocation"
    cmd = mock_run.call_args.args[0]
    assert isinstance(cmd, list)

    # Per Sprint 7 D1 + Sprint 11 US-11.1: argv contains `-m
    # scripts.mlx_lora_entry --train-mode dpo --train-type lora`. The entry
    # wrapper applies the US-11.1 length-normalization patch and then
    # dispatches to `mlx_lm_lora.train.main()`.
    assert "-m" in cmd, f"cmd missing -m: {cmd}"
    assert cmd[cmd.index("-m") + 1] == "scripts.mlx_lora_entry", f"cmd: {cmd}"

    assert "--train-mode" in cmd, f"cmd missing --train-mode: {cmd}"
    assert cmd[cmd.index("--train-mode") + 1] == "dpo", f"cmd: {cmd}"

    assert "--train-type" in cmd, f"cmd missing --train-type: {cmd}"
    assert cmd[cmd.index("--train-type") + 1] == "lora", f"cmd: {cmd}"

    # --reference-model-path MUST be set explicitly (design §5.3 pitfall).
    assert "--reference-model-path" in cmd, f"cmd missing --reference-model-path: {cmd}"

    # The materialised train.jsonl must contain BOTH `chosen` AND `rejected`
    # top-level keys — proves the formatter passes both fields, AC requirement.
    train_jsonl = pathlib.Path(args.data) / "dpo_prepared" / "train.jsonl"
    assert train_jsonl.exists(), f"missing train.jsonl at {train_jsonl}"
    first_row = json.loads(train_jsonl.read_text().splitlines()[0])
    assert "chosen" in first_row, f"first row missing 'chosen': {first_row}"
    assert "rejected" in first_row, f"first row missing 'rejected': {first_row}"
    assert "prompt" in first_row, f"first row missing 'prompt': {first_row}"

    # Stock `mlx_lm` flags MUST NOT appear in the DPO argv (we are using
    # the mlx-lm-lora fork instead).
    joined = " ".join(cmd)
    assert "-m mlx_lm lora" not in joined, f"stock mlx_lm flag leaked into DPO cmd: {cmd}"


# ────────────────────────────────────────────────────────────────────────
# AC-7.1.3 — missing DPO data is non-fatal
# ────────────────────────────────────────────────────────────────────────
def test_dpo_missing_data_nonfatal(tmp_path, monkeypatch):
    # No DPO data on disk, anywhere. Stub find_dpo_data to return None for
    # full determinism (the production search probes ~/.human/dpo and any
    # convo-training* dir at the repo root; pinning the seam keeps the test
    # hermetic).
    monkeypatch.setattr(fg, "find_dpo_data", lambda data_dir, from_corrections=False: None)

    args = _make_args(tmp_path)
    adapter_dir = pathlib.Path(args.adapter_path)
    adapter_dir.mkdir(parents=True, exist_ok=True)

    fake_result = types.SimpleNamespace(returncode=0)
    with patch.object(fg.subprocess, "run", return_value=fake_result) as mock_run:
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = fg.run_dpo(args, adapter_dir)
    out = buf.getvalue()

    assert rc == 0, "missing DPO data must be non-fatal"
    assert mock_run.call_count == 0, "no subprocess should run when DPO data is absent"
    assert "no preference data" in out, f"expected clearly-labeled warning in: {out!r}"
    assert "skipping" in out, f"expected 'skipping' in: {out!r}"


# ────────────────────────────────────────────────────────────────────────
# AC-7.1.4 — --sft-only short-circuits DPO entirely
# ────────────────────────────────────────────────────────────────────────
def test_sft_only_skips_dpo_entirely(tmp_path, monkeypatch):
    # Stage real DPO data so that *without* --sft-only the path would fire.
    monkeypatch.setenv("HU_DPO_DETERMINISTIC", "1")
    _stage_jsonl_dpo(tmp_path)

    # We exercise the higher-level dispatch logic to prove the wiring: with
    # sft_only=True, run_finetune must NOT invoke run_dpo. The cleanest way
    # to assert this without booting the whole pipeline is to patch run_dpo
    # itself and check it was never called.
    args = _make_args(tmp_path, sft_only=True, dpo=True)
    # Ensure train.jsonl exists so run_sft passes its preflight.
    train_jsonl = pathlib.Path(args.data) / "train.jsonl"
    train_jsonl.parent.mkdir(parents=True, exist_ok=True)
    train_jsonl.write_text('{"messages": []}\n')

    fake_result = types.SimpleNamespace(returncode=0)
    with patch.object(fg.subprocess, "run", return_value=fake_result) as mock_run, \
         patch.object(fg, "run_dpo") as mock_dpo, \
         patch.object(fg, "stop_mlx_server", return_value=None), \
         patch.object(fg, "start_mlx_server", return_value=None):
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = fg.run_finetune(args)

    assert rc == 0
    assert mock_dpo.call_count == 0, "--sft-only must skip run_dpo entirely"

    # And: no subprocess call's argv contains the token 'dpo' anywhere.
    for call in mock_run.call_args_list:
        cmd = call.args[0]
        assert all(tok != "dpo" for tok in cmd), \
            f"--sft-only must not leak 'dpo' into any subprocess argv: {cmd}"


# ────────────────────────────────────────────────────────────────────────
# AC-7.2.3 (bonus, lands here) — --from-corrections resolves the locked path
# ────────────────────────────────────────────────────────────────────────
def test_from_corrections_flag_resolves_db(tmp_path, monkeypatch):
    """When --from-corrections is set, find_dpo_data resolves to
    ~/.human/dpo/pairs.jsonl (LOCKED path; produced by US-7.2's miner)
    and the DPO subprocess then runs against materialised pairs.
    """
    monkeypatch.setenv("HU_DPO_DETERMINISTIC", "1")
    monkeypatch.setenv("HOME", str(tmp_path))
    # Re-import Path.home() — Path.home() reads $HOME at call time on POSIX,
    # but on macOS Python uses pwd.getpwuid() which ignores $HOME. Patch
    # Path.home directly to make the test hermetic.
    monkeypatch.setattr(fg.Path, "home", classmethod(lambda cls: tmp_path))

    staged = _stage_corrections_jsonl(tmp_path)
    assert staged.exists()

    args = _make_args(tmp_path, from_corrections=True)
    adapter_dir = pathlib.Path(args.adapter_path)

    fake_result = types.SimpleNamespace(returncode=0)
    with patch.object(fg.subprocess, "run", return_value=fake_result) as mock_run:
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = fg.run_dpo(args, adapter_dir)

    assert rc == 0
    assert mock_run.call_count == 1, \
        "--from-corrections with a real pairs.jsonl should invoke DPO once"

    # The DPO data dir's train.jsonl should contain the same content from the
    # corrections file (filtered to {prompt, chosen, rejected} keys).
    train_jsonl = pathlib.Path(args.data) / "dpo_prepared" / "train.jsonl"
    assert train_jsonl.exists()
    rows = [json.loads(l) for l in train_jsonl.read_text().splitlines() if l.strip()]
    assert len(rows) >= 1
    for row in rows:
        assert {"prompt", "chosen", "rejected"}.issubset(row.keys())


# ────────────────────────────────────────────────────────────────────────
# Negative guard — `--fine-tune-type dpo` MUST NOT be emitted (D1 contract)
# ────────────────────────────────────────────────────────────────────────
def test_dpo_does_not_emit_stock_mlx_lm_flag(tmp_path, monkeypatch):
    """Per Sprint 7 D1, the implementer-emitted DPO command uses the
    `mlx-lm-lora` fork, NOT stock `mlx_lm`. Asserting the negative protects
    against regression to the broken pre-Sprint-7 behaviour."""
    monkeypatch.setenv("HU_DPO_DETERMINISTIC", "1")
    monkeypatch.setattr(fg.Path, "home", classmethod(lambda cls: tmp_path))
    _stage_jsonl_dpo(tmp_path)
    args = _make_args(tmp_path)
    adapter_dir = pathlib.Path(args.adapter_path)

    fake_result = types.SimpleNamespace(returncode=0)
    with patch.object(fg.subprocess, "run", return_value=fake_result) as mock_run:
        buf = io.StringIO()
        with redirect_stdout(buf):
            fg.run_dpo(args, adapter_dir)

    cmd = mock_run.call_args.args[0]
    # The legacy SFT-on-chosen-only path emitted `--fine-tune-type lora` AND
    # invoked `-m mlx_lm lora`. Neither is allowed here.
    assert "--fine-tune-type" not in cmd, \
        f"DPO cmd must NOT contain stock mlx_lm's --fine-tune-type: {cmd}"
    assert "mlx_lm" not in cmd or cmd[cmd.index("-m") + 1] != "mlx_lm", \
        f"DPO cmd must invoke mlx_lm_lora.train, not stock mlx_lm: {cmd}"
