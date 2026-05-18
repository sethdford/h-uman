#!/usr/bin/env python3
"""
Phase E2 (2026-05-18) — MLX-lm.lora bridge.

Closes (the wire portion of) THE big honest gap from the audit: the
LoRA we train now actually TARGETS the served gemma-4-26b model, not
the reference HUML GPT.

How it works:
  1. Caller provides a JSONL of training pairs (typically Alpaca-DPO
     format from m3_dpo_from_rewrites.py, OR a chat-completion log
     export). We auto-detect and convert to the format mlx-lm expects.
  2. We invoke `mlx_lm.lora` as a subprocess when it's importable.
     This trains a real LoRA against the served model (4-bit gemma)
     and writes safetensors with q_proj/k_proj/v_proj/o_proj lora_A
     and lora_B tensors — exactly the format /v1/adapters/swap loads.
  3. When mlx_lm is NOT installed, we fall back to writing an
     empty-tensors safetensors with a clear metadata block saying
     "install mlx-lm to train against gemma."

That's the structural fix. The LoRA path now targets a model that
actually serves user-visible inference.

This script is intentionally separate from training_loop.py's
lora-persona path. Both live in the codebase:
  - training_loop.py --source-jsonl → lora-persona (reference HUML GPT)
  - m3_mlx_lora_bridge.py            → mlx-lm.lora (served gemma)

Eventually m3_outcome_driver.py will choose between them based on
config (or run both and let C5's A/B eval pick).

Usage:
    python3 scripts/m3_mlx_lora_bridge.py \\
        --pairs ~/.human/training-data/m3-alpaca-dpo.jsonl \\
        --model mlx-community/gemma-4-26b-a4b-it-4bit \\
        --adapter-out ~/.human/training-data/adapters/gemma-lora-v1 \\
        --rank 16 --iters 200

Exit codes:
    0 — adapter produced (real if mlx_lm available, stub otherwise)
    2 — input parse failure / arg validation
    3 — mlx_lm available but training itself failed
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def _have_mlx_lm() -> bool:
    """True when `mlx_lm` is importable in the current Python.
    Detection is per-invocation (cheap; not cached) so an operator
    who installs mlx-lm mid-session doesn't need to restart."""
    try:
        import mlx_lm  # noqa: F401
        return True
    except ImportError:
        return False


