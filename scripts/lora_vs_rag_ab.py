#!/usr/bin/env python3
"""
Wave 7 / W7-3 — LoRA-vs-RAG A/B harness.

Tests the assumption we inherited (fine-tune the model) against the approach the
research says often wins at low data volume: RAG over the target's OWN past
messages as per-turn few-shot grounding. For the SAME prompts it scores two
paths with the per-axis decomposition (W7-2) and reports which wins, per axis.

  Path A (LoRA): generate via the local Gemma+LoRA serving path.
  Path B (RAG):  retrieve the K most similar real messages from the reference
                 corpus, inject them as few-shot, generate via a base model.

Generation needs a live model; --dry-run lets the harness + comparison run
headless (retrieval + scoring are deterministic and the interesting part). Use
--responses-a / --responses-b to score pre-captured outputs from a real run.

Usage:
  scripts/lora_vs_rag_ab.py --reference real.jsonl --prompts heldout.jsonl --dry-run
"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import fidelity_axes as fx


def _content_words(text):
    return {w for w in fx._words(text) if w not in fx.STOPWORDS and len(w) > 2}


def retrieve_similar(prompt, corpus, k=3):
    """Pure lexical retrieval: top-k corpus messages by content-word Jaccard to
    the prompt. The RAG grounding pool. Ties broken by original order."""
    pw = _content_words(prompt)
    scored = []
    for i, msg in enumerate(corpus):
        mw = _content_words(msg)
        denom = len(pw | mw) or 1
        scored.append((len(pw & mw) / denom, -i, msg))
    scored.sort(reverse=True)
    return [m for _, _, m in scored[:k]]


def build_rag_fewshot(prompt, retrieved):
    """Assemble a few-shot prompt grounding the model in the target's real voice."""
    lines = ["Here are examples of how I text:"]
    for ex in retrieved:
        lines.append(f"- {ex}")
    lines.append("")
    lines.append(f"Reply in exactly that voice to: {prompt}")
    return "\n".join(lines)


def compare_paths(reference, lora_responses, rag_responses):
    """Pure: score both paths per-axis vs the reference, declare per-axis +
    overall winners and a recommendation."""
    a = fx.decompose(reference, lora_responses)
    b = fx.decompose(reference, rag_responses)
    per_axis = {}
    a_wins = b_wins = ties = 0
    for axis in a["axes"]:
        sa, sb = a["axes"][axis], b["axes"][axis]
        if abs(sa - sb) < 0.02:
            winner = "tie"
            ties += 1
        elif sa > sb:
            winner = "lora"
            a_wins += 1
        else:
            winner = "rag"
            b_wins += 1
        per_axis[axis] = {"lora": sa, "rag": sb, "winner": winner}

    if a["aggregate"] > b["aggregate"] + 0.02:
        overall = "lora"
    elif b["aggregate"] > a["aggregate"] + 0.02:
        overall = "rag"
    else:
        overall = "tie"

    rec = {
        "lora": "Fine-tune wins — keep LoRA as the primary voice path.",
        "rag": "RAG-over-own-messages wins — retrieval grounding beats the adapter "
        "at this data volume; consider RAG-first (or hybrid).",
        "tie": "Statistical tie — prefer the cheaper/operationally-simpler path "
        "(usually RAG, since it needs no training run).",
    }[overall]
    return {
        "overall_winner": overall,
        "lora_aggregate": a["aggregate"],
        "rag_aggregate": b["aggregate"],
        "axis_wins": {"lora": a_wins, "rag": b_wins, "tie": ties},
        "per_axis": per_axis,
        "recommendation": rec,
    }


def _gen_local_dry(prompt):
    return "yeah sounds good, lemme know"  # stand-in adapter voice


def _gen_rag_dry(prompt, retrieved):
    # RAG dry-run echoes the closest real message — simulates grounding in voice.
    return retrieved[0] if retrieved else "ok"


def run_ab(reference, prompts, dry_run, mlx_url, k=3):
    lora_resps, rag_resps = [], []
    for p in prompts:
        retrieved = retrieve_similar(p, reference, k)
        if dry_run:
            lora_resps.append(_gen_local_dry(p))
            rag_resps.append(_gen_rag_dry(p, retrieved))
        else:
            # Live generation requires the running server/provider; not wired in
            # this standalone harness. Capture responses elsewhere and pass via
            # --responses-a / --responses-b.
            raise SystemExit("live generation not supported here; use --dry-run or --responses-a/-b")
    return compare_paths(reference, lora_resps, rag_resps)


def main():
    ap = argparse.ArgumentParser(description="LoRA-vs-RAG A/B fidelity harness (W7-3)")
    ap.add_argument("--reference", type=Path, required=True, help="real target messages (RAG pool + scoring ref)")
    ap.add_argument("--prompts", type=Path, required=True, help="held-out prompts JSONL")
    ap.add_argument("--responses-a", type=Path, default=None, help="pre-captured LoRA responses")
    ap.add_argument("--responses-b", type=Path, default=None, help="pre-captured RAG responses")
    ap.add_argument("--mlx-url", default="http://127.0.0.1:8741/v1")
    ap.add_argument("--k", type=int, default=3)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--output-json", type=Path, default=None)
    args = ap.parse_args()

    reference = fx.load_messages(args.reference)
    if args.responses_a and args.responses_b:
        result = compare_paths(reference, fx.load_messages(args.responses_a),
                               fx.load_messages(args.responses_b))
    else:
        prompts = fx.load_messages(args.prompts)
        result = run_ab(reference, prompts, args.dry_run, args.mlx_url, args.k)

    print(json.dumps(result, indent=2))
    print(f"\n=== A/B WINNER: {result['overall_winner'].upper()} "
          f"(lora {result['lora_aggregate']:.2f} vs rag {result['rag_aggregate']:.2f}) ===",
          file=sys.stderr)
    print(f"  {result['recommendation']}", file=sys.stderr)
    if args.output_json:
        args.output_json.write_text(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
