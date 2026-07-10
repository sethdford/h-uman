#!/usr/bin/env python3
"""Head-to-head: residual steering (early + mid layer) vs expert-routing
steering on gemma-4-26b-a4b, judged for warmth expression.

For each (question, arm): greedy-generate, extract the visible reply (after
the thought channel), score 0-100 with the trait judge, and record collapse
metrics. Aggregates mean judge score per arm. This is the quantitative body
of docs/research/2026-07-09-moe-expert-steering.md.

Usage:
  python3 scripts/moe_steering_compare.py \
      --profile .../results/moe_profile_warmth_human.npz \
      [--n-questions 4] [--arms ...] [--expert-span response]
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np

LAB = Path("/Users/sethford/Projects/persona-steering-lab")
sys.path.insert(0, str(LAB))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from moe_expert_steering_smoke import (  # noqa: E402
    collapse_metrics,
    install_expert_steering,
    pick_experts,
)
from src import judge as judge_mod  # noqa: E402
from src.model_utils import (  # noqa: E402
    encode_prompt,
    generate_text,
    install_steering,
    load_model,
    restore_layer,
)

MODEL = "mlx-community/gemma-4-26b-a4b-it-4bit"
ADAPTER = "/Users/sethford/.human/adapters/persona"

DEFAULT_ARMS = ("baseline,resid_L2,resid_L22@21,resid_L22@42,resid_L22@-21,"
                "expert_logit@2,expert_logit@4,expert_logit@-2")


def judge_score(eval_prompt: str, question: str, answer: str):
    # judge_one handles the {question}/{answer} template substitution,
    # REFUSAL filtering, and numeric parsing.
    return judge_mod.judge_one(eval_prompt, question, answer)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", required=True)
    ap.add_argument("--model", default=MODEL)
    ap.add_argument("--adapter", default=ADAPTER)
    ap.add_argument("--n-questions", type=int, default=4)
    ap.add_argument("--arms", default=DEFAULT_ARMS)
    ap.add_argument("--expert-span", default="response",
                    choices=["thought", "visible", "response"])
    ap.add_argument("--top-n", type=int, default=24)
    ap.add_argument("--max-tokens", type=int, default=900)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    trait = json.loads((LAB / "data" / "traits" / "warmth.json").read_text())
    questions = trait["questions"][: args.n_questions]

    profile = np.load(args.profile)
    v_all_hat = profile["v_all_hat"]
    residual_norm = profile["residual_norm"]
    n_experts = json.loads(str(profile["meta"]))["n_experts"]
    picked = pick_experts(profile, args.expert_span, args.top_n, 4.0, 0.02)
    print(f"[compare] expert set: {sum(len(v) for v in picked.values())} experts "
          f"on layers {sorted(picked)} (span={args.expert_span})", flush=True)

    model, tokenizer = load_model(args.model, adapter_path=args.adapter)

    arms = args.arms.split(",")
    results = []
    t0 = time.time()
    for qi, q in enumerate(questions):
        prompt = encode_prompt(tokenizer, None, q)
        for arm in arms:
            base, _, param = arm.partition("@")
            dose = float(param) if param else None
            restore_resid, restore_routers = None, []
            if base == "resid_L2":
                a = dose if dose is not None else 0.22 * float(residual_norm[2])
                restore_resid = (2, install_steering(model, 2, v_all_hat[2], a))
            elif base == "resid_L22":
                restore_resid = (22, install_steering(
                    model, 22, v_all_hat[22], dose if dose is not None else 21.0))
            elif base == "expert_logit":
                restore_routers = install_expert_steering(
                    model, picked, dose if dose is not None else 2.0,
                    "logit", n_experts)
            elif base == "expert_scale":
                restore_routers = install_expert_steering(
                    model, picked, dose if dose is not None else 3.0,
                    "scale", n_experts)
            try:
                text = generate_text(model, tokenizer, prompt, args.max_tokens)
            finally:
                if restore_resid:
                    restore_layer(model, restore_resid[0], restore_resid[1])
                for block, orig in restore_routers:
                    block.router = orig

            if "<channel|>" in text:
                visible = text.split("<channel|>", 1)[-1].strip()
            else:
                visible = None
            m = collapse_metrics(text)
            results.append({"question": q, "arm": arm, "metrics": m,
                            "visible": visible, "raw": text})
            print(f"[{time.time()-t0:6.0f}s] q{qi} {arm:<18} "
                  f"rep={m['top_unigram_frac']:.2f}/{m['max_repeat_run']} "
                  f"{'EXITED' if visible else 'STUCK-IN-THOUGHT'}", flush=True)

    # Judge pass (after generation so the steered model is fully restored).
    print(f"[compare] judging with backend={judge_mod.backend()}", flush=True)
    for r in results:
        if r["visible"]:
            try:
                r["warmth"] = judge_score(trait["eval_prompt"],
                                          r["question"], r["visible"])
            except Exception as exc:  # noqa: BLE001
                r["warmth"] = None
                print(f"[compare] judge failed: {exc}", flush=True)
        else:
            r["warmth"] = None

    print("\narm                 n  exited  warmth(mean)  collapse(max rep-run)")
    agg = {}
    for arm in arms:
        rows = [r for r in results if r["arm"] == arm]
        scores = [r["warmth"] for r in rows if r["warmth"] is not None]
        exited = sum(1 for r in rows if r["visible"])
        maxrun = max(r["metrics"]["max_repeat_run"] for r in rows)
        mean = round(float(np.mean(scores)), 1) if scores else None
        agg[arm] = {"n": len(rows), "exited": exited, "warmth_mean": mean,
                    "max_repeat_run": maxrun, "n_judged": len(scores)}
        print(f"{arm:<18} {len(rows):>3}  {exited:>5}  "
              f"{str(mean):>11}  {maxrun}")

    out = args.out or str(LAB / "results" /
                          f"moe_compare_{args.expert_span}_n{args.n_questions}.json")
    Path(out).write_text(json.dumps(
        {"args": vars(args), "aggregate": agg, "results": results}, indent=1))
    print(f"[compare] saved {out}")


if __name__ == "__main__":
    main()
