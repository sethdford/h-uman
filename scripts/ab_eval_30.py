#!/usr/bin/env python3
"""
30-prompt A/B eval between two adapters.
Generates with each adapter on held-out prompts; writes JSON arrays for `human ml lora-ab`.

Usage:
  python3 scripts/ab_eval_30.py <sft_dir> <dpo_dir>

Outputs:
  /tmp/ab30_sft.json
  /tmp/ab30_dpo.json
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

BASE = "mlx-community/gemma-4-e2b-it-4bit"


def load_prompts(valid_jsonl: Path, n: int = 30) -> list[str]:
    prompts: list[str] = []
    with open(valid_jsonl, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.strip():
                continue
            try:
                obj = json.loads(line)
            except Exception:
                continue
            if "prompt" not in obj:
                continue
            p = obj["prompt"].strip()
            if 5 <= len(p) <= 400:
                prompts.append(p[:300])
            if len(prompts) >= n:
                break
    return prompts


def generate(adapter_dir: Path, prompt: str, max_tokens: int = 80) -> str:
    cmd = [
        sys.executable, "-m", "mlx_lm", "generate",
        "--model", BASE,
        "--adapter-path", str(adapter_dir),
        "--prompt", prompt,
        "--max-tokens", str(max_tokens),
        "--temp", "0.0",
    ]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    except subprocess.TimeoutExpired:
        return "[timeout]"
    if result.returncode != 0:
        return f"[gen_err: {result.stderr[-150:].strip()}]"
    out = result.stdout
    # mlx_lm.generate prints framed output. Strip the framing.
    lines = [
        l for l in out.splitlines()
        if l and not l.startswith("==") and not l.startswith("Prompt") and not l.startswith("Generation:")
        and "tokens-per-sec" not in l and "Peak memory" not in l
    ]
    return ("\n".join(lines)[-300:]).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sft_dir", type=Path)
    ap.add_argument("dpo_dir", type=Path)
    ap.add_argument(
        "--valid",
        type=Path,
        default=Path.home() / ".human/training-data/dpo_finetune/valid.jsonl",
    )
    ap.add_argument("--n", type=int, default=30)
    args = ap.parse_args()

    if not args.sft_dir.exists():
        sys.exit(f"SFT dir missing: {args.sft_dir}")
    if not args.dpo_dir.exists():
        sys.exit(f"DPO dir missing: {args.dpo_dir}")

    prompts = load_prompts(args.valid, n=args.n)
    print(f"Loaded {len(prompts)} prompts from {args.valid}")
    if not prompts:
        sys.exit("No prompts loaded")

    sft_responses: list[str] = []
    dpo_responses: list[str] = []
    for i, p in enumerate(prompts):
        print(f"[{i+1}/{len(prompts)}] {p[:60]!r}...", flush=True)
        t0 = time.time()
        s = generate(args.sft_dir, p)
        t1 = time.time()
        d = generate(args.dpo_dir, p)
        t2 = time.time()
        print(f"  SFT ({t1 - t0:.1f}s): {s[:60]!r}")
        print(f"  DPO ({t2 - t1:.1f}s): {d[:60]!r}")
        sft_responses.append(s)
        dpo_responses.append(d)

    sft_out = Path("/tmp/ab30_sft.json")
    dpo_out = Path("/tmp/ab30_dpo.json")
    sft_out.write_text(json.dumps(sft_responses, ensure_ascii=False, indent=2))
    dpo_out.write_text(json.dumps(dpo_responses, ensure_ascii=False, indent=2))
    print(f"\nWrote {sft_out} ({len(sft_responses)} responses)")
    print(f"Wrote {dpo_out} ({len(dpo_responses)} responses)")


if __name__ == "__main__":
    main()
