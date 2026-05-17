"""Sprint 11 / US-11.4 — DPOP loss head argv tests for `run_dpo()`.

Verifies the CLI shape emitted by `scripts/finetune-gemma.py:run_dpo()` when
the operator opts into the Smaug-style positive-clipping loss head via
`--dpo-cpo-loss-type dpop`.

The critical contract pinned here (from the US-11.4 design doc and the lead's
implementation brief):

  1. Default loss type stays `sigmoid` — the Sprint 7/8 contract MUST NOT
     break. AC-11.4.5.
  2. `--dpo-cpo-loss-type dpop` propagates to upstream `mlx_lm_lora.train`
     intact (same flag spelling).
  3. `--delta <dpop_delta>` is ALWAYS emitted alongside `dpop` — never relying
     on upstream's default. Upstream's default is `50.0`, which is 500x the
     Smaug-recommended `0.1` and would catastrophically over-anchor.
  4. `--dpop-delta` overrides the default `0.1`.
  5. The cross-cutting risk: `run_dpo()` has TWO subprocess paths post-US-11.3
     (the `chosen_r` early-stop wrapper vs. plain `subprocess.run`). Both
     branches MUST build the same `cmd` list with respect to DPOP flags. We
     test BOTH branches end-to-end here so a future drift between them fails
     loud.

No real `mlx_lm_lora`, no real model, no network — subprocess.run and
subprocess.Popen are mocked.

The script's filename has a hyphen, so we load it via importlib.util (matches
the precedent in `tests/test_finetune_gemma_dpo.py`).
"""
from __future__ import annotations

import argparse
import importlib.util
import io
import pathlib
import sys
import types
from contextlib import redirect_stdout
from unittest.mock import patch

import pytest


# ── Module loader ───────────────────────────────────────────────────────
_HERE = pathlib.Path(__file__).resolve().parent
_SCRIPT = _HERE.parent / "scripts" / "finetune-gemma.py"


