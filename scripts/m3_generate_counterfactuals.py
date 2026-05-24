#!/usr/bin/env python3
"""
Phase H2 (2026-05-18) — counterfactual preference data generator.

Takes a corpus JSONL from H1 (m3_extract_corpus.py) and produces
preference pairs ready for DPO training, WITHOUT waiting for real
production traffic to accumulate REWRITEs.

How it works:
  1. Read N Seth-authored turns from the H1 corpus.
  2. For each, prompt a strong LLM (default: GPT-5) to generate K
     plausible variations along specific style axes (more formal,
     less casual, longer, shorter, with emoji, more direct).
  3. The original Seth response is `chosen`. The most-different
     variation is `rejected`. Emit Alpaca-DPO JSONL:
         {"prompt": <prev message>, "chosen": <Seth>, "rejected": <variant>}

  N real responses → N preference pairs (one per pivot) at minimum.

Why this is legitimate training data:
  - The PAIR teaches the trainer "Seth picks brevity over verbosity"
    or "Seth uses 'yeah' not 'absolutely'" — directional preference,
    not absolute content.
  - The original real Seth response is the canonical "good" answer.
  - The LLM-generated variant is a HYPOTHESIS about what wrong-style
    looks like; the gradient teaches the model to move away from it.

Fallback when no LLM API:
  - Deterministic synthetic generator that produces obvious style
    violations (assistant-speak phrases, formal greetings, repeated
    hedging). Useful for tests + as a baseline; less rich than LLM
    output but legitimate negative signal.

Usage:
    python3 scripts/m3_generate_counterfactuals.py \\
        --corpus ~/.human/training-data/m3-corpus.jsonl \\
        --out ~/.human/training-data/m3-counterfactuals.jsonl \\
        --max-records 200 --variations 3

Exit:
    0 — wrote at least one pair
    2 — input missing or no Seth turns found
"""
from __future__ import annotations

import argparse
import json
import os
import random
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

DEFAULT_CORPUS = Path.home() / ".human" / "training-data" / "m3-corpus.jsonl"
DEFAULT_OUT = Path.home() / ".human" / "training-data" / "m3-counterfactuals.jsonl"


# ─────────────────────────────────────────────────────────────────────
# Synthetic (deterministic) variant generator — used when no LLM
# ─────────────────────────────────────────────────────────────────────

ASSISTANT_PREFIXES = [
    "I would be happy to help with that. ",
    "Certainly! ",
    "Absolutely. ",
    "As an AI assistant, ",
    "Let me think about that. ",
]
ASSISTANT_HEDGES = [
    " I hope this helps.",
    " Please let me know if you have any other questions.",
    " Is there anything else I can assist you with?",
    " Let me know if that makes sense.",
]
FORMAL_REWRITES = {
    "yeah": "yes, certainly",
    "yep": "yes",
    "nope": "no, I'm afraid not",
    "haha": "(I find that amusing)",
    "lol": "(amusing)",
    "ok": "very well",
    "k": "very well",
    "thx": "thank you",
}


def synthetic_variants(text: str, k: int = 3) -> list[str]:
    """Deterministic style violations of `text`. Each result is
    distinctly worse-than-original by Seth's standards (more assistant-
    sounding, more formal, more padded). Used as the test path and as
    the fallback when no LLM API is available."""
    if not text:
        return []
    rng = random.Random(hash(text) & 0xFFFFFFFF)  # deterministic per-input

    variants = []

    # Variant 1: assistant prefix + hedge tail (the "robotic" style)
    pfx = ASSISTANT_PREFIXES[rng.randrange(len(ASSISTANT_PREFIXES))]
    sfx = ASSISTANT_HEDGES[rng.randrange(len(ASSISTANT_HEDGES))]
    variants.append(f"{pfx}{text}{sfx}")

    # Variant 2: formalize casual tokens (yeah→yes, lol→(amusing))
    formal = text
    for casual, replacement in FORMAL_REWRITES.items():
        formal = formal.replace(casual, replacement).replace(
            casual.capitalize(), replacement.capitalize())
    if formal != text:
        variants.append(formal)
    else:
        # Fallback if no formalization applied — pad with verbose hedges
        variants.append(f"Allow me to provide a thoughtful response: {text}. "
                         f"I trust this is what you were looking for.")

    # Variant 3: ultra-verbose expansion (>3x length)
    if len(variants) < k:
        variants.append(
            f"To address your inquiry comprehensively: {text}. "
            f"I want to ensure I've fully understood the context here, "
            f"so please feel free to clarify if my interpretation is off.")

    return variants[:k]


# ─────────────────────────────────────────────────────────────────────
# LLM variant generator — OpenAI by default
# ─────────────────────────────────────────────────────────────────────

LLM_PROMPT_TMPL = (
    "You generate {k} alternative responses to the message below. Each "
    "alternative should sound LESS like a casual friend texting and MORE "
    "like a stilted AI assistant (more formal, longer, more hedging, "
    "fewer contractions, fewer typos). Keep the meaning roughly the same.\n\n"
    "Reply with EXACTLY {k} numbered alternatives, one per line, like:\n"
    "1. <alternative 1>\n"
    "2. <alternative 2>\n"
    "3. <alternative 3>\n\n"
    "Previous message in conversation: {prev}\n"
    "Real Seth response: {real}\n"
)


