#!/usr/bin/env python3
"""Measure bytes-per-token for the SERVING base (GLM) vs the prior base (gemma)
on REAL h-uman text. Read-only: loads tokenizers only, never touches :8741.

Run this when the serving base changes FAMILIES (gemma -> GLM -> ...), then
update the measured figures in include/human/core/tokens.h. Do not change
HU_TOKENS_BYTES_PER_TOKEN to the mean this prints: see that header for why the
constant is deliberately below the mean for a budgeting estimator.

Usage:  ~/Documents/gemma-realtime-1/.venv312/bin/python scripts/measure-bytes-per-token.py
(needs `transformers` and the model snapshots already in the HF cache; loads
tokenizers only, never model weights.)
"""
import json, os, glob, sys
from transformers import AutoTokenizer

def corpus():
    """Real strings the estimator actually sees: persona prompt text + replies."""
    out = []
    # 1. persona JSONs — these become prompt text
    for p in glob.glob(os.path.expanduser('~/.human/personas/*.json'))[:4]:
        try:
            d = json.load(open(p))
            def walk(x):
                if isinstance(x, str) and len(x) > 20: out.append(x)
                elif isinstance(x, dict):
                    for v in x.values(): walk(v)
                elif isinstance(x, list):
                    for v in x: walk(v)
            walk(d)
        except Exception: pass
    # 2. blinded A/B sheet — real Seth messages + real model replies
    try:
        d = json.load(open('data/eval_blinded_ab.json'))
        def walk2(x):
            if isinstance(x, str) and len(x) > 15: out.append(x)
            elif isinstance(x, dict):
                for v in x.values(): walk2(v)
            elif isinstance(x, list):
                for v in x: walk2(v)
        walk2(d)
    except Exception: pass
    return out

texts = corpus()
if not texts:
    print("NO CORPUS — refusing to emit a ratio"); sys.exit(2)

for name, repo in (("GLM-4.5-Air (SERVING)", "mlx-community/GLM-4.5-Air-4bit"),
                   ("gemma-4-31b (PRIOR)",  "mlx-community/gemma-4-31b-it-4bit")):
    try:
        tok = AutoTokenizer.from_pretrained(repo, local_files_only=True, trust_remote_code=True)
    except Exception as e:
        print(f"{name:24s} UNAVAILABLE ({type(e).__name__})"); continue
    tb = sum(len(t.encode('utf-8')) for t in texts)
    tt = sum(len(tok.encode(t, add_special_tokens=False)) for t in texts)
    # per-string ratios, for the spread (a mean can hide a bimodal corpus)
    rs = sorted(len(t.encode())/max(1,len(tok.encode(t, add_special_tokens=False))) for t in texts)
    med = rs[len(rs)//2]
    print(f"{name:24s} n={len(texts):5d} bytes={tb:7d} tokens={tt:7d} "
          f"bytes/token: mean={tb/tt:.3f} median={med:.3f} p10={rs[len(rs)//10]:.2f} p90={rs[9*len(rs)//10]:.2f}")
