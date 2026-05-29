#!/usr/bin/env python3
"""Ordering gate for the reply-first adapter.

Metric: does the user-facing reply come BEFORE the deliberation? Generates each
held-out casual prompt with the v5 adapter (offline) and measures % reply-first +
first-reply-token index. PASS iff pct_reply_first >= floor AND median index <= max_idx.

Exit: 0 = PASS, 1 = FAIL, 2 = SKIP/DEFERRED (mlx_lm unavailable / adapter missing).
Run: python3 scripts/eval_ordering.py --adapter-path <dir> --output-json out.json
"""
import argparse
import json
import statistics
import subprocess
import sys
from datetime import datetime
from pathlib import Path

MODEL_ID = "mlx-community/gemma-4-31b-it-4bit"
DEFAULT_SENTINEL = "<|channel|>thought"


def split_on_sentinel(generated: str, sentinel: str) -> tuple[str, str]:
    idx = generated.find(sentinel)
    if idx == -1:
        return generated, ""
    return generated[:idx], generated[idx + len(sentinel):]


def is_reply_first(generated: str, sentinel: str) -> bool:
    pre, _ = split_on_sentinel(generated, sentinel)
    return len(pre.strip()) > 0


def first_reply_token_index(generated: str, sentinel: str) -> int:
    """0 if reply streams first; large penalty if deliberation precedes the reply."""
    pre, _ = split_on_sentinel(generated, sentinel)
    if len(pre.strip()) > 0:
        return 0
    return len(generated.split())


def build_verdict(generations: list[str], sentinel: str, adapter_path: str,
                  floor: float = 0.90, max_idx: int = 8) -> dict:
    flags = [is_reply_first(g, sentinel) for g in generations]
    idxs = [first_reply_token_index(g, sentinel) for g in generations]
    pct = sum(flags) / len(flags) if flags else 0.0
    median_idx = statistics.median(idxs) if idxs else 1e9
    ordering_pass = pct >= floor and median_idx <= max_idx
    return {
        "timestamp": datetime.now().isoformat(),
        "verdict": "PASS" if ordering_pass else "FAIL",
        "exit_code": 0 if ordering_pass else 1,
        "n_prompts": len(generations),
        "adapter_path": adapter_path,
        "pct_reply_first": round(pct, 4),
        "median_first_reply_token_idx": median_idx,
        "gate": {"ordering_pass": ordering_pass, "floor": floor, "max_idx": max_idx},
    }


def generate(prompt: str, adapter_path: str, max_tokens: int = 80) -> str:
    cmd = [sys.executable, "-m", "mlx_lm", "generate", "--model", MODEL_ID,
           "--adapter-path", adapter_path, "--prompt", prompt,
           "--max-tokens", str(max_tokens), "--temp", "0.0"]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    except subprocess.TimeoutExpired:
        return "[timeout]"
    if r.returncode != 0:
        return "[gen_err]"
    lines = [l for l in r.stdout.splitlines()
             if l and not l.startswith("==") and not l.startswith("Prompt")
             and not l.startswith("Generation:") and "tokens-per-sec" not in l
             and "Peak memory" not in l]
    return "\n".join(lines).strip()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--adapter-path", type=Path, required=True)
    ap.add_argument("--prompts", type=Path,
                    default=Path.home() / ".human/training-data/replyfirst-heldout.jsonl")
    ap.add_argument("--sentinel", default=DEFAULT_SENTINEL)
    ap.add_argument("--output-json", type=Path)
    args = ap.parse_args()

    if not args.adapter_path.exists():
        v = {"timestamp": datetime.now().isoformat(), "verdict": "SKIP",
             "reason": f"adapter not found: {args.adapter_path}", "exit_code": 2}
        print(f"[SKIP] {v['reason']}")
        if args.output_json:
            args.output_json.write_text(json.dumps(v, indent=2))
        return 2

    # held-out examples are SFT {"text": prompt + "\n" + target}; recover the prompt
    prompts = []
    for line in args.prompts.read_text().splitlines():
        if not line.strip():
            continue
        text = json.loads(line)["text"]
        prompts.append(text.rsplit("\n", 1)[0] if "\n" in text else text)

    gens = [generate(p, str(args.adapter_path)) for p in prompts]
    verdict = build_verdict(gens, args.sentinel, str(args.adapter_path))
    print(json.dumps(verdict, indent=2))
    if args.output_json:
        args.output_json.write_text(json.dumps(verdict, indent=2))
    return verdict["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
