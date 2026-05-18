#!/usr/bin/env python3
"""
Phase C5 verifier — pins the A/B eval harness contracts.

Tests four verdict paths through MetadataJudge:
  1. PASS — real LoRA candidate beats empty-tensors baseline
  2. NO-CHANGE — same adapter on both sides
  3. FAIL — candidate is empty-tensors stub (training didn't happen)
  4. REGRESS — candidate has smaller rank than baseline

Plus the format-inspection contracts:
  5. inspect_lora_binary parses a well-formed LoRA file
  6. inspect_safetensors parses a well-formed safetensors header
  7. Mixed adapters detect format crossover correctly

Run: python3 scripts/test_m3_eval_adapter.py
Exit 0 = all pass; 1 = at least one failure.
"""
from __future__ import annotations

import importlib.util
import json
import struct
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def _load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


eval_mod = _load_module("m3_eval_adapter", REPO_ROOT / "scripts" / "m3_eval_adapter.py")
train_mod = _load_module("training_loop", REPO_ROOT / "scripts" / "training_loop.py")


_PASS = 0
_FAIL = 0


def _ok(name: str, cond: bool, detail: str = "") -> None:
    global _PASS, _FAIL
    if cond:
        _PASS += 1
        print(f"  PASS  {name}")
    else:
        _FAIL += 1
        print(f"  FAIL  {name}  {detail}")


def _make_lora_binary(path: Path, rank: int = 4, layers: int = 5):
    """Write a minimal LORA-magic binary that inspect_lora_binary parses
    successfully. Tensor data is omitted — the inspector only reads the
    header, not the tensors."""
    with open(path, "wb") as f:
        f.write(b"LORA")
        f.write(struct.pack("<I", rank))
        f.write(struct.pack("<d", 1e-3))   # learning_rate
        f.write(struct.pack("<I", layers))
        f.write(b"\x00" * 64)              # placeholder tensor body


def _make_empty_safetensors(path: Path):
    """Use training_loop's helper to produce a real empty-tensors
    safetensors with no '__metadata__' tensor keys."""
    summary = train_mod.summarize_outcomes([
        {"t": 1, "l": 100, "pt": 10, "ct": 5, "m": 0, "a": 0, "g": 1, "k": 1},
    ])
    train_mod.write_dry_run_adapter(path, summary, 0, 1)


# ─────────────────────────────────────────────────────────────────────
# Tests
# ─────────────────────────────────────────────────────────────────────

def test_inspect_lora_binary_parses_header():
    print("\n--- test_inspect_lora_binary_parses_header ---")
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "adapter.bin"
        _make_lora_binary(p, rank=8, layers=12)
        meta = eval_mod.inspect_lora_binary(p)
        _ok("returned a meta dict", meta is not None)
        _ok("format=lora-bin", meta["format"] == "lora-bin")
        _ok("rank=8", meta["rank"] == 8)
        _ok("num_layers=12", meta["num_layers"] == 12)
        _ok("size_bytes is the file size", meta["size_bytes"] == p.stat().st_size)


def test_inspect_safetensors_parses_header():
    print("\n--- test_inspect_safetensors_parses_header ---")
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "adapter.safetensors"
        _make_empty_safetensors(p)
        meta = eval_mod.inspect_safetensors(p)
        _ok("returned a meta dict", meta is not None)
        _ok("format=safetensors", meta["format"] == "safetensors")
        _ok("tensor_count=0 (empty stub)", meta["tensor_count"] == 0)
        _ok("metadata block has outcome_count",
            meta["metadata"].get("outcome_count") == "1")


def test_pass_verdict_real_lora_vs_empty_stub():
    print("\n--- test_pass_verdict_real_lora_vs_empty_stub ---")
    with tempfile.TemporaryDirectory() as d:
        baseline = Path(d) / "baseline.safetensors"
        candidate = Path(d) / "candidate.bin"
        _make_empty_safetensors(baseline)
        _make_lora_binary(candidate, rank=4, layers=5)
        verdict = eval_mod.MetadataJudge().evaluate(
            eval_mod.inspect_adapter(baseline),
            eval_mod.inspect_adapter(candidate),
        )
        _ok("verdict=pass", verdict["verdict"] == "pass",
            f"got {verdict['verdict']}: {verdict['reason']}")


def test_no_change_verdict_same_adapter_both_sides():
    print("\n--- test_no_change_verdict_same_adapter_both_sides ---")
    with tempfile.TemporaryDirectory() as d:
        a = Path(d) / "a.bin"
        _make_lora_binary(a, rank=4, layers=5)
        verdict = eval_mod.MetadataJudge().evaluate(
            eval_mod.inspect_adapter(a),
            eval_mod.inspect_adapter(a),
        )
        _ok("verdict=no-change", verdict["verdict"] == "no-change",
            f"got {verdict['verdict']}: {verdict['reason']}")


def test_fail_verdict_candidate_is_empty_stub():
    print("\n--- test_fail_verdict_candidate_is_empty_stub ---")
    with tempfile.TemporaryDirectory() as d:
        baseline = Path(d) / "baseline.bin"
        candidate = Path(d) / "candidate.safetensors"
        _make_lora_binary(baseline, rank=4, layers=5)
        _make_empty_safetensors(candidate)
        verdict = eval_mod.MetadataJudge().evaluate(
            eval_mod.inspect_adapter(baseline),
            eval_mod.inspect_adapter(candidate),
        )
        _ok("verdict=fail (candidate didn't train)",
            verdict["verdict"] == "fail",
            f"got {verdict['verdict']}: {verdict['reason']}")


def test_regress_verdict_candidate_smaller_rank():
    print("\n--- test_regress_verdict_candidate_smaller_rank ---")
    with tempfile.TemporaryDirectory() as d:
        baseline = Path(d) / "baseline.bin"
        candidate = Path(d) / "candidate.bin"
        _make_lora_binary(baseline, rank=8, layers=12)
        _make_lora_binary(candidate, rank=4, layers=12)
        verdict = eval_mod.MetadataJudge().evaluate(
            eval_mod.inspect_adapter(baseline),
            eval_mod.inspect_adapter(candidate),
        )
        _ok("verdict=regress (rank dropped 8→4)",
            verdict["verdict"] == "regress",
            f"got {verdict['verdict']}: {verdict['reason']}")


def test_missing_file_returns_none():
    print("\n--- test_missing_file_returns_none ---")
    with tempfile.TemporaryDirectory() as d:
        meta = eval_mod.inspect_adapter(Path(d) / "nonexistent.bin")
        _ok("missing file → None", meta is None)


def main():
    print("M3 adapter A/B eval (C5) verifier")
    test_inspect_lora_binary_parses_header()
    test_inspect_safetensors_parses_header()
    test_pass_verdict_real_lora_vs_empty_stub()
    test_no_change_verdict_same_adapter_both_sides()
    test_fail_verdict_candidate_is_empty_stub()
    test_regress_verdict_candidate_smaller_rank()
    test_missing_file_returns_none()
    print(f"\n--- Results: {_PASS} passed, {_FAIL} failed ---")
    return 0 if _FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
