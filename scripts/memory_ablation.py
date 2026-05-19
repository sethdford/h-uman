#!/usr/bin/env python3
"""
Layer 2 / Month 4 — Memory ablation runner.

Tests the SILENT ASSUMPTION: does the giant memory context block
(personal_model, world_model, contact_context, conversation_context,
humanness_context, residue_carryover, etc.) materially change h-uman's
outputs, or does the model treat it as noise?

For each test prompt, runs the SAME prompt through two configs and
compares shape-score + speaker-ID:
  A. Full agent_turn pipeline (gateway with --with-agent, all memory injected)
  B. Direct MLX with persona-only prompt (no memory context)

If A and B produce statistically-equivalent shape-scores and speaker-ID
probabilities, memory is NOT moving outputs — and Layer 2's RAG-targeted
refactor will reclaim throughput + tokens.

If A measurably exceeds B, memory IS doing real work — and the
question becomes which memory fields move the needle. (Future work:
ablate one memory field at a time.)

Per [Eval4Sim 2026 arXiv:2603.02876], the right metric is APC-style
atomic-claim faithfulness; pure-Python proxy here uses shape_score +
PersonaEval P(Seth).

Usage:
  python3 scripts/memory_ablation.py --suite eval_suites/imessage_humanness.json
"""

import argparse
import json
import statistics
import sys
import time
from pathlib import Path
from urllib import error, request

sys.path.insert(0, str(Path(__file__).parent))
from eval_shape_classifier import classify  # noqa: E402

GATEWAY = "http://127.0.0.1:3006/v1/chat/completions"  # full agent_turn
MLX_DIRECT = "http://127.0.0.1:8741/v1/chat/completions"  # bare MLX

PERSONA_PATH = Path.home() / ".human" / "personas" / "seth.json"


def build_compact_persona_prompt() -> str:
    """Mirror hu_persona_build_prompt_compact for the bare-MLX path.
    Same structure that the eval-side compact builder produces:
    identity + iMessage overlay + 5 example shots + closing imperative."""
    p = json.loads(PERSONA_PATH.read_text())
    parts = []
    if p.get("core_anchor"):
        parts.append(p["core_anchor"])
    identity = (p.get("core") or {}).get("identity") or p.get("identity") or ""
    if identity:
        parts.append(f"You are {p.get('name','Seth')}. {identity[:600]}")
    overlay = next((o for o in p.get("overlays", []) if o.get("channel") == "imessage"),
                   (p.get("channel_overlays") or {}).get("imessage"))
    if isinstance(overlay, dict):
        parts.append("Channel: imessage")
        for k in ("formality", "avg_length", "emoji_usage"):
            v = overlay.get(k)
            if v:
                parts.append(f"- {k.replace('_',' ').title()}: {str(v)[:200]}")
        for note in (overlay.get("style_notes") or [])[:4]:
            parts.append(f"- {note}")
    rules = p.get("communication_rules") or []
    if rules:
        parts.append("Rules:")
        for r in rules[:4]:
            parts.append(f"- {r}")
    examples = []
    for bank in p.get("example_banks", []):
        if bank.get("channel") == "imessage":
            for ex in bank.get("examples", [])[:5]:
                examples.append(ex)
            break
    if examples:
        parts.append("Examples of how you text:")
        for ex in examples:
            inc = ex.get("incoming", "")[:120]
            resp = ex.get("response", "")[:160]
            parts.append(f"- Them: {inc!r}\n  You: {resp!r}")
    parts.append(
        "Reply as yourself. ONE message. No markdown, no bullet lists. "
        "Never start with 'Depending on', 'Here are', 'Certainly', 'Absolutely'."
    )
    return "\n\n".join(parts)


def post_chat(url: str, body: dict, timeout: int = 120) -> tuple[str, float, str]:
    data = json.dumps(body).encode("utf-8")
    req = request.Request(url, data=data, method="POST",
                          headers={"Content-Type": "application/json"})
    t0 = time.time()
    try:
        with request.urlopen(req, timeout=timeout) as r:
            resp = json.loads(r.read())
        try:
            return resp["choices"][0]["message"]["content"].strip(), time.time() - t0, ""
        except (KeyError, IndexError):
            return "", time.time() - t0, str(resp)[:120]
    except (error.URLError, error.HTTPError, json.JSONDecodeError, ConnectionError) as e:
        return "", time.time() - t0, str(e)[:200]


