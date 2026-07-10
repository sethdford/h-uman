#!/usr/bin/env python3
"""Export an expert-steering spec from a MoE expert-activation profile.

Takes the profile npz written by moe_expert_profiler.py, selects the top
trait-differential experts (two-proportion z-score over pos/neg routing
frequencies), and writes a compact <trait>_experts.npz the mlx-server
steering loader understands:

  layers   (N,) int32   decoder-layer index per selected expert
  experts  (N,) int32   expert index within the layer
  signs    (N,) float32 +1 = trait-positive expert, -1 = trait-negative
  base_bias ()  float32 router-logit bias at coefficient 1.0

Usage:
  python3 scripts/moe_export_expert_steering.py \
      --profile .../results/moe_profile_warmth_human.npz \
      --out .../vectors/warmth_experts.npz [--top-n 24] [--base-bias 2.0]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from moe_expert_steering_smoke import pick_experts  # noqa: E402


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--span", default="response",
                    choices=["thought", "visible", "response"])
    ap.add_argument("--top-n", type=int, default=24)
    ap.add_argument("--z-min", type=float, default=4.0)
    ap.add_argument("--df-min", type=float, default=0.02)
    ap.add_argument("--base-bias", type=float, default=2.0,
                    help="router-logit bias applied at steering coefficient 1.0")
    args = ap.parse_args()

    profile = np.load(args.profile)
    meta = json.loads(str(profile["meta"]))
    picked = pick_experts(profile, args.span, args.top_n, args.z_min, args.df_min)

    layers, experts, signs = [], [], []
    for li in sorted(picked):
        for e, s in picked[li]:
            layers.append(li)
            experts.append(e)
            signs.append(s)

    out = Path(args.out)
    np.savez(
        out,
        layers=np.array(layers, dtype=np.int32),
        experts=np.array(experts, dtype=np.int32),
        signs=np.array(signs, dtype=np.float32),
        base_bias=np.float32(args.base_bias),
        meta=json.dumps({**meta, "span": args.span, "top_n": args.top_n,
                         "z_min": args.z_min, "df_min": args.df_min,
                         "profile": str(args.profile)}),
    )
    n_pos = sum(1 for s in signs if s > 0)
    print(f"[export] {out}: {len(layers)} experts on layers "
          f"{sorted(set(layers))} ({n_pos} positive, {len(signs)-n_pos} negative), "
          f"base_bias {args.base_bias}")


if __name__ == "__main__":
    main()
