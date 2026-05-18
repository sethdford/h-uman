#!/usr/bin/env python3
"""
Phase E2 verifier — pins the MLX-lm.lora bridge's wire.

Tests the parts that don't require actually running mlx_lm.lora:
  1. detect_format correctly identifies alpaca-dpo, chat-completion-log,
     and unknown shapes
  2. convert_to_mlx_lm_jsonl passes alpaca-dpo unchanged
  3. convert_to_mlx_lm_jsonl wraps chat-completion-log in gemma chat
     template
  4. write_stub_adapter produces a parseable safetensors file with
     the right metadata block when mlx_lm is unavailable

Run: python3 scripts/test_m3_mlx_lora_bridge.py
"""
from __future__ import annotations

import importlib.util
import json
import struct
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BRIDGE = REPO_ROOT / "scripts" / "m3_mlx_lora_bridge.py"


def _load():
    spec = importlib.util.spec_from_file_location("m3_mlx_lora_bridge", BRIDGE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


b = _load()

_PASS = 0
_FAIL = 0


def _ok(name: str, cond: bool, detail: str = ""):
    global _PASS, _FAIL
    if cond:
        _PASS += 1
        print(f"  PASS  {name}")
    else:
        _FAIL += 1
        print(f"  FAIL  {name}  {detail}")


def test_detect_format_alpaca_dpo():
    print("\n--- test_detect_format_alpaca_dpo ---")
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "a.jsonl"
        p.write_text(
            '{"prompt": "hi", "chosen": "hey", "rejected": "as an AI"}\n'
        )
        _ok("detect alpaca-dpo", b.detect_format(p) == "alpaca-dpo")


def test_detect_format_chat_log():
    print("\n--- test_detect_format_chat_log ---")
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "c.jsonl"
        p.write_text('{"prompt": "yo", "response": "hey"}\n')
        _ok("detect chat-completion-log",
            b.detect_format(p) == "chat-completion-log")


def test_detect_format_unknown():
    print("\n--- test_detect_format_unknown ---")
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "u.jsonl"
        p.write_text('{"random": "fields"}\n')
        _ok("detect unknown", b.detect_format(p) == "unknown")


def test_detect_format_missing_file():
    print("\n--- test_detect_format_missing_file ---")
    _ok("missing file → unknown",
        b.detect_format(Path("/tmp/nonexistent_e2_test.jsonl")) == "unknown")


def test_convert_alpaca_dpo_pass_through():
    print("\n--- test_convert_alpaca_dpo_pass_through ---")
    with tempfile.TemporaryDirectory() as d:
        src = Path(d) / "src.jsonl"
        dst = Path(d) / "dst.jsonl"
        src.write_text(
            '{"prompt": "hi", "chosen": "hey", "rejected": "no"}\n'
            '{"prompt": "yo", "chosen": "hi", "rejected": "as AI"}\n'
        )
        n = b.convert_to_mlx_lm_jsonl(src, dst, "alpaca-dpo")
        _ok("converted 2 records", n == 2)
        lines = dst.read_text().splitlines()
        rec0 = json.loads(lines[0])
        _ok("preserves prompt/chosen/rejected",
            rec0.get("chosen") == "hey" and rec0.get("rejected") == "no")


def test_convert_chat_log_wraps_in_template():
    print("\n--- test_convert_chat_log_wraps_in_template ---")
    with tempfile.TemporaryDirectory() as d:
        src = Path(d) / "src.jsonl"
        dst = Path(d) / "dst.jsonl"
        src.write_text(
            '{"prompt": "morning", "response": "morning :)"}\n'
        )
        n = b.convert_to_mlx_lm_jsonl(src, dst, "chat-completion-log")
        _ok("converted 1 record", n == 1)
        rec = json.loads(dst.read_text().splitlines()[0])
        _ok("has 'text' field (mlx-lm SFT shape)", "text" in rec)
        _ok("contains start_of_turn user marker",
            "<start_of_turn>user" in rec.get("text", ""))
        _ok("contains the response text", "morning :)" in rec.get("text", ""))


def test_write_stub_adapter_parseable():
    print("\n--- test_write_stub_adapter_parseable ---")
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "stub.safetensors"
        b.write_stub_adapter(out, "mlx_lm not installed (test)", 5)
        _ok("file exists", out.exists())
        with open(out, "rb") as f:
            header_len = struct.unpack("<Q", f.read(8))[0]
            header = json.loads(f.read(header_len).decode("utf-8"))
        meta = header.get("__metadata__", {})
        _ok("has metadata block", "__metadata__" in header)
        _ok("reason recorded", "not installed" in meta.get("reason", ""))
        _ok("pairs_count recorded", meta.get("pairs_count") == "5")
        _ok("fix recorded (operator hint)",
            "mlx-lm" in meta.get("fix", ""))


def test_have_mlx_lm_returns_bool():
    print("\n--- test_have_mlx_lm_returns_bool ---")
    result = b._have_mlx_lm()
    _ok("returns a bool", isinstance(result, bool))


def main():
    print("M3 MLX-lm.lora bridge (E2) verifier")
    test_detect_format_alpaca_dpo()
    test_detect_format_chat_log()
    test_detect_format_unknown()
    test_detect_format_missing_file()
    test_convert_alpaca_dpo_pass_through()
    test_convert_chat_log_wraps_in_template()
    test_write_stub_adapter_parseable()
    test_have_mlx_lm_returns_bool()
    print(f"\n--- Results: {_PASS} passed, {_FAIL} failed ---")
    return 0 if _FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
