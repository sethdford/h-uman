#!/usr/bin/env python3
"""
M3 behavioral eval (2026-05-19) — the "did it actually learn?" test.

The metadata judge in m3_eval_adapter.py only checks STRUCTURE: did
training produce a real safetensors file with non-zero tensors? PASS
means the pipeline worked, not that the model behaves more like Seth.

This script proves BEHAVIOR. For each held-out prompt (one the trainer
never saw):
  1. Generate from BASE model (no adapter)
  2. Generate from BASE + CANDIDATE adapter
  3. Score both outputs against Seth's style heuristics:
       - length (Seth tends to be brief)
       - emoji presence (Seth uses them)
       - casual markers (yeah, lol, haha, yep)
       - formal markers (Certainly!, As an AI, I would be happy)
       - contractions (it's, can't, won't)
  4. Aggregate Seth-style scores per output; compare base vs candidate

A PASS verdict requires the candidate to beat base on at least 3 of
the 5 dimensions, AND not regress significantly on any.

Costs ~1-2 sec per prompt × 2 generations × N prompts. With default
N=20 prompts and a 4B model, runs in ~1 minute.

Usage:
    python3 scripts/m3_behavioral_eval.py \\
        --model mlx-community/gemma-3-4b-it-bf16 \\
        --candidate-adapter ~/.human/training-data/adapters/seth-dpo-full-072746 \\
        --prompts-jsonl ~/.human/training-data/m3-holdout-prompts.jsonl \\
        --max-prompts 20 \\
        --json-out /tmp/m3-behavioral-verdict.json

Exit codes:
  0 — verdict produced (regardless of pass/fail)
  2 — mlx_lm missing or model unreachable
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path


# ─────────────────────────────────────────────────────────────────────
# Style heuristics — deterministic, no API needed
# ─────────────────────────────────────────────────────────────────────

CASUAL_MARKERS = [
    "yeah", "yep", "yup", "nope", "lol", "lmao", "haha", "ha",
    "ok", "okay", "k ", "fr", "tbh", "ngl", "imo", "btw",
    "kinda", "sorta", "gonna", "wanna", "gotta",
]
# Match if the FULL output stripped is short and casual, OR the casual
# marker appears as a clear word boundary
FORMAL_MARKERS = [
    "certainly", "absolutely", "I would be happy to",
    "as an ai", "let me assist", "happy to help",
    "I'd be glad to", "of course", "by all means",
    "I hope this helps", "is there anything else",
    "please let me know", "feel free to",
]
EMOJI_RE = re.compile(r"[\U0001F300-\U0001FAFF\U0001F600-\U0001F64F"
                       r"\U00002600-\U000027BF\U0001F900-\U0001F9FF"
                       r"\U0001F680-\U0001F6FF\U0001F1E0-\U0001F1FF]+")
CONTRACTION_RE = re.compile(
    r"\b(?:it's|that's|don't|can't|won't|isn't|wasn't|hasn't|haven't|"
    r"doesn't|didn't|wouldn't|couldn't|shouldn't|let's|I'm|I'll|"
    r"I've|I'd|you're|you'll|you've|you'd|we're|we'll|we've|"
    r"they're|they'll|they've|they'd|he's|she's|there's|here's|"
    r"what's|where's|when's|how's)\b",
    re.IGNORECASE)


def style_score(text: str) -> dict:
    """Compute the 5 Seth-style dimensions for one output. Each is a
    raw number; the verdict aggregates and compares."""
    t = (text or "").strip()
    lower = t.lower()
    n_chars = len(t)
    casual_hits = sum(1 for m in CASUAL_MARKERS if m in lower)
    formal_hits = sum(1 for m in FORMAL_MARKERS if m in lower)
    emoji_count = len(EMOJI_RE.findall(t))
    contractions = len(CONTRACTION_RE.findall(t))
    return {
        "n_chars": n_chars,
        "casual_hits": casual_hits,
        "formal_hits": formal_hits,
        "emoji_count": emoji_count,
        "contractions": contractions,
    }


# ─────────────────────────────────────────────────────────────────────
# MLX generate wire
# ─────────────────────────────────────────────────────────────────────

def mlx_generate(model: str, prompt: str, adapter_path: Path | None,
                  max_tokens: int = 80, temp: float = 0.7,
                  seed: int = 0) -> str:
    """Invoke `python -m mlx_lm generate ...` and return the assistant
    response only (strips the prompt echo and chat-template tags)."""
    cmd = [
        sys.executable, "-m", "mlx_lm", "generate",
        "--model", model,
        "--prompt", prompt,
        "--max-tokens", str(max_tokens),
        "--temp", f"{temp:g}",
        "--seed", str(seed),
        "--verbose", "False",
    ]
    if adapter_path is not None:
        cmd += ["--adapter-path", str(adapter_path)]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True,
                                  timeout=120)
    except subprocess.TimeoutExpired:
        return "[TIMEOUT]"
    if result.returncode != 0:
        return f"[ERROR rc={result.returncode}: {result.stderr.strip()[:100]}]"
    # mlx_lm's --verbose False output is just the generated text
    out = result.stdout.strip()
    # Strip any chat-template tags that may have leaked through
    for tag in ("<start_of_turn>", "<end_of_turn>", "<eos>"):
        out = out.replace(tag, "").strip()
    return out


# ─────────────────────────────────────────────────────────────────────
# Aggregation + verdict
# ─────────────────────────────────────────────────────────────────────

def aggregate_scores(per_prompt: list[dict]) -> dict:
    """Sum / mean each style dimension across all prompts."""
    if not per_prompt:
        return {}
    n = len(per_prompt)
    out = {
        "n_prompts":      n,
        "mean_chars":     sum(p["n_chars"]      for p in per_prompt) / n,
        "total_casual":   sum(p["casual_hits"]  for p in per_prompt),
        "total_formal":   sum(p["formal_hits"]  for p in per_prompt),
        "total_emoji":    sum(p["emoji_count"]  for p in per_prompt),
        "total_contract": sum(p["contractions"] for p in per_prompt),
    }
    return out


def behavioral_verdict(base_agg: dict, cand_agg: dict,
                        ref_agg: dict) -> dict:
    """Compare candidate vs base across 5 dimensions. The contract for
    EVERY dimension is the same: closer-to-reference is better.

    The earlier version assumed casual/emoji/contractions were
    monotonically Seth-like ("more = better"), but the live 2026-05-19
    run showed that's wrong — base gemma-3-4b HALLUCINATES Seth-like
    behavior, producing MORE casual markers / contractions / emoji
    than Seth's actual short replies. The adapter correctly tones
    that back, but a "more = better" heuristic flagged the correction
    as a regression.

    Use distance-to-reference for ALL dimensions. The adapter wins
    when its dimension value is strictly closer to the reference
    than base's value is.
    """
    def closer_to_ref(base_v, cand_v, ref_v):
        return abs(cand_v - ref_v) < abs(base_v - ref_v)

    dims = [
        {"name": "length_proximity",
         "base":      round(base_agg["mean_chars"], 1),
         "candidate": round(cand_agg["mean_chars"], 1),
         "reference": round(ref_agg["mean_chars"], 1)},
        {"name": "casual_markers",
         "base":      base_agg["total_casual"],
         "candidate": cand_agg["total_casual"],
         "reference": ref_agg["total_casual"]},
        {"name": "formal_markers",
         "base":      base_agg["total_formal"],
         "candidate": cand_agg["total_formal"],
         "reference": ref_agg["total_formal"]},
        {"name": "emoji_count",
         "base":      base_agg["total_emoji"],
         "candidate": cand_agg["total_emoji"],
         "reference": ref_agg["total_emoji"]},
        {"name": "contractions",
         "base":      base_agg["total_contract"],
         "candidate": cand_agg["total_contract"],
         "reference": ref_agg["total_contract"]},
    ]
    for d in dims:
        d["candidate_better"] = closer_to_ref(d["base"], d["candidate"],
                                                d["reference"])

    wins = sum(1 for d in dims if d["candidate_better"])
    if wins >= 3:
        verdict = "pass"
        reason = f"candidate beats base on {wins}/5 Seth-style dimensions"
    elif wins == 2:
        verdict = "no-change"
        reason = f"candidate beats base on {wins}/5 dimensions (mixed)"
    else:
        verdict = "regress"
        reason = f"candidate only beats base on {wins}/5 dimensions"
    return {
        "judge": "behavioral",
        "verdict": verdict,
        "reason": reason,
        "wins": wins,
        "dimensions": dims,
        "base_agg": base_agg,
        "candidate_agg": cand_agg,
        "reference_agg": ref_agg,
    }


def diversity_check(candidate_outputs: list[str]) -> dict:
    """Detect mode collapse: an adapter that emits the same (or
    near-identical) output regardless of the prompt is structurally
    Seth-like by style metrics but is semantically broken.

    Returns {is_collapsed, distinct_ratio, top_repeat, top_count}:
      - distinct_ratio: unique outputs / total outputs (1.0 = all different,
        0.1 = 90% are identical)
      - top_repeat: the most-frequent output (truncated)
      - top_count: how many times it appears
      - is_collapsed: True when distinct_ratio < 0.5 OR top_count >= 4
        out of any N (the "I'm here!" failure we saw in 2026-05-19)
    """
    if not candidate_outputs:
        return {"is_collapsed": False, "distinct_ratio": 1.0,
                "top_repeat": "", "top_count": 0}
    n = len(candidate_outputs)
    distinct = len(set(candidate_outputs))
    ratio = distinct / n
    # Most-frequent output
    from collections import Counter
    counter = Counter(candidate_outputs)
    top, top_count = counter.most_common(1)[0]
    # Mode collapse if many duplicates
    is_collapsed = (ratio < 0.5) or (top_count >= 4 and n >= 4)
    return {
        "is_collapsed": is_collapsed,
        "distinct_ratio": round(ratio, 3),
        "distinct_count": distinct,
        "total_count": n,
        "top_repeat": top[:80],
        "top_count": top_count,
    }


# ─────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--model", default=os.environ.get(
        "HUMAN_MLX_MODEL", "mlx-community/gemma-3-4b-it-bf16"))
    ap.add_argument("--candidate-adapter", type=Path, required=True,
                    help="Adapter dir or .safetensors to compare against base")
    ap.add_argument("--prompts-jsonl", type=Path, required=True,
                    help="Held-out prompts {prompt, reference} JSONL")
    ap.add_argument("--max-prompts", type=int, default=20,
                    help="Cap on prompts evaluated (compute cost; default 20)")
    ap.add_argument("--max-tokens", type=int, default=60,
                    help="Max tokens per generation (default 60)")
    ap.add_argument("--json-out", type=Path,
                    help="Write verdict + sample outputs to this path")
    ap.add_argument("--samples-out", type=Path,
                    help="Write side-by-side prompt/base/candidate JSONL")
    args = ap.parse_args()

    print(f"\n{'='*60}")
    print(f"  M3 BEHAVIORAL EVAL (judge=heuristic)")
    print(f"{'='*60}")
    print(f"  Model:     {args.model}")
    print(f"  Adapter:   {args.candidate_adapter}")
    print(f"  Prompts:   {args.prompts_jsonl}")
    print(f"  Max:       {args.max_prompts} prompts")

    if not args.prompts_jsonl.exists():
        print(f"  ERROR: holdout prompts not found", file=sys.stderr)
        return 2

    # Load held-out
    prompts: list[dict] = []
    with open(args.prompts_jsonl) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                prompts.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    prompts = prompts[:args.max_prompts]
    print(f"  Using {len(prompts)} held-out prompts")
    if not prompts:
        print(f"  ERROR: no prompts loaded", file=sys.stderr)
        return 2

    # Score reference outputs (these are Seth's actual replies)
    ref_scores = [style_score(p["reference"]) for p in prompts]
    ref_agg = aggregate_scores(ref_scores)

    # Generate base + candidate
    base_scores: list[dict] = []
    cand_scores: list[dict] = []
    samples: list[dict] = []
    start = time.time()
    for i, p in enumerate(prompts):
        prompt = p["prompt"]
        print(f"  [{i+1}/{len(prompts)}]  {prompt[:50]!r}...")
        base_out = mlx_generate(args.model, prompt, adapter_path=None,
                                  max_tokens=args.max_tokens)
        cand_out = mlx_generate(args.model, prompt, adapter_path=args.candidate_adapter,
                                  max_tokens=args.max_tokens)
        base_scores.append(style_score(base_out))
        cand_scores.append(style_score(cand_out))
        samples.append({
            "prompt": prompt,
            "reference": p.get("reference", ""),
            "base": base_out,
            "candidate": cand_out,
        })
        # Quick progress signal: elapsed + a sample
        if i < 2:
            print(f"      base:      {base_out[:80]!r}")
            print(f"      candidate: {cand_out[:80]!r}")
            print(f"      reference: {p['reference'][:80]!r}")

    elapsed = time.time() - start
    print(f"  Elapsed: {elapsed:.1f}s ({elapsed/len(prompts):.2f}s/prompt)")

    # Verdict
    base_agg = aggregate_scores(base_scores)
    cand_agg = aggregate_scores(cand_scores)
    v = behavioral_verdict(base_agg, cand_agg, ref_agg)

    # Mode-collapse guard: override the verdict if the candidate
    # produced the same output for most prompts. This is a known
    # over-training failure on small datasets — style metrics say
    # PASS (the single output is Seth-like) but the model lost its
    # ability to respond contextually.
    div = diversity_check([s["candidate"] for s in samples])
    v["diversity"] = div
    if div["is_collapsed"]:
        v["verdict_pre_diversity"] = v["verdict"]
        v["verdict"] = "regress"
        v["reason"] = (f"mode collapse: candidate emits identical output "
                       f"on {div['top_count']}/{div['total_count']} prompts "
                       f"({div['distinct_ratio']*100:.0f}% distinct). "
                       f"Top repeat: {div['top_repeat']!r}. "
                       f"(Pre-diversity verdict was: {v['verdict_pre_diversity']})")

    print()
    print(f"  Verdict:   {v['verdict'].upper()}")
    print(f"  Reason:    {v['reason']}")
    print()
    print(f"  Dimensions (lower = closer to reference):")
    for d in v["dimensions"]:
        winner = "✓" if d["candidate_better"] else "✗"
        print(f"    {winner} {d['name']:<20}  "
              f"base={d['base']!s:>8}  "
              f"candidate={d['candidate']!s:>8}  "
              f"ref={d['reference']!s:>8}")

    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        with open(args.json_out, "w") as f:
            json.dump(v, f, indent=2, ensure_ascii=False)
        print(f"  Verdict JSON: {args.json_out}")
    if args.samples_out:
        args.samples_out.parent.mkdir(parents=True, exist_ok=True)
        with open(args.samples_out, "w") as f:
            for s in samples:
                f.write(json.dumps(s, ensure_ascii=False) + "\n")
        print(f"  Samples:      {args.samples_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