def _load_finetune_gemma():
    spec = importlib.util.spec_from_file_location("finetune_gemma_dpop", _SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules["finetune_gemma_dpop"] = mod
    spec.loader.exec_module(mod)
    return mod


fg = _load_finetune_gemma()


# ── Namespace factory (mirrors test_finetune_gemma_dpo.py) ──────────────
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
        train_type="lora",
        # Default test posture: opt out of US-11.3 early-stop wrapper so the
        # plain subprocess.run branch is exercised. The cross-branch test
        # below flips this to `chosen_r` and re-asserts the same argv shape.
        early_stopping_signal="none",
        length_normalize=False,
        # US-11.4 — defaults match the production argparse defaults so the
        # baseline test pins the Sprint 7/8 sigmoid contract.
        dpo_cpo_loss_type="sigmoid",
        dpop_delta=0.1,
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
    adapter_dir = tmp_path / "adapter"
    adapter_dir.mkdir(parents=True, exist_ok=True)
    (adapter_dir / "adapters.safetensors").write_bytes(b"")
    return dst


def _assert_adjacent(cmd: list, flag: str, value: str) -> None:
    """`cmd` contains `flag` immediately followed by `value`. Fails loud."""
    assert flag in cmd, f"missing {flag} in argv: {cmd}"
    idx = cmd.index(flag)
    assert idx + 1 < len(cmd), f"{flag} is the last token in argv: {cmd}"
    assert cmd[idx + 1] == value, (
        f"{flag} expected to be followed by {value!r} but got "
        f"{cmd[idx + 1]!r}; full argv: {cmd}"
    )


# ────────────────────────────────────────────────────────────────────────
# AC-11.4.5 (back-compat) — default loss head is sigmoid, no --delta
# ────────────────────────────────────────────────────────────────────────
def test_default_loss_type_is_sigmoid_no_delta(tmp_path, monkeypatch):
    """The Sprint 7/8 contract: with no opt-in, run_dpo emits
    `--dpo-cpo-loss-type sigmoid` and NO `--delta` flag at all. A regression
    that flips the default to dpop would silently change training dynamics
    for every existing pipeline."""
    monkeypatch.setenv("HU_DPO_DETERMINISTIC", "1")
    monkeypatch.setattr(fg.Path, "home", classmethod(lambda cls: tmp_path))
    _stage_jsonl_dpo(tmp_path)
    args = _make_args(tmp_path)  # defaults: sigmoid + dpop_delta=0.1
    adapter_dir = pathlib.Path(args.adapter_path)

    fake_result = types.SimpleNamespace(returncode=0)
    with patch.object(fg.subprocess, "run", return_value=fake_result) as mock_run:
        with redirect_stdout(io.StringIO()):
            rc = fg.run_dpo(args, adapter_dir)

    assert rc == 0
    cmd = mock_run.call_args.args[0]
    _assert_adjacent(cmd, "--dpo-cpo-loss-type", "sigmoid")
    assert "--delta" not in cmd, (
        "--delta MUST NOT be emitted when loss type is not dpop "
        "(would confuse upstream and the operator on debug): "
        f"{cmd}"
    )
    # The DPOP flag is not user-facing as a positional in the cmd — it is
    # only translated when dpop is selected. Defense-in-depth: assert that
    # neither --dpop-delta nor --dpop-lambda leak into upstream argv
    # (those are our user-facing names; upstream consumes --delta only).
    assert "--dpop-delta" not in cmd, f"--dpop-delta leaked into upstream argv: {cmd}"
    assert "--dpop-lambda" not in cmd, f"--dpop-lambda leaked into upstream argv: {cmd}"


# ────────────────────────────────────────────────────────────────────────
# AC-11.4.1 — --dpo-cpo-loss-type dpop propagates AND emits --delta 0.1
# ────────────────────────────────────────────────────────────────────────
def test_dpop_propagates_loss_type_and_default_delta(tmp_path, monkeypatch):
    """Opting into dpop MUST yield BOTH the loss-type flag AND the explicit
    delta. The lead's critical contract: never inherit upstream's --delta
    default (50.0)."""
    monkeypatch.setenv("HU_DPO_DETERMINISTIC", "1")
    monkeypatch.setattr(fg.Path, "home", classmethod(lambda cls: tmp_path))
    _stage_jsonl_dpo(tmp_path)
    args = _make_args(tmp_path, dpo_cpo_loss_type="dpop")  # default delta=0.1
    adapter_dir = pathlib.Path(args.adapter_path)

    fake_result = types.SimpleNamespace(returncode=0)
    with patch.object(fg.subprocess, "run", return_value=fake_result) as mock_run:
        with redirect_stdout(io.StringIO()):
            rc = fg.run_dpo(args, adapter_dir)

    assert rc == 0
    cmd = mock_run.call_args.args[0]
    _assert_adjacent(cmd, "--dpo-cpo-loss-type", "dpop")
    # The whole point of this story: --delta is explicit, equals 0.1.
    _assert_adjacent(cmd, "--delta", "0.1")
    # And there is exactly one --delta in the argv (no accidental duplication).
    assert cmd.count("--delta") == 1, f"expected exactly one --delta: {cmd}"


# ────────────────────────────────────────────────────────────────────────
# --dpop-delta overrides the default
# ────────────────────────────────────────────────────────────────────────
def test_dpop_delta_override(tmp_path, monkeypatch):
    """Operator-supplied --dpop-delta overrides the 0.1 default verbatim.
    Tests both a smaller (0.05) and a larger (0.5) value to confirm the
    formatter is not silently clipping."""
    monkeypatch.setenv("HU_DPO_DETERMINISTIC", "1")
    monkeypatch.setattr(fg.Path, "home", classmethod(lambda cls: tmp_path))

    for delta_value, expected_str in [(0.05, "0.05"), (0.5, "0.5")]:
        _stage_jsonl_dpo(tmp_path)
        args = _make_args(
            tmp_path,
            dpo_cpo_loss_type="dpop",
            dpop_delta=delta_value,
        )
        adapter_dir = pathlib.Path(args.adapter_path)

        fake_result = types.SimpleNamespace(returncode=0)
        with patch.object(fg.subprocess, "run", return_value=fake_result) as mock_run:
            with redirect_stdout(io.StringIO()):
                rc = fg.run_dpo(args, adapter_dir)

        assert rc == 0
        cmd = mock_run.call_args.args[0]
        _assert_adjacent(cmd, "--delta", expected_str)


# ────────────────────────────────────────────────────────────────────────
# Cross-cutting risk — DPOP must work with the chosen_r early-stop wrapper.
# Both subprocess branches in run_dpo() build the SAME cmd list. If a future
# refactor adds the DPOP flags to only one branch, this test fails loud.
# ────────────────────────────────────────────────────────────────────────
def test_dpop_with_chosen_r_early_stop_wrapper(tmp_path, monkeypatch):
    """The US-11.3 early-stop wrapper path also wires the DPOP flags. We
    patch the wrapper itself to capture the cmd argument and assert the
    same shape contract as the plain-subprocess branch."""
    monkeypatch.setenv("HU_DPO_DETERMINISTIC", "1")
    monkeypatch.setattr(fg.Path, "home", classmethod(lambda cls: tmp_path))
    _stage_jsonl_dpo(tmp_path)
    args = _make_args(
        tmp_path,
        early_stopping_signal="chosen_r",
        dpo_cpo_loss_type="dpop",
        dpop_delta=0.1,
    )
    adapter_dir = pathlib.Path(args.adapter_path)

    # Capture the early-stop module's run_with_early_stop invocation.
    # We replace fg._load_early_stop_module() outright with a function
    # that returns a stub module whose run_with_early_stop captures the
    # cmd argv. Patching the live module object does not work because
    # `_load_early_stop_module` re-executes the module spec on every
    # call (returning a fresh module each time).
    captured = {}

    def fake_run_with_early_stop(cmd, detector, **kwargs):
        captured["cmd"] = cmd
        captured["kwargs"] = kwargs
        return 0, None

    # Build the stub module. We grab the real module once for the
    # ChosenRPlateauDetector class (run_dpo instantiates it before calling
    # run_with_early_stop), then override only the function under test.
    real_es_mod = fg._load_early_stop_module()
    stub = types.SimpleNamespace(
        ChosenRPlateauDetector=real_es_mod.ChosenRPlateauDetector,
        run_with_early_stop=fake_run_with_early_stop,
    )
    with patch.object(fg, "_load_early_stop_module", return_value=stub):
        with redirect_stdout(io.StringIO()):
            rc = fg.run_dpo(args, adapter_dir)

    assert rc == 0, "early-stop wrapper should return 0 on a successful (mocked) run"
    cmd = captured["cmd"]
    assert isinstance(cmd, list), f"expected a list cmd: {cmd!r}"
    _assert_adjacent(cmd, "--dpo-cpo-loss-type", "dpop")
    _assert_adjacent(cmd, "--delta", "0.1")
    # US-11.1 contract from Wave 0: env=child_env must be propagated through
    # to the wrapper. This guards against a refactor that drops the env arg
    # (which would also drop HU_DPO_LENGTH_NORM for the dpop run).
    assert "env" in captured["kwargs"], (
        "early-stop wrapper invocation MUST include env=child_env "
        "(US-11.1 length-norm propagation)"
    )


# ────────────────────────────────────────────────────────────────────────
# Negative guard — non-dpop loss types do not get --delta emitted
# ────────────────────────────────────────────────────────────────────────
@pytest.mark.parametrize("loss_type", ["sigmoid", "ipo", "cpo"])
def test_no_delta_for_non_dpop_loss_types(tmp_path, monkeypatch, loss_type):
    """Only `dpop` consumes --delta in upstream mlx_lm_lora. Emitting --delta
    for sigmoid/ipo/cpo would either be silently ignored (best case) or
    rejected by upstream argparse (worst case). Either is a footgun. Pin it."""
    monkeypatch.setenv("HU_DPO_DETERMINISTIC", "1")
    monkeypatch.setattr(fg.Path, "home", classmethod(lambda cls: tmp_path))
    _stage_jsonl_dpo(tmp_path)
    args = _make_args(tmp_path, dpo_cpo_loss_type=loss_type)
    adapter_dir = pathlib.Path(args.adapter_path)

    fake_result = types.SimpleNamespace(returncode=0)
    with patch.object(fg.subprocess, "run", return_value=fake_result) as mock_run:
        with redirect_stdout(io.StringIO()):
            rc = fg.run_dpo(args, adapter_dir)

    assert rc == 0
    cmd = mock_run.call_args.args[0]
    _assert_adjacent(cmd, "--dpo-cpo-loss-type", loss_type)
    assert "--delta" not in cmd, (
        f"--delta MUST NOT be emitted for loss_type={loss_type!r}: {cmd}"
    )


# ────────────────────────────────────────────────────────────────────────
# Argparse surface — `python3 scripts/finetune-gemma.py --help` shows both flags
# ────────────────────────────────────────────────────────────────────────
def test_argparse_exposes_dpop_flags():
    """Smoke-test that the argparse parser in main() exposes the two flags.
    We do not invoke main() (which would call run_finetune); we re-build a
    parser-equivalent by parsing a no-op invocation through the same module."""
    # Import the module, build an args list that exercises the new flags,
    # and confirm argparse accepts them. We do not actually call main() —
    # we shell out to --help and assert exit 0 + help text.
    import subprocess

    result = subprocess.run(
        [sys.executable, str(_SCRIPT), "--help"],
        capture_output=True,
        text=True,
        timeout=30,
    )
    # --help exits 0
    assert result.returncode == 0, (
        f"--help exited {result.returncode}; stderr: {result.stderr!r}"
    )
    helptext = result.stdout
    assert "--dpo-cpo-loss-type" in helptext, (
        f"--help missing --dpo-cpo-loss-type:\n{helptext}"
    )
    assert "--dpop-delta" in helptext, f"--help missing --dpop-delta:\n{helptext}"
    # The default 0.1 is documented in help text (defense against silent
    # upstream-default-leak regression).
    assert "0.1" in helptext, (
        "Expected default delta '0.1' to appear in --help output (operators "
        f"need to see the Smaug-vs-upstream divergence):\n{helptext}"
    )
