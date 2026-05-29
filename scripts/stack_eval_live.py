#!/usr/bin/env python3
"""#1 — Composed-stack fidelity eval: measure the three SOTA personalization legs
TOGETHER vs each alone, through the live steering-enabled server.

The server always serves base + v4-repair LoRA (leg 1). This harness layers:
  - leg 2 (RAG): inject a grounding block of the user's most-similar real
    messages into the system prompt (substantive register only — the measured
    register-conditional finding).
  - leg 3 (steering): pass residual-stream steering coefficients via the
    `steering` field (formality/verbosity in the safe [-1,1] envelope).

Arms compared per register:
  A = LoRA-only         (current prod behavior)
  B = LoRA + RAG        (substantive turns)
  C = LoRA + RAG + steer (the full stack)
Scored by fidelity_axes.decompose against the real-message corpus.

Run against the steering server (default :8742, NOT prod :8741).
Usage: scripts/stack_eval_live.py [--url ...] [--corpus ...] [--output-json ...]
"""
import argparse
import json
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import fidelity_axes as fx
from lora_vs_rag_ab import build_rag_fewshot, retrieve_similar

SYSTEM = ("You are Seth. Reply in his exact texting voice — natural, concise, real "
          "opinions and warmth, lowercase-leaning, no AI-assistant tells.")
CASUAL = ["hey whatup", "cool cool i'll let you know", "wyd", "you free this weekend?",
          "ugh today was rough", "wanna grab dinner thursday", "lol nice", "did u eat yet"]
SUBSTANTIVE = ["honestly how do you decide what's worth doing",
               "what do you actually think about all this AI stuff",
               "why do you think that startup failed", "how are you feeling about the move",
               "what's been on your mind lately"]
# Steering coeffs in the safe envelope. Casual: terser + less formal. Substantive:
# slightly more verbose, neutral formality (let the content breathe).
STEER_CASUAL = {"formality": -0.5, "verbosity": -0.5}
STEER_SUBSTANTIVE = {"formality": -0.3, "verbosity": 0.4}


def gen(url, system, user, steering, max_tokens=90):
    body = {"model": "gemma-4-31b-it-4bit",
            "messages": [{"role": "system", "content": system},
                         {"role": "user", "content": user}],
            "max_tokens": max_tokens, "temperature": 0.7}
    if steering:
        body["steering"] = steering
    req = urllib.request.Request(url.rstrip("/") + "/chat/completions",
                                 data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return (json.load(r)["choices"][0]["message"]["content"] or "").strip()


def score(corpus, replies):
    replies = [r for r in replies if r.strip()]
    return fx.decompose(corpus, replies)["aggregate"] if replies else 0.0


def run_register(url, corpus, prompts, use_rag, steer):
    a, b, c = [], [], []
    for p in prompts:
        rag_block = build_rag_fewshot(p, retrieve_similar(p, corpus, 3)) if use_rag else ""
        sys_rag = SYSTEM + ("\n\n" + rag_block if rag_block else "")
        a.append(gen(url, SYSTEM, p, None))                 # LoRA only
        b.append(gen(url, sys_rag, p, None) if use_rag else a[-1])  # +RAG
        c.append(gen(url, sys_rag, p, steer))               # +RAG +steer
    return a, b, c


def main():
    ap = argparse.ArgumentParser(description="Composed-stack fidelity eval (#1)")
    ap.add_argument("--url", default="http://127.0.0.1:8742/v1")
    ap.add_argument("--corpus", type=Path, default=Path.home() / ".human" / "voice_corpus.jsonl")
    ap.add_argument("--output-json", type=Path, default=None)
    args = ap.parse_args()
    if not args.corpus.exists():
        print(f"[error] corpus not found: {args.corpus}", file=sys.stderr)
        return 1
    corpus = fx.load_messages(args.corpus)
    print(f"[info] corpus {len(corpus)} msgs\n", flush=True)

    results = {}
    # Casual: RAG OFF (register-conditional finding: RAG hurts casual); steer only.
    # Substantive: RAG ON + steer (RAG's measured win).
    for label, prompts, use_rag, steer in (
            ("casual", CASUAL, False, STEER_CASUAL),
            ("substantive", SUBSTANTIVE, True, STEER_SUBSTANTIVE)):
        print(f"=== {label} (rag={'on' if use_rag else 'off'}, steer={steer}) ===", flush=True)
        a, b, c = run_register(args.url, corpus, prompts, use_rag, steer)
        sa, sb, sc = score(corpus, a), score(corpus, b), score(corpus, c)
        results[label] = {"lora_only": round(sa, 4), "lora_rag": round(sb, 4),
                          "lora_rag_steer": round(sc, 4),
                          "stack_delta_vs_lora": round(sc - sa, 4),
                          "n": len(prompts)}
        for p, ra, rc in list(zip(prompts, a, c))[:3]:
            print(f"  {p[:30]:30} | lora: {ra[:45]!r}\n  {'':30} | stack: {rc[:45]!r}", flush=True)
        print(f"  -> lora={sa:.4f}  +rag={sb:.4f}  +rag+steer={sc:.4f}  "
              f"stack_delta={sc - sa:+.4f}\n", flush=True)

    print(json.dumps(results, indent=2))
    if args.output_json:
        args.output_json.write_text(json.dumps(results, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
