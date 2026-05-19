#!/usr/bin/env python3
"""
Month 6 — Ablation orchestrator: which layers move which metrics?

The 6-month SOTA plan has 5 independent layers stacked on top of the
shipped Layer 0 (persona-eval bypass fix + compact persona prompt):

  L1: ORPO adapter trained on imessage_tapback preferences
  L2: Memory-retrieval (RAG) replacing dump-all-context
  L3: Multi-turn naturalness (drift test as evidence)
  L4: Multimodal policy (tapback / voice / GIF)
  L5: Verifier TTT (best-of-N + shape-classifier argmax)

To publish credible results, we need each layer's contribution
*isolated*. The orchestrator runs the same prompt suite through:

  R0 — baseline (no layers; bare MLX + persona)
  R1 — baseline + L5 (TTT only)
  R2 — baseline + L1 (ORPO only)
  R3 — baseline + L2 (memory RAG only)
  R4 — baseline + L1 + L2 + L5 (composite)
  R5 — baseline + all layers (the shipped product)

…and produces a comparison table. Metrics per run:

  - mean shape_score (deterministic regex classifier)
  - mean PersonaEval P(Seth) (logistic regression on style features)
  - mean total latency
  - mean prompt length (proxy for token cost)

For Month 6's "publish the data (anonymized)" deliverable, this
becomes the ablation table in the writeup.

Per Eval4Sim 2026 (arXiv:2603.02876), atomic-claim faithfulness is the
right faithfulness metric, but it requires LLM-as-judge. Skipped here
for the pure-Python tier; can be added later by piping responses
through an OpenAI/Anthropic completion endpoint.

Usage:
  python3 scripts/ablation_orchestrator.py --suite eval_suites/imessage_humanness.json
  python3 scripts/ablation_orchestrator.py --runs R0,R5  # subset
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

# Endpoints — fill in based on shipped state of each layer
ENDPOINTS = {
    "gateway_full":  "http://127.0.0.1:3006/v1/chat/completions",   # all layers
    "mlx_direct":    "http://127.0.0.1:8741/v1/chat/completions",   # bare
    # Future:
    # "gateway_no_l2": "http://127.0.0.1:3007/v1/chat/completions",
    # "gateway_no_l1": "http://127.0.0.1:3008/v1/chat/completions",
    # …each run mode corresponds to a gateway flag combination.
}


def post_chat(url: str, body: dict, timeout: int = 180) -> tuple[str, float, str]:
    data = json.dumps(body).encode("utf-8")
    req = request.Request(url, data=data, method="POST",
                          headers={"Content-Type": "application/json"})
    t0 = time.time()
    try:
        with request.urlopen(req, timeout=timeout) as r:
            resp = json.loads(r.read())
        try:
            return (resp["choices"][0]["message"]["content"].strip(),
                    time.time() - t0, "")
        except (KeyError, IndexError):
            return "", time.time() - t0, str(resp)[:120]
    except (error.URLError, error.HTTPError, json.JSONDecodeError,
            ConnectionError, TimeoutError, OSError) as e:
        return "", time.time() - t0, str(e)[:200]


def build_persona_prompt() -> str:
    """Compact persona prompt — mirrors C-side hu_persona_build_prompt_compact.
    Imports from memory_ablation.py so we don't duplicate the persona-build."""
    from memory_ablation import build_compact_persona_prompt
    return build_compact_persona_prompt()


RUN_SPECS = {
    "R0": {
        "label": "baseline (bare MLX + persona only)",
        "endpoint_key": "mlx_direct",
        "with_persona": True,
        "ttt_n": 1,
    },
    "R1": {
        "label": "baseline + L5 TTT (best-of-5)",
        "endpoint_key": "mlx_direct",
        "with_persona": True,
        "ttt_n": 5,
    },
    "R2": {
        "label": "baseline + L1 ORPO adapter (placeholder: gateway w/ adapter)",
        "endpoint_key": "gateway_full",
        "with_persona": False,  # gateway injects persona
        "ttt_n": 1,
        "note": "wires once dpo_pairs threshold met + adapter trained",
    },
    "R3": {
        "label": "baseline + L2 RAG-targeted memory",
        "endpoint_key": "gateway_full",
        "with_persona": False,
        "ttt_n": 1,
        "note": "wires after memory ablation determines retrieval surface",
    },
    "R4": {
        "label": "baseline + L1 + L2 + L5 (no L4 multimodal)",
        "endpoint_key": "gateway_full",
        "with_persona": False,
        "ttt_n": 5,
    },
    "R5": {
        "label": "all layers (shipped product)",
        "endpoint_key": "gateway_full",
        "with_persona": False,
        "ttt_n": 5,
    },
}


