#!/usr/bin/env python3
"""RAG-on vs RAG-off A/B through the LIVE mlx-server (port 8741).

Both arms hit the SAME live adapter (v4-repair); the only difference is whether
the system prompt carries a RAG grounding block of the user's most-similar real
past messages (retrieved from voice_corpus.jsonl). Scores each arm's replies for
stylistic fidelity to the real-message corpus via fidelity_axes.decompose.

The casual register is the known weak spot (ideal-corpus regression finding), so
casual + substantive prompts are scored separately.

Usage:
  scripts/rag_ab_live.py [--corpus ~/.human/voice_corpus.jsonl] [--k 3]
      [--mlx-url http://127.0.0.1:8741/v1] [--max-tokens 80]
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

CASUAL = ["hey whatup", "cool cool i'll let you know", "wyd",
          "did u see that article about machine learning", "you free this weekend?",
          "ugh today was rough", "wanna grab dinner thursday", "lol nice"]
SUBSTANTIVE = ["honestly how do you decide what's worth doing",
               "what do you actually think about all this AI stuff",
               "why do you think that startup failed",
               "how are you feeling about the move"]


def generate(mlx_url, system, user, max_tokens):
    body = json.dumps({"model": "gemma-4-31b-it-4bit",
                       "messages": [{"role": "system", "content": system},
                                    {"role": "user", "content": user}],
                       "max_tokens": max_tokens, "temperature": 0.7}).encode()
    req = urllib.request.Request(mlx_url.rstrip("/") + "/chat/completions", data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=120) as r:
        d = json.load(r)
    return (d["choices"][0]["message"]["content"] or "").strip()


def run(corpus, prompts, mlx_url, k, max_tokens):
    off, on = [], []
    for p in prompts:
        retrieved = retrieve_similar(p, corpus, k)
        rag_block = build_rag_fewshot(p, retrieved)
        off_reply = generate(mlx_url, SYSTEM, p, max_tokens)
        on_reply = generate(mlx_url, SYSTEM + "\n\n" + rag_block, p, max_tokens)
        off.append(off_reply)
        on.append(on_reply)
        print(f"  [{p[:34]:34}]\n     off: {off_reply[:70]!r}\n     on : {on_reply[:70]!r}",
              flush=True)
    return off, on


def score(corpus, replies):
    replies = [r for r in replies if r.strip()]
    if not replies:
        return 0.0
    return fx.decompose(corpus, replies)["aggregate"]


def main():
    ap = argparse.ArgumentParser(description="RAG-on vs RAG-off live A/B")
    ap.add_argument("--corpus", type=Path, default=Path.home() / ".human" / "voice_corpus.jsonl")
    ap.add_argument("--k", type=int, default=3)
    ap.add_argument("--mlx-url", default="http://127.0.0.1:8741/v1")
    ap.add_argument("--max-tokens", type=int, default=80)
    ap.add_argument("--output-json", type=Path, default=None)
    args = ap.parse_args()

    if not args.corpus.exists():
        print(f"[error] corpus not found: {args.corpus}", file=sys.stderr)
        return 1
    corpus = fx.load_messages(args.corpus)
    print(f"[info] corpus: {len(corpus)} messages\n", flush=True)

    results = {}
    for label, prompts in (("casual", CASUAL), ("substantive", SUBSTANTIVE)):
        print(f"=== {label} ===", flush=True)
        off, on = run(corpus, prompts, args.mlx_url, args.k, args.max_tokens)
        s_off, s_on = score(corpus, off), score(corpus, on)
        empt_off = sum(1 for r in off if not r.strip())
        empt_on = sum(1 for r in on if not r.strip())
        results[label] = {"rag_off": round(s_off, 4), "rag_on": round(s_on, 4),
                          "delta": round(s_on - s_off, 4),
                          "empties_off": empt_off, "empties_on": empt_on, "n": len(prompts)}
        print(f"  -> {label}: RAG-off {s_off:.4f} | RAG-on {s_on:.4f} | "
              f"delta {s_on - s_off:+.4f} (empties off={empt_off} on={empt_on})\n", flush=True)

    print(json.dumps(results, indent=2))
    if args.output_json:
        args.output_json.write_text(json.dumps(results, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
