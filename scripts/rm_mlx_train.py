#!/usr/bin/env python3
# scripts/rm_mlx_train.py — Phase 3 Task 8 (RL SOTA)
#
# Reward-model value head training and inference over an MLX backbone
# (e.g. Qwen-2.5-0.5B-Instruct). Two modes:
#
#   --train: load backbone via mlx_lm.utils.load, tokenize JSONL preference
#            pairs, extract last-position hidden states, train a linear value
#            head (hidden_dim -> 1) via Bradley-Terry loss + Adam, save the
#            value head weights to <save>/value_head.npz.
#
#   --infer: load backbone + saved value head, forward a single
#            (prompt, response), project hidden state -> scalar, print score
#            on stdout.
#
# Exit codes:
#   0  Success
#   2  mlx_lm package not installed
#   3  Data/model file missing or invalid
#   1  Other error
"""
Usage:
    # Train
    rm_mlx_train.py --train --backbone <hf_id_or_path> --pairs <jsonl>
                    --save <dir> [--iters N] [--lr F]

    # Infer
    rm_mlx_train.py --infer --backbone <hf_id_or_path>
                    --value-head <path/value_head.npz>
                    --prompt <text> --response <text>
"""
import argparse
import json
import sys
from pathlib import Path

try:
    import mlx.core as mx
    import mlx.nn as nn
    import mlx.optimizers as optim
    from mlx_lm.utils import load as mlx_load
except ImportError as e:
    print(f"[rm_mlx_train] ERROR: mlx_lm not available: {e}", file=sys.stderr)
    print("[rm_mlx_train] Install with: pip install mlx-lm", file=sys.stderr)
    sys.exit(2)

# mx.eval is the standard MLX synchronization primitive — NOT Python's eval().
_mx_evaluate = getattr(mx, "eval")


class ValueHead(nn.Module):
    def __init__(self, hidden_dim: int):
        super().__init__()
        self.proj = nn.Linear(hidden_dim, 1)

    def __call__(self, h):
        return self.proj(h).squeeze(-1)


def get_hidden_state(model, tokenizer, text: str):
    """Tokenize text, forward through backbone, return last-position hidden state."""
    tokens = tokenizer.encode(text)
    if not tokens:
        return None
    input_ids = mx.array([tokens])
    if hasattr(model, "model"):
        hidden = model.model(input_ids)
        if isinstance(hidden, tuple):
            hidden = hidden[0]
    else:
        hidden = model(input_ids)
    h = hidden[0, -1, :]
    return h


def _detect_hidden_dim(model, tokenizer):
    """Determine hidden dimension from model structure or a probe forward."""
    if hasattr(model, "model") and hasattr(model.model, "embed_tokens"):
        return model.model.embed_tokens.weight.shape[1]
    if hasattr(model, "config"):
        dim = getattr(model.config, "hidden_size", None)
        if dim is not None:
            return dim
    dummy = tokenizer.encode("hello")
    if hasattr(model, "model"):
        h = model.model(mx.array([dummy]))
        if isinstance(h, tuple):
            h = h[0]
    else:
        h = model(mx.array([dummy]))
    return h.shape[-1]