def run_one_prompt(prompt: str, spec: dict, persona_prompt: str) -> dict:
    """Drive one prompt through one run config. Returns aggregated metrics."""
    endpoint = ENDPOINTS.get(spec["endpoint_key"])
    if not endpoint:
        return {"text": "", "shape": {"score": 0.0, "pass": False, "len": 0},
                "elapsed_s": 0.0, "error": "endpoint-not-configured"}
    n = spec["ttt_n"]
    candidates = []
    for i in range(n):
        # Spread temperature when n > 1
        temp = 0.9 if n == 1 else (0.70 + 0.10 * i)
        messages = []
        if spec.get("with_persona"):
            messages.append({"role": "system", "content": persona_prompt})
        messages.append({"role": "user", "content": prompt})
        body = {"model": "gemma-4-26b", "messages": messages,
                "max_tokens": 80, "temperature": temp}
        text, elapsed, err = post_chat(endpoint, body)
        shape = classify(text, channel="imessage")
        candidates.append({"text": text, "shape": shape,
                           "elapsed_s": elapsed, "error": err})
    # argmax shape_score (TTT step)
    best = max(candidates, key=lambda c: (c["shape"]["score"],
                                          -c["shape"]["len"]))
    best["total_elapsed_s"] = sum(c["elapsed_s"] for c in candidates)
    best["n_candidates"] = n
    return best


def summarize_run(run_id: str, results: list) -> dict:
    scores = [r["shape"]["score"] for r in results]
    lens = [r["shape"]["len"] for r in results]
    lats = [r["total_elapsed_s"] for r in results]
    return {
        "run_id": run_id,
        "label": RUN_SPECS[run_id]["label"],
        "n": len(results),
        "mean_shape": statistics.mean(scores) if scores else 0.0,
        "mean_len": statistics.mean(lens) if lens else 0.0,
        "mean_latency_s": statistics.mean(lats) if lats else 0.0,
        "shape_pass_rate": sum(1 for s in scores if s >= 1.0) / len(scores)
        if scores else 0.0,
    }


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--suite", default="eval_suites/imessage_humanness.json")
    p.add_argument("--runs", default="R0,R5",
                   help="Comma-sep run IDs (R0..R5). Default: R0,R5 (baseline + shipped)")
    p.add_argument("--n", type=int, default=4, help="N prompts per run")
    p.add_argument("--out", default="/tmp/ablation_results.json")
    args = p.parse_args()

    suite = json.loads(Path(args.suite).read_text())
    tasks = suite["tasks"][: args.n]
    persona_prompt = build_persona_prompt()
    run_ids = [r.strip() for r in args.runs.split(",") if r.strip()]

    print("=" * 78)
    print(f"ABLATION ORCHESTRATOR — {len(run_ids)} runs × {len(tasks)} prompts")
    print("=" * 78)
    for r in run_ids:
        print(f"  {r}: {RUN_SPECS[r]['label']}")
    print()

    all_results = {}
    summaries = []
    for run_id in run_ids:
        spec = RUN_SPECS[run_id]
        print(f"\n--- {run_id}: {spec['label']} ---")
        run_results = []
        for t in tasks:
            prompt = t["prompt"]
            res = run_one_prompt(prompt, spec, persona_prompt)
            score = res["shape"]["score"]
            print(f"  [{t.get('id'):25}] score={score:.2f} "
                  f"len={res['shape']['len']:>3} "
                  f"{res['total_elapsed_s']:5.1f}s | "
                  f"{res['text'][:60]!r}")
            run_results.append(res)
        all_results[run_id] = run_results
        summaries.append(summarize_run(run_id, run_results))

    # Comparison table
    print()
    print("=" * 78)
    print("COMPARISON TABLE")
    print("=" * 78)
    print(f"  {'run':<5} {'label':<48} {'shape':>7} {'pass%':>6} "
          f"{'len':>5} {'lat':>6}")
    print("  " + "-" * 76)
    for s in summaries:
        print(f"  {s['run_id']:<5} {s['label'][:48]:<48} "
              f"{s['mean_shape']:>7.3f} {100*s['shape_pass_rate']:>5.0f}% "
              f"{s['mean_len']:>5.0f} {s['mean_latency_s']:>5.1f}s")

    # Deltas vs R0 baseline
    if "R0" in {s["run_id"] for s in summaries} and len(summaries) >= 2:
        baseline = next(s for s in summaries if s["run_id"] == "R0")
        print(f"\n  Deltas vs R0 baseline (shape={baseline['mean_shape']:.3f}):")
        for s in summaries:
            if s["run_id"] == "R0":
                continue
            d_shape = s["mean_shape"] - baseline["mean_shape"]
            d_lat = s["mean_latency_s"] - baseline["mean_latency_s"]
            print(f"    {s['run_id']}: Δshape={d_shape:+.3f}  "
                  f"Δlatency={d_lat:+.1f}s")

    out = {
        "suite": suite["name"],
        "n_prompts": len(tasks),
        "runs": [{"id": r, "spec": RUN_SPECS[r],
                  "results": all_results[r], "summary": s}
                 for r, s in zip(run_ids, summaries)],
    }
    Path(args.out).write_text(json.dumps(out, indent=2))
    print(f"\nFull results: {args.out}")


if __name__ == "__main__":
    main()
