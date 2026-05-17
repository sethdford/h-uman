#!/usr/bin/env python3
"""
ab_sweep.py — Evaluate multiple DPO checkpoints against an SFT baseline.

Generates from EACH adapter on the same N prompts, scores each via the
existing `human ml lora-ab` infrastructure, AND counts coherence-failure
modes (<pad> token leakage). Output is a Pareto-frontier view: pick the
adapter that maximizes fidelity WHILE minimizing failure rate.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

BASE = "mlx-community/gemma-4-e2b-it-4bit"


def load_prompts(valid_jsonl: Path, n: int) -> list[str]:
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
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    except subprocess.TimeoutExpired:
        return "[timeout]"
    if r.returncode != 0:
        return f"[gen_err: {r.stderr[-150:].strip()}]"
    out = r.stdout
    lines = [
        l for l in out.splitlines()
        if l and not l.startswith("==") and not l.startswith("Prompt")
        and not l.startswith("Generation:")
        and "tokens-per-sec" not in l and "Peak memory" not in l
    ]
    return ("\n".join(lines)[-300:]).strip()


def score(persona: str, before: Path, after: Path) -> tuple[float, float, float, int, int]:
    """Returns (before_mean, after_mean, delta, before_scored, after_scored)."""
    cmd = ["./build/human", "ml", "lora-ab",
           "--persona", persona, "--before", str(before), "--after", str(after)]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    out = r.stdout
    bm = am = d = 0.0
    bs = as_ = 0
    for line in out.splitlines():
        if "before:" in line and "mean=" in line:
            bm = float(line.split("mean=")[1].split()[0])
            bs = int(line.split("scored=")[1].split()[0])
        elif "after:" in line and "mean=" in line:
            am = float(line.split("mean=")[1].split()[0])
            as_ = int(line.split("scored=")[1].split()[0])
        elif "delta:" in line:
            d = float(line.split("delta:")[1].strip().split()[0])
    return bm, am, d, bs, as_


def failure_rate(responses: list[str]) -> dict[str, int]:
    """Count failure modes per response set."""
    return {
        "pad_leakage": sum(1 for r in responses if "<pad>" in r),
        "very_short": sum(1 for r in responses if len(r) < 20),
        "non_ascii_garbage": sum(1 for r in responses if any(ord(c) > 0x7F and not c.isalpha() for c in r) and "<pad>" not in r),
        "n": len(responses),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sft_dir", type=Path)
    ap.add_argument("dpo_dirs", nargs="+", type=Path, help="One or more DPO checkpoint dirs to evaluate")
    ap.add_argument("--persona", default="seth")
    ap.add_argument(
        "--valid", type=Path,
        default=Path.home() / ".human/training-data/dpo_finetune/valid.jsonl",
    )
    ap.add_argument("--n", type=int, default=30)
    ap.add_argument("--outdir", type=Path, default=Path("/tmp/ab_sweep"))
    args = ap.parse_args()

    args.outdir.mkdir(parents=True, exist_ok=True)

    prompts = load_prompts(args.valid, args.n)
    print(f"Loaded {len(prompts)} prompts from {args.valid}\n")

    # Step 1: generate from SFT baseline once
    sft_path = args.outdir / "sft.json"
    if sft_path.exists():
        print(f"Reusing {sft_path}")
        sft_responses = json.loads(sft_path.read_text())
    else:
        print(f"Generating SFT baseline ({args.sft_dir.name})...")
        sft_responses = []
        for i, p in enumerate(prompts):
            t = time.time()
            r = generate(args.sft_dir, p)
            print(f"  [{i+1}/{len(prompts)}] {time.time()-t:.1f}s")
            sft_responses.append(r)
        sft_path.write_text(json.dumps(sft_responses, ensure_ascii=False, indent=2))

    # Step 2: generate from each DPO checkpoint
    results = []
    for dpo_dir in args.dpo_dirs:
        dpo_path = args.outdir / f"{dpo_dir.name}.json"
        if dpo_path.exists():
            print(f"Reusing {dpo_path}")
            dpo_responses = json.loads(dpo_path.read_text())
        else:
            print(f"\nGenerating {dpo_dir.name}...")
            dpo_responses = []
            for i, p in enumerate(prompts):
                t = time.time()
                r = generate(dpo_dir, p)
                print(f"  [{i+1}/{len(prompts)}] {time.time()-t:.1f}s")
                dpo_responses.append(r)
            dpo_path.write_text(json.dumps(dpo_responses, ensure_ascii=False, indent=2))

        bm, am, d, bs, as_ = score(args.persona, sft_path, dpo_path)
        fr = failure_rate(dpo_responses)
        results.append({
            "checkpoint": dpo_dir.name,
            "fidelity_mean": am,
            "delta_vs_sft": d,
            "pad_leakage": fr["pad_leakage"],
            "very_short": fr["very_short"],
            "n": fr["n"],
        })

    # Step 3: print Pareto-frontier table
    sft_fr = failure_rate(sft_responses)
    print("\n" + "=" * 90)
    print(f"{'Checkpoint':<25} {'Fidelity':<12} {'Δ vs SFT':<12} {'<pad> fails':<14} {'short fails':<14}")
    print("-" * 90)
    print(f"{'SFT baseline':<25} {'-':<12} {'0.000':<12} {sft_fr['pad_leakage']:<14} {sft_fr['very_short']:<14}")
    for r in results:
        print(f"{r['checkpoint']:<25} {r['fidelity_mean']:.3f}        {r['delta_vs_sft']:+.3f}        {r['pad_leakage']}/{r['n']:<10}    {r['very_short']}/{r['n']}")
    print("=" * 90)
    print("\nSweet spot = highest Δ with lowest <pad> failures.")


if __name__ == "__main__":
    main()