def llm_variants(prev: str, real: str, k: int, model: str,
                 api_key: str) -> list[str]:
    """Call the LLM. Returns list of variant strings, or [] on failure."""
    body = json.dumps({
        "model": model,
        "messages": [{"role": "user",
                       "content": LLM_PROMPT_TMPL.format(k=k, prev=prev, real=real)}],
        "max_tokens": 400,
        "temperature": 0.7,
    }).encode()
    req = urllib.request.Request("https://api.openai.com/v1/chat/completions",
                                  data=body, method="POST",
                                  headers={"Content-Type": "application/json",
                                           "Authorization": f"Bearer {api_key}"})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            payload = json.loads(resp.read().decode())
        text = (payload.get("choices", [{}])[0]
                       .get("message", {}).get("content", "").strip())
    except (urllib.error.URLError, urllib.error.HTTPError, json.JSONDecodeError) as e:
        print(f"  WARN: LLM call failed ({e}). Falling back to synthetic variants.")
        return []
    # Parse "1. ...\n2. ...\n3. ..." into a list
    variants = []
    for line in text.split("\n"):
        line = line.strip()
        if not line:
            continue
        # Strip leading "1.", "2.", etc.
        if line[0].isdigit() and line[1:3] in (". ", ".\t", ") "):
            line = line[3:].strip()
        elif line.startswith("- "):
            line = line[2:].strip()
        if line:
            variants.append(line)
    return variants[:k]


# ─────────────────────────────────────────────────────────────────────
# Corpus pair finder — pairs each Seth turn with its preceding context
# ─────────────────────────────────────────────────────────────────────

def find_seth_turns_with_context(corpus_path: Path,
                                  max_records: int) -> list[tuple[str, str]]:
    """Walk the corpus chronologically; for each Seth (`assistant` role)
    turn, find the immediately-preceding `user` turn from the SAME
    contact (`handle`). Returns list of (prev_user_text, seth_text).

    Why same-contact: cross-contact pairs don't make sense as a
    conversation. A reply to Mom and a reply to a coworker on the
    same day aren't in the same conversation context.
    """
    if not corpus_path.exists():
        return []
    by_contact: dict[str, list[dict]] = {}
    with open(corpus_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            by_contact.setdefault(r.get("handle", ""), []).append(r)
    pairs = []
    for handle, records in by_contact.items():
        # Sort chronologically (oldest first)
        records.sort(key=lambda r: r.get("ts_ms", 0))
        for i, r in enumerate(records):
            if r.get("role") != "assistant":
                continue
            # Walk backward for the most-recent user turn from same contact
            for j in range(i - 1, -1, -1):
                if records[j].get("role") == "user":
                    pairs.append((records[j]["content"], r["content"]))
                    break
            if len(pairs) >= max_records:
                return pairs
    return pairs


# ─────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--max-records", type=int, default=200,
                    help="Cap on Seth turns to process (default 200)")
    ap.add_argument("--variations", type=int, default=3,
                    help="Variations per turn (default 3; cheapest is 3)")
    ap.add_argument("--model", default=os.environ.get("HUMAN_LLM_MODEL", "gpt-5"))
    ap.add_argument("--no-llm", action="store_true",
                    help="Skip LLM calls — use synthetic variants only")
    ap.add_argument("--seed", type=int, default=0,
                    help="Random seed for variant-pick determinism")
    args = ap.parse_args()

    print(f"\n{'='*60}")
    print(f"  M3 COUNTERFACTUAL GENERATOR (H2)")
    print(f"{'='*60}")
    print(f"  Corpus:     {args.corpus}")
    print(f"  Out:        {args.out}")
    print(f"  Max recs:   {args.max_records}")
    print(f"  Variations: {args.variations} per turn")

    pairs = find_seth_turns_with_context(args.corpus, args.max_records)
    print(f"  Seth turns: {len(pairs)} (paired with user context)")
    if not pairs:
        print(f"  ERROR: no usable Seth turns found in {args.corpus}", file=sys.stderr)
        return 2

    api_key = os.environ.get("OPENAI_API_KEY", "")
    use_llm = not args.no_llm and api_key
    if not use_llm:
        print(f"  Using SYNTHETIC variants (no OPENAI_API_KEY / --no-llm)")
    else:
        print(f"  Using LLM variants (model={args.model})")
    print(f"{'='*60}\n")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    # --seed > 0 → reproducible random variant pick (gives training data
    # diversity across regeneration cycles). --seed == 0 (default) →
    # deterministic "most-different by length" pick (preserves the
    # original behavior; same input → same rejected, run after run).
    rng = random.Random(args.seed) if args.seed else None
    n_written = 0

    with open(args.out, "w") as fout:
        for prev, real in pairs:
            if not real or len(real) < 3:
                continue
            if use_llm:
                variants = llm_variants(prev, real, args.variations,
                                         args.model, api_key)
                if not variants:
                    variants = synthetic_variants(real, args.variations)
            else:
                variants = synthetic_variants(real, args.variations)
            if not variants:
                continue
            # Pick the rejected variant:
            #   seed=0  → deterministic max-length-delta (default)
            #   seed>0  → seeded random pick (reproducible diversity)
            if rng is not None:
                rejected = rng.choice(variants)
            else:
                rejected = max(variants, key=lambda v: abs(len(v) - len(real)))
            record = {
                "prompt": prev,
                "chosen": real,
                "rejected": rejected,
                "_source": "counterfactual",
                "_variations_count": len(variants),
            }
            fout.write(json.dumps(record, ensure_ascii=False) + "\n")
            n_written += 1

    print(f"  Wrote {n_written} preference pairs → {args.out}")
    if n_written == 0:
        print(f"  ERROR: no pairs emitted", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
