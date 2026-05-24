#!/usr/bin/env python3
"""
M3 GCE remote trainer (2026-05-19) — runs INSIDE the GPU VM.

This script is uploaded by scripts/m3_gce_train.sh and executed via
gcloud compute ssh. It:
  1. Loads the base model (HuggingFace transformers + bitsandbytes 4-bit
     quantization if VRAM is tight)
  2. Wraps it in a PEFT LoRA adapter (rank=R, alpha=2R)
  3. Trains SFT on `chosen` completions from the Alpaca-DPO pairs
     (mirrors what m3_mlx_lora_bridge.py does locally, but in PyTorch)
  4. Saves the LoRA safetensors to --adapter-out

Why HuggingFace not MLX: mlx_lm only runs on Apple Silicon. Any GCE
VM (CUDA) needs the standard transformers/peft stack. The output
safetensors has the SAME shape and tensor names as mlx_lm.lora's
output, so downstream eval scripts (m3_eval_adapter.py + the daemon's
/v1/adapters/swap) don't care which backend produced it.

The Deep Learning AMI ships with: torch, transformers, peft, accelerate,
bitsandbytes, datasets. If any are missing we pip-install at startup.

Usage (called by m3_gce_train.sh, not directly):
    python3 m3_gce_train_remote.py \\
        --pairs /tmp/pairs.jsonl \\
        --adapter-out /tmp/adapter \\
        --base-model google/gemma-3-4b-it \\
        --iters 50 --rank 8 --batch-size 1 --learning-rate 5e-5

Exit codes:
  0 — adapter saved
  2 — pre-flight (deps, data, model) failure
  3 — training error
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path


def _pip_install_if_missing(*packages: str) -> None:
    """Best-effort install of any missing deps. The DL AMI usually
    has all of these, but we hedge in case the family drifts."""
    missing = []
    for p in packages:
        mod = p.split("[")[0].replace("-", "_")
        try:
            __import__(mod)
        except ImportError:
            missing.append(p)
    if missing:
        print(f"  Installing missing deps: {missing}")
        subprocess.check_call([sys.executable, "-m", "pip", "install",
                                "--quiet", *missing])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pairs", required=True)
    ap.add_argument("--adapter-out", required=True)
    ap.add_argument("--base-model", default="google/gemma-3-4b-it")
    ap.add_argument("--iters", type=int, default=50)
    ap.add_argument("--rank", type=int, default=8)
    ap.add_argument("--batch-size", type=int, default=1)
    ap.add_argument("--learning-rate", type=float, default=5e-5)
    ap.add_argument("--max-seq-length", type=int, default=512)
    args = ap.parse_args()

    print(f"\n{'='*60}")
    print(f"  M3 GCE REMOTE TRAINER")
    print(f"{'='*60}")
    print(f"  Pairs:       {args.pairs}")
    print(f"  Adapter out: {args.adapter_out}")
    print(f"  Base model:  {args.base_model}")
    print(f"  Iters/rank:  {args.iters}/{args.rank}")
    print(f"  Python:      {sys.executable}")

    # Pre-flight: ensure deps are present
    _pip_install_if_missing(
        "torch", "transformers", "peft", "accelerate",
        "bitsandbytes", "datasets", "safetensors",
    )

    import torch
    from transformers import (
        AutoModelForCausalLM, AutoTokenizer, BitsAndBytesConfig,
        TrainingArguments, Trainer, DataCollatorForLanguageModeling,
    )
    from peft import LoraConfig, get_peft_model, TaskType
    from datasets import Dataset

    # CUDA sanity
    if not torch.cuda.is_available():
        print("  ERROR: CUDA not available on this VM", file=sys.stderr)
        return 2
    print(f"  CUDA:        {torch.cuda.get_device_name(0)} "
          f"({torch.cuda.get_device_properties(0).total_memory / 1e9:.1f} GB)")

    # Load Alpaca-DPO pairs → flatten to {text} for SFT-on-chosen.
    # Mirrors the same convert step in m3_mlx_lora_bridge.py so the
    # training signal is identical between local and cloud runs.
    print(f"\n  Loading pairs from {args.pairs} ...")
    records = []
    with open(args.pairs) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            prompt = r.get("prompt") or ""
            chosen = r.get("chosen") or ""
            if not prompt or not chosen:
                continue
            # Gemma chat template
            text = (f"<start_of_turn>user\n{prompt}<end_of_turn>\n"
                    f"<start_of_turn>model\n{chosen}<end_of_turn>")
            records.append({"text": text})
    print(f"  Loaded {len(records)} training records")
    if not records:
        print("  ERROR: no usable records", file=sys.stderr)
        return 2

    # Tokenize
    print(f"\n  Loading tokenizer for {args.base_model} ...")
    tokenizer = AutoTokenizer.from_pretrained(args.base_model,
                                                trust_remote_code=True)
    if tokenizer.pad_token is None:
        tokenizer.pad_token = tokenizer.eos_token

    def tokenize(example):
        return tokenizer(example["text"], truncation=True,
                         max_length=args.max_seq_length,
                         padding="max_length")

    ds = Dataset.from_list(records).map(tokenize, batched=False,
                                          remove_columns=["text"])

    # Load base model — use 4-bit if VRAM is < 30 GB (L4 = 24 GB)
    vram_gb = torch.cuda.get_device_properties(0).total_memory / 1e9
    use_4bit = vram_gb < 30
    print(f"\n  Loading base model (4-bit={use_4bit}, VRAM={vram_gb:.1f} GB) ...")
    if use_4bit:
        bnb_config = BitsAndBytesConfig(
            load_in_4bit=True,
            bnb_4bit_quant_type="nf4",
            bnb_4bit_compute_dtype=torch.bfloat16,
        )
        model = AutoModelForCausalLM.from_pretrained(
            args.base_model,
            quantization_config=bnb_config,
            device_map="auto",
            trust_remote_code=True,
        )
    else:
        model = AutoModelForCausalLM.from_pretrained(
            args.base_model,
            torch_dtype=torch.bfloat16,
            device_map="auto",
            trust_remote_code=True,
        )

    # Apply LoRA — same modules as mlx_lm.lora targets
    lora_config = LoraConfig(
        r=args.rank,
        lora_alpha=args.rank * 2,
        target_modules=["q_proj", "k_proj", "v_proj", "o_proj"],
        lora_dropout=0.05,
        bias="none",
        task_type=TaskType.CAUSAL_LM,
    )
    model = get_peft_model(model, lora_config)
    model.print_trainable_parameters()

    # Train
    out_dir = Path(args.adapter_out)
    out_dir.mkdir(parents=True, exist_ok=True)
    targs = TrainingArguments(
        output_dir=str(out_dir),
        num_train_epochs=1,
        max_steps=args.iters,
        per_device_train_batch_size=args.batch_size,
        gradient_accumulation_steps=1,
        learning_rate=args.learning_rate,
        warmup_steps=max(1, args.iters // 10),
        logging_steps=max(1, args.iters // 10),
        save_strategy="no",
        bf16=True,
        report_to="none",
        remove_unused_columns=False,
    )
    trainer = Trainer(
        model=model,
        args=targs,
        train_dataset=ds,
        data_collator=DataCollatorForLanguageModeling(
            tokenizer=tokenizer, mlm=False),
    )

    print(f"\n  Training: {args.iters} iters, rank={args.rank}, "
          f"batch_size={args.batch_size}, lr={args.learning_rate}")
    start = time.time()
    try:
        trainer.train()
    except Exception as e:
        print(f"  ERROR: training raised {type(e).__name__}: {e}",
              file=sys.stderr)
        import traceback
        traceback.print_exc()
        return 3
    elapsed = time.time() - start
    print(f"\n  Training done in {elapsed:.1f}s")

    # Save the LoRA adapter (just the adapter, not the full model)
    print(f"\n  Saving adapter to {out_dir}/ ...")
    model.save_pretrained(str(out_dir))
    # List what's there
    for f in sorted(out_dir.iterdir()):
        size = f.stat().st_size
        print(f"    {f.name}: {size:>12} bytes")

    return 0


if __name__ == "__main__":
    sys.exit(main())