def run_a(prompt: str) -> tuple[str, float, str]:
    return post_chat(GATEWAY, {
        "model": "gemma-4-26b",
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": 80, "temperature": 0.9,
    })


def run_b(prompt: str, persona_prompt: str) -> tuple[str, float, str]:
    return post_chat(MLX_DIRECT, {
        "model": "gemma-4-26b",
        "messages": [
            {"role": "system", "content": persona_prompt},
            {"role": "user", "content": prompt},
        ],
        "max_tokens": 80, "temperature": 0.9,
    })


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--suite", default="eval_suites/imessage_humanness.json")
    p.add_argument("--out", default="/tmp/memory_ablation.json")
    p.add_argument("--n", type=int, default=4, help="N tasks (from start of suite)")
    args = p.parse_args()

    suite = json.loads(Path(args.suite).read_text())
    tasks = suite["tasks"][: args.n]
    persona_prompt = build_compact_persona_prompt()
    print(f"=== MEMORY ABLATION on {suite['name']} (n={len(tasks)}) ===")
    print(f"  A = gateway --with-agent (full memory)")
    print(f"  B = direct MLX with compact persona only ({len(persona_prompt)} chars)")
    print()
    results = []
    for t in tasks:
        prompt = t.get("prompt", "")
        print(f"--- {t.get('id')} ---")
        print(f"  prompt: {prompt[:100]!r}")
        a_text, a_elapsed, a_err = run_a(prompt)
        a_shape = classify(a_text, channel="imessage")
        print(f"  A (full):    score={a_shape['score']:.2f} len={a_shape['len']:>3} "
              f"{a_elapsed:5.1f}s | {a_text[:90]!r}")
        b_text, b_elapsed, b_err = run_b(prompt, persona_prompt)
        b_shape = classify(b_text, channel="imessage")
        print(f"  B (persona): score={b_shape['score']:.2f} len={b_shape['len']:>3} "
              f"{b_elapsed:5.1f}s | {b_text[:90]!r}")
        results.append({
            "task_id": t.get("id"),
            "prompt": prompt,
            "A_full": {"text": a_text, "elapsed_s": a_elapsed, "shape": a_shape, "error": a_err},
            "B_persona": {"text": b_text, "elapsed_s": b_elapsed, "shape": b_shape, "error": b_err},
        })
        print()
    # Aggregate
    a_scores = [r["A_full"]["shape"]["score"] for r in results]
    b_scores = [r["B_persona"]["shape"]["score"] for r in results]
    a_lens = [r["A_full"]["shape"]["len"] for r in results]
    b_lens = [r["B_persona"]["shape"]["len"] for r in results]
    a_lat = [r["A_full"]["elapsed_s"] for r in results]
    b_lat = [r["B_persona"]["elapsed_s"] for r in results]
    print("=== AGGREGATE ===")
    print(f"  {'metric':<20} {'A (full)':>10} {'B (persona-only)':>20} {'A - B':>10}")
    am = statistics.mean(a_scores) if a_scores else 0
    bm = statistics.mean(b_scores) if b_scores else 0
    print(f"  {'mean shape':<20} {am:>10.3f} {bm:>20.3f} {am-bm:>+10.3f}")
    am = statistics.mean(a_lens) if a_lens else 0
    bm = statistics.mean(b_lens) if b_lens else 0
    print(f"  {'mean len':<20} {am:>10.1f} {bm:>20.1f} {am-bm:>+10.1f}")
    am = statistics.mean(a_lat) if a_lat else 0
    bm = statistics.mean(b_lat) if b_lat else 0
    print(f"  {'mean latency (s)':<20} {am:>10.1f} {bm:>20.1f} {am-bm:>+10.1f}")
    print()
    if a_scores and b_scores:
        delta = statistics.mean(a_scores) - statistics.mean(b_scores)
        if abs(delta) < 0.05:
            print("VERDICT: memory context appears NOT to materially change outputs.")
            print("  → Layer 2 refactor (RAG-targeted retrieval) reclaims tokens + latency.")
        elif delta > 0.05:
            print(f"VERDICT: full memory context HELPS by +{delta:.3f} mean-shape.")
            print("  → Layer 2 refactor target: keep the parts that help, drop the rest.")
        else:
            print(f"VERDICT: full memory context HURTS by {delta:.3f} mean-shape.")
            print("  → Layer 2 priority: identify which memory field is poisoning outputs.")
    Path(args.out).write_text(json.dumps(results, indent=2))
    print(f"Full results: {args.out}")


if __name__ == "__main__":
    main()