def detect_format(jsonl_path: Path) -> str:
    """Inspect the first non-blank record and decide which format
    we have. Returns one of:
      - 'alpaca-dpo'  → {prompt, chosen, rejected}
      - 'chat-completion-log' → {prompt, response} or similar
      - 'unknown'

    Detection by keys, not content. The two formats round-trip into
    mlx-lm differently — alpaca-dpo is a DPO trainer input,
    chat-completion-log feeds SFT."""
    if not jsonl_path.exists():
        return "unknown"
    with open(jsonl_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if {"prompt", "chosen", "rejected"} <= set(rec):
                return "alpaca-dpo"
            if "messages" in rec or ("prompt" in rec and "response" in rec):
                return "chat-completion-log"
            return "unknown"
    return "unknown"


def convert_to_mlx_lm_jsonl(src: Path, dst: Path, src_format: str) -> int:
    """Convert one of our formats into mlx-lm's expected JSONL.

    mlx-lm.lora expects: `{"text": "..."}` per line for SFT, OR
                          `{"prompt": "...", "chosen": "...", "rejected": "..."}`
                          for DPO (matching our alpaca-dpo).

    Alpaca-DPO passes through unchanged.
    Chat-completion logs concatenate (prompt, response) into the
    `text` field with the chat template's role markers."""
    n = 0
    with open(src) as fin, open(dst, "w") as fout:
        for line in fin:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if src_format == "alpaca-dpo":
                fout.write(json.dumps(rec) + "\n")
                n += 1
            elif src_format == "chat-completion-log":
                prompt = rec.get("prompt") or ""
                response = rec.get("response") or ""
                if not prompt or not response:
                    continue
                # Gemma chat template (matches mlx-community/gemma-4-26b-a4b-it).
                # mlx-lm trains on the raw concatenated text; the template
                # markers signal where the model should learn to respond.
                text = (f"<start_of_turn>user\n{prompt}<end_of_turn>\n"
                        f"<start_of_turn>model\n{response}<end_of_turn>")
                fout.write(json.dumps({"text": text}) + "\n")
                n += 1
    return n


def write_stub_adapter(adapter_out: Path, reason: str, pairs_count: int) -> None:
    """Empty-tensors safetensors when mlx_lm isn't available. Same
    shape as training_loop.write_dry_run_adapter — but with a
    metadata block that specifically says "install mlx-lm to enable
    real training against the served model." """
    adapter_out.parent.mkdir(parents=True, exist_ok=True)
    header = {
        "__metadata__": {
            "format": "m3-mlx-bridge-stub-v1",
            "produced_by": "scripts/m3_mlx_lora_bridge.py",
            "reason": reason,
            "fix": "pip install mlx-lm (Apple Silicon required)",
            "pairs_count": str(pairs_count),
            "produced_at": str(int(time.time())),
        }
    }
    header_bytes = json.dumps(header).encode("utf-8")
    with open(adapter_out, "wb") as f:
        f.write(len(header_bytes).to_bytes(8, "little"))
        f.write(header_bytes)


def invoke_mlx_lm_lora(model: str, train_jsonl: Path, adapter_out: Path,
                       rank: int, iters: int, learning_rate: float,
                       batch_size: int, mode: str) -> int:
    """Subprocess `python -m mlx_lm.lora ...`. mlx-lm's CLI is the
    canonical entry point; we don't import its internals because
    they shift between minor versions.

    `mode` is 'lora' (SFT) or 'dpo' (preference) — picked from the
    detected JSONL format."""
    # mlx-lm's CLI surface (as of mlx-lm 0.20+): train mode args are
    #   --model <hf-id> --train --data <dir> --adapter-path <out>
    #   --batch-size --iters --learning-rate --lora-layers --num-layers
    # The --data flag is a DIRECTORY containing train.jsonl + valid.jsonl;
    # we lay out a temp dir that meets that shape.
    with tempfile.TemporaryDirectory() as td:
        data_dir = Path(td)
        # mlx-lm needs both train.jsonl and valid.jsonl (will error
        # if valid is missing). For warmup we split 90/10.
        all_lines = train_jsonl.read_text().splitlines()
        all_lines = [l for l in all_lines if l.strip()]
        cut = max(1, int(len(all_lines) * 0.9))
        (data_dir / "train.jsonl").write_text("\n".join(all_lines[:cut]) + "\n")
        (data_dir / "valid.jsonl").write_text("\n".join(all_lines[cut:]) + "\n")

        adapter_out.parent.mkdir(parents=True, exist_ok=True)
        cmd = [
            sys.executable, "-m", "mlx_lm.lora",
            "--model", model,
            "--train",
            "--data", str(data_dir),
            "--adapter-path", str(adapter_out),
            "--batch-size", str(batch_size),
            "--iters", str(iters),
            "--learning-rate", f"{learning_rate:g}",
            "--lora-layers", str(rank),
        ]
        if mode == "dpo":
            # mlx-lm 0.21+ added a --dpo flag; older versions ignore it
            # and train SFT. Either way we get a real adapter.
            cmd.append("--dpo")
        print(f"  Invoking: {' '.join(cmd)}")
        return subprocess.call(cmd)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pairs", type=Path, required=True,
                    help="JSONL of training pairs (alpaca-dpo or chat-completion log)")
    ap.add_argument("--model", default=os.environ.get(
                        "HUMAN_MLX_MODEL", "mlx-community/gemma-4-26b-a4b-it-4bit"),
                    help="MLX model ID (default %(default)s)")
    ap.add_argument("--adapter-out", type=Path, required=True,
                    help="Path to write the produced LoRA safetensors")
    ap.add_argument("--rank", type=int, default=16,
                    help="LoRA rank (default 16; SOTA persona-tune range is 16-64)")
    ap.add_argument("--iters", type=int, default=200,
                    help="Training iterations (default 200)")
    ap.add_argument("--learning-rate", type=float, default=1e-4,
                    help="LR (default 1e-4; lower than C3 reference-GPT because "
                         "we're fine-tuning a pretrained model, not training fresh)")
    ap.add_argument("--batch-size", type=int, default=4)
    ap.add_argument("--check-only", action="store_true",
                    help="Don't train; just report whether mlx_lm is installed "
                         "and what the input JSONL format looks like")
    args = ap.parse_args()

    print(f"\n{'='*60}")
    print(f"  M3 MLX-LM LORA BRIDGE (E2)")
    print(f"{'='*60}")
    print(f"  Pairs:    {args.pairs}")
    print(f"  Model:    {args.model}")
    print(f"  Adapter:  {args.adapter_out}")

    if not args.pairs.exists():
        print(f"  ERROR: pairs JSONL not found at {args.pairs}", file=sys.stderr)
        return 2

    src_format = detect_format(args.pairs)
    print(f"  Format:   {src_format}")
    if src_format == "unknown":
        print(f"  ERROR: could not detect format of {args.pairs.name}. "
              f"Expected alpaca-dpo or chat-completion-log keys.", file=sys.stderr)
        return 2

    has_mlx = _have_mlx_lm()
    print(f"  mlx_lm:   {'installed' if has_mlx else 'NOT installed'}")

    if args.check_only:
        print(f"{'='*60}")
        print(f"  Check-only mode — no training run.")
        return 0

    if not has_mlx:
        print(f"  Producing STUB adapter (mlx_lm unavailable).")
        write_stub_adapter(args.adapter_out, "mlx_lm not installed",
                           sum(1 for _ in open(args.pairs)))
        print(f"  Stub adapter: {args.adapter_out} "
              f"({args.adapter_out.stat().st_size} bytes)")
        print(f"  To enable real training: pip install mlx-lm "
              f"(Apple Silicon required)")
        return 0

    # mlx-lm path: convert format and invoke
    with tempfile.NamedTemporaryFile(suffix=".jsonl", delete=False) as tmpf:
        train_jsonl = Path(tmpf.name)
    n = convert_to_mlx_lm_jsonl(args.pairs, train_jsonl, src_format)
    print(f"  Converted {n} records → {train_jsonl}")
    if n < 2:
        print(f"  ERROR: fewer than 2 records — not enough for train/valid split",
              file=sys.stderr)
        train_jsonl.unlink()
        return 2

    mode = "dpo" if src_format == "alpaca-dpo" else "lora"
    rc = invoke_mlx_lm_lora(args.model, train_jsonl, args.adapter_out,
                            args.rank, args.iters, args.learning_rate,
                            args.batch_size, mode)
    train_jsonl.unlink()
    if rc != 0:
        print(f"  mlx_lm.lora exited with rc={rc}", file=sys.stderr)
        return 3
    print(f"  Adapter written: {args.adapter_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
