#!/usr/bin/env python3
# scripts/dpo_mlx_train.py
#
# Phase 2 (RL SOTA): wrapper around mlx-lm-lora's DPO trainer. Called via
# popen from src/ml/dpo_real_mlx.c. We wrap rather than invoke a CLI because
# (a) the third-party mlx-lm-lora package exposes train_dpo programmatically,
# not as a python -m entrypoint; (b) wrapping lets us print structured progress
# (loss, iter) to stdout in a format the C side can parse.
"""
Usage:
    dpo_mlx_train.py --model <hf_id> --data <jsonl_path> --adapter-path <dir>
                     --iters <N> --beta <beta> [--batch-size <B>]
"""
import argparse
import json
import sys
from pathlib import Path

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True, help="HF model id, e.g. mlx-community/gemma-3-4b-it-bf16")
    ap.add_argument("--data", required=True, help="Path to JSONL preference pairs")
    ap.add_argument("--adapter-path", required=True, help="Output directory for adapters.safetensors")
    ap.add_argument("--iters", type=int, default=100)
    ap.add_argument("--beta", type=float, default=0.1)
    ap.add_argument("--batch-size", type=int, default=1)
    args = ap.parse_args()

    try:
        from mlx_lm_lora.trainer.dpo_trainer import train_dpo, DPOTrainingArgs
        from mlx_lm_lora.utils import PreferenceDataset
        from mlx_lm.utils import load
    except ImportError as e:
        print(f"[dpo_mlx_train] ERROR: mlx-lm-lora package not available: {e}", file=sys.stderr)
        print("[dpo_mlx_train] Install with: pip install mlx-lm-lora", file=sys.stderr)
        sys.exit(2)

    Path(args.adapter_path).mkdir(parents=True, exist_ok=True)

    print(f"[dpo_mlx_train] loading model {args.model}", flush=True)
    model, tokenizer = load(args.model)

    print(f"[dpo_mlx_train] loading preferences from {args.data}", flush=True)
    dataset = PreferenceDataset(args.data, tokenizer)

    train_args = DPOTrainingArgs(
        iters=args.iters,
        batch_size=args.batch_size,
        beta=args.beta,
        adapter_path=args.adapter_path,
    )

    print(f"[dpo_mlx_train] starting DPO: iters={args.iters} beta={args.beta}", flush=True)
    train_dpo(model=model, tokenizer=tokenizer, dataset=dataset, args=train_args)

    safetensors = Path(args.adapter_path) / "adapters.safetensors"
    if not safetensors.exists() or safetensors.stat().st_size == 0:
        print(f"[dpo_mlx_train] ERROR: expected output {safetensors} missing or empty", file=sys.stderr)
        sys.exit(3)
    print(f"[dpo_mlx_train] DONE — adapter written to {safetensors} ({safetensors.stat().st_size} bytes)", flush=True)
    return 0

if __name__ == "__main__":
    sys.exit(main())
