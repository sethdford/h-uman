#!/usr/bin/env python3
"""
M3 GCE remote ORPO trainer (2026-06-05) — runs INSIDE the GPU VM.

Sibling of m3_gce_train_remote.py, but trains **true ORPO** (odds-ratio
preference optimization, reference-free) instead of SFT-on-chosen. ORPO uses
BOTH `chosen` and `rejected`, so its loss actively PENALIZES the rejected
deliberation preamble — the root-cause fix for the served gemma-4-31b +
seth-lora-v4-repair adapter, which front-loads ~120-340 tokens of markdown-
bullet deliberation before the terse reply (and ignores the no-think
instruction). SFT-on-chosen only rewards brevity; it does not punish the
preamble.

Reference-free is what lets a 31B fit on one 80 GB A100: no frozen reference
model copy in VRAM (DPO would need one). QLoRA (bitsandbytes 4-bit) + a small
LoRA keeps the trainable footprint tiny.

Why HuggingFace+TRL not MLX: mlx_lm only runs on Apple Silicon; a CUDA GCE VM
needs the transformers/peft/trl stack. The output LoRA safetensors has the same
tensor names as mlx_lm.lora's output, so the daemon's /v1/adapters/swap and the
eval scripts don't care which backend produced it.

Input (--pairs JSONL): one {"prompt","chosen","rejected"} object per line, as
produced by scripts/build_orpo_deliberation_set.py.

Usage (called by m3_gce_train.sh --objective orpo, not directly):
    python3 m3_gce_orpo_remote.py \\
        --pairs /tmp/pairs.jsonl --adapter-out /tmp/adapter \\
        --base-model google/gemma-4-31b-it \\
        --iters 300 --rank 16 --batch-size 1 --learning-rate 5e-6 --beta 0.1

Exit codes: 0 saved | 2 pre-flight failure | 3 training error
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path


def _pip_install_if_missing(*packages: str) -> None:
    missing = []
    for p in packages:
        mod = p.split("[")[0].split(">")[0].split("=")[0].replace("-", "_")
        try:
            __import__(mod)
        except ImportError:
            missing.append(p)
    if missing:
        print(f"  Installing missing deps: {missing}")
        subprocess.check_call([sys.executable, "-m", "pip", "install", "--quiet", *missing])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pairs", required=True)
    ap.add_argument("--adapter-out", required=True)
    # NOTE casing: the canonical gated repo is gemma-4-31B-it (capital B); the
    # served mlx-community/gemma-4-31b-it-8bit declares this as its base_model.
    ap.add_argument("--base-model", default="google/gemma-4-31B-it")
    ap.add_argument("--iters", type=int, default=300, help="max optimizer steps")
    ap.add_argument("--rank", type=int, default=16)
    ap.add_argument("--batch-size", type=int, default=1)
    ap.add_argument("--grad-accum", type=int, default=8)
    ap.add_argument("--learning-rate", type=float, default=5e-6)
    ap.add_argument("--beta", type=float, default=0.1, help="ORPO lambda (OR weight)")
    ap.add_argument("--max-seq-length", type=int, default=1024)
    ap.add_argument("--max-prompt-length", type=int, default=512)
    args = ap.parse_args()

    print(f"\n{'='*60}\n  M3 GCE REMOTE ORPO TRAINER\n{'='*60}")
    print(f"  Pairs:       {args.pairs}")
    print(f"  Adapter out: {args.adapter_out}")
    print(f"  Base model:  {args.base_model}")
    print(f"  Steps/rank:  {args.iters}/{args.rank}  beta={args.beta}  lr={args.learning_rate}")
    print(f"  Python:      {sys.executable}")

    _pip_install_if_missing(
        "torch", "transformers", "peft", "accelerate", "bitsandbytes",
        "datasets", "safetensors", "trl>=0.13",
    )

    import torch
    from transformers import AutoModelForCausalLM, AutoTokenizer, BitsAndBytesConfig
    from peft import LoraConfig
    from datasets import Dataset
    from trl import ORPOConfig, ORPOTrainer

    if not torch.cuda.is_available():
        print("  ERROR: CUDA not available on this VM", file=sys.stderr)
        return 2
    vram_gb = torch.cuda.get_device_properties(0).total_memory / 1e9
    print(f"  CUDA:        {torch.cuda.get_device_name(0)} ({vram_gb:.1f} GB)")

    # --- Load preference pairs -> ORPO columns (prompt/chosen/rejected). TRL
    # concatenates prompt+chosen and prompt+rejected internally; we supply only
    # the Gemma chat-template scaffold so the boundary matches inference. ---
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
            prompt, chosen, rejected = r.get("prompt"), r.get("chosen"), r.get("rejected")
            if not (prompt and chosen and rejected):
                continue
            records.append({
                "prompt": f"<start_of_turn>user\n{prompt}<end_of_turn>\n<start_of_turn>model\n",
                "chosen": f"{chosen}<end_of_turn>",
                "rejected": f"{rejected}<end_of_turn>",
            })
    print(f"  Loaded {len(records)} ORPO pairs")
    if not records:
        print("  ERROR: no usable pairs", file=sys.stderr)
        return 2
    ds = Dataset.from_list(records)

    print(f"\n  Loading tokenizer for {args.base_model} ...")
    tokenizer = AutoTokenizer.from_pretrained(args.base_model, trust_remote_code=True)
    if tokenizer.pad_token is None:
        tokenizer.pad_token = tokenizer.eos_token

    use_4bit = vram_gb < 90  # 31B QLoRA fits one 80GB A100; bf16 would not
    print(f"\n  Loading base model (4-bit={use_4bit}) ...")
    model_kwargs = dict(device_map="auto", trust_remote_code=True)
    if use_4bit:
        model_kwargs["quantization_config"] = BitsAndBytesConfig(
            load_in_4bit=True, bnb_4bit_quant_type="nf4",
            bnb_4bit_compute_dtype=torch.bfloat16,
        )
    else:
        model_kwargs["torch_dtype"] = torch.bfloat16

    # gemma-4-31B-it is multimodal (Gemma4ForConditionalGeneration,
    # image-text-to-text), so AutoModelForCausalLM usually REFUSES it. Load via
    # the image-text-to-text auto class; PEFT then attaches the LoRA to the
    # language tower's q/k/v/o_proj by name suffix, and text-only ORPO batches
    # route through the text path (pixel_values are optional). Fall back to
    # CausalLM for text-only base variants. The first remote run validates this.
    model = None
    try:
        from transformers import AutoModelForImageTextToText
        model = AutoModelForImageTextToText.from_pretrained(args.base_model, **model_kwargs)
        print("  Loaded via AutoModelForImageTextToText (multimodal gemma4)")
    except Exception as e:
        print(f"  AutoModelForImageTextToText unavailable/failed ({type(e).__name__}: {e});"
              f" falling back to AutoModelForCausalLM")
        model = AutoModelForCausalLM.from_pretrained(args.base_model, **model_kwargs)
        print("  Loaded via AutoModelForCausalLM")

    # LoRA targets mirror mlx_lm.lora so the adapter loads into the served model.
    peft_config = LoraConfig(
        r=args.rank, lora_alpha=args.rank * 2,
        target_modules=["q_proj", "k_proj", "v_proj", "o_proj"],
        lora_dropout=0.05, bias="none", task_type="CAUSAL_LM",
    )

    out_dir = Path(args.adapter_out)
    out_dir.mkdir(parents=True, exist_ok=True)
    orpo_cfg = ORPOConfig(
        output_dir=str(out_dir),
        max_steps=args.iters,
        per_device_train_batch_size=args.batch_size,
        gradient_accumulation_steps=args.grad_accum,
        learning_rate=args.learning_rate,
        beta=args.beta,
        max_length=args.max_seq_length,
        max_prompt_length=args.max_prompt_length,
        warmup_steps=max(1, args.iters // 10),
        logging_steps=max(1, args.iters // 20),
        save_strategy="no",
        bf16=True,
        report_to="none",
        remove_unused_columns=False,
    )

    # TRL renamed tokenizer-> processing_class around v0.12; construct defensively.
    try:
        trainer = ORPOTrainer(model=model, args=orpo_cfg, train_dataset=ds,
                              processing_class=tokenizer, peft_config=peft_config)
    except TypeError:
        trainer = ORPOTrainer(model=model, args=orpo_cfg, train_dataset=ds,
                              tokenizer=tokenizer, peft_config=peft_config)

    print(f"\n  ORPO training: {args.iters} steps, rank={args.rank}, "
          f"beta={args.beta}, effective batch={args.batch_size * args.grad_accum}")
    start = time.time()
    try:
        trainer.train()
    except Exception as e:
        print(f"  ERROR: training raised {type(e).__name__}: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return 3
    print(f"\n  Training done in {time.time() - start:.1f}s")

    print(f"\n  Saving adapter to {out_dir}/ ...")
    trainer.save_model(str(out_dir))
    for f in sorted(out_dir.iterdir()):
        print(f"    {f.name}: {f.stat().st_size:>12} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