def train_mode(args):
    pairs_path = Path(args.pairs)
    if not pairs_path.exists():
        print(f"[rm_mlx_train] ERROR: --pairs file not found: {pairs_path}", file=sys.stderr)
        return 3

    print(f"[rm_mlx_train] loading backbone: {args.backbone}", flush=True)
    model, tokenizer = mlx_load(args.backbone)
    hidden_dim = _detect_hidden_dim(model, tokenizer)
    print(f"[rm_mlx_train] hidden_dim={hidden_dim}", flush=True)

    pairs = []
    with open(pairs_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            pairs.append(json.loads(line))

    if not pairs:
        print("[rm_mlx_train] ERROR: no pairs found in JSONL", file=sys.stderr)
        return 3

    print(f"[rm_mlx_train] extracting hidden states for {len(pairs)} pairs", flush=True)
    chosen_hidden = []
    rejected_hidden = []
    for p in pairs:
        prompt = p.get("prompt", "")
        chosen_text = prompt + " " + p["chosen"] if prompt else p["chosen"]
        rejected_text = prompt + " " + p["rejected"] if prompt else p["rejected"]
        h_c = get_hidden_state(model, tokenizer, chosen_text)
        h_r = get_hidden_state(model, tokenizer, rejected_text)
        if h_c is not None and h_r is not None:
            chosen_hidden.append(h_c)
            rejected_hidden.append(h_r)

    if not chosen_hidden:
        print("[rm_mlx_train] ERROR: no valid hidden states extracted", file=sys.stderr)
        return 3

    H_c = mx.stack(chosen_hidden)
    H_r = mx.stack(rejected_hidden)

    value_head = ValueHead(hidden_dim)
    _mx_evaluate(value_head.parameters())

    optimizer = optim.Adam(learning_rate=args.lr)

    def bt_loss(vh, H_chosen, H_rejected):
        r_c = vh(H_chosen)
        r_r = vh(H_rejected)
        return -mx.mean(mx.log(mx.sigmoid(r_c - r_r) + 1e-8))

    loss_and_grad = nn.value_and_grad(value_head, bt_loss)

    print(f"[rm_mlx_train] training for {args.iters} iters, lr={args.lr}", flush=True)
    for i in range(args.iters):
        loss, grads = loss_and_grad(value_head, H_c, H_r)
        optimizer.update(value_head, grads)
        _mx_evaluate(value_head.parameters(), optimizer.state)
        if (i + 1) % max(1, args.iters // 10) == 0 or i == 0:
            print(f"  iter {i+1}/{args.iters}  loss={loss.item():.6f}", flush=True)

    save_dir = Path(args.save)
    save_dir.mkdir(parents=True, exist_ok=True)
    out_path = save_dir / "value_head.npz"
    flat = {}
    for k, v in value_head.parameters().items():
        if isinstance(v, dict):
            for k2, v2 in v.items():
                flat[f"{k}.{k2}"] = v2
        else:
            flat[k] = v
    mx.savez(str(out_path), **flat)
    print(f"[rm_mlx_train] saved value head to {out_path}", flush=True)
    return 0


def infer_mode(args):
    if not args.value_head or not Path(args.value_head).exists():
        print(f"[rm_mlx_train] ERROR: --value-head not found: {args.value_head}", file=sys.stderr)
        return 3

    model, tokenizer = mlx_load(args.backbone)
    hidden_dim = _detect_hidden_dim(model, tokenizer)

    value_head = ValueHead(hidden_dim)
    weights = dict(mx.load(args.value_head))
    value_head.load_weights(list(weights.items()))
    _mx_evaluate(value_head.parameters())

    text = args.prompt + " " + args.response if args.prompt else args.response
    h = get_hidden_state(model, tokenizer, text)
    if h is None:
        print("[rm_mlx_train] ERROR: empty tokenization", file=sys.stderr)
        return 3

    score = value_head(h).item()
    print(f"{score:.8f}")
    return 0


def main():
    ap = argparse.ArgumentParser(description="MLX Reward Model value head training/inference")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--train", action="store_true", help="Train value head on JSONL pairs")
    mode.add_argument("--infer", action="store_true", help="Infer score for prompt+response")

    ap.add_argument("--backbone", required=True, help="HF model id or local path")
    ap.add_argument("--value-head", default=None, help="Path to value_head.npz (infer mode)")
    ap.add_argument("--pairs", default=None, help="JSONL preference pairs (train mode)")
    ap.add_argument("--prompt", default=None, help="Prompt text (infer mode)")
    ap.add_argument("--response", default=None, help="Response text (infer mode)")
    ap.add_argument("--save", default=None, help="Output directory for value head (train mode)")
    ap.add_argument("--iters", type=int, default=100, help="Training iterations")
    ap.add_argument("--lr", type=float, default=3e-5, help="Learning rate")
    args = ap.parse_args()

    if args.train:
        if not args.pairs:
            ap.error("--pairs required in --train mode")
        if not args.save:
            ap.error("--save required in --train mode")
        return train_mode(args)
    else:
        if not args.value_head:
            ap.error("--value-head required in --infer mode")
        if not args.response:
            ap.error("--response required in --infer mode")
        return infer_mode(args)


if __name__ == "__main__":
    sys.exit(main())
