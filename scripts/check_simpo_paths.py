#!/usr/bin/env python3
# Run with ~/.human/venvs/mlxtune312/bin/python (needs mlx, mlx_lm, mlx_tune). Loads no weights.
# 2026-09-13 result: KV-cache fork consistent; shared path = response-only logp, plain path = prompt+response,
# both / FULL length; beta 2.0 grad-norm 67x beta 0.05; one SGD step at lr 5e-6: beta 0.05 delta -1e-7, beta 2.0 delta -6e-4.
(batch>=2) path, and how large is the gradient under beta 0.05 vs 2.0?
No weights loaded — a random 2-layer llama with vocab 64."""
import mlx.core as mx, mlx.nn as nn
from mlx_lm.models.llama import Model, ModelArgs
from mlx_tune.losses import simpo_loss, compute_log_probs_pair_shared_prefix, compute_log_probs_with_lengths, _can_use_shared_prefix
mx.random.seed(0)
args = ModelArgs(model_type="llama", hidden_size=32, num_hidden_layers=2, intermediate_size=64,
                 num_attention_heads=2, num_key_value_heads=2, rms_norm_eps=1e-5, vocab_size=64,
                 head_dim=16, max_position_embeddings=128)
m = Model(args); mx.eval(m.parameters())
PL, CL, RL = 6, 14, 12               # shared prompt 6 tokens; chosen 14, rejected 12 (incl. prompt)
prompt = mx.random.randint(0, 64, (1, PL))
chosen = mx.concatenate([prompt, mx.random.randint(0, 64, (1, CL-PL))], axis=1)
rejected = mx.concatenate([prompt, mx.random.randint(0, 64, (1, RL-PL)), mx.zeros((1, CL-RL), dtype=mx.int32)], axis=1)  # right-pad to CL
cl, rl = mx.array([CL]), mx.array([RL])
assert _can_use_shared_prefix(CL, RL, PL, 1)

# (1) KV-cache fork consistency: response-token logp via the shared path must equal a plain full forward restricted to response positions
lc_s, lr_s = compute_log_probs_pair_shared_prefix(m, chosen, rejected, CL, RL, PL)
def resp_logp(ids, L):
    logits = m(ids[:, :-1]); lp = -nn.losses.cross_entropy(logits, ids[:, 1:], reduction="none")
    return lp[:, PL-1:L-1].sum(axis=-1)
lc_p, lr_p = resp_logp(chosen, CL), resp_logp(rejected, RL)
print(f"[1] shared-prefix response logp  chosen {lc_s.item():.5f} vs plain-forward {lc_p.item():.5f}   rejected {lr_s.item():.5f} vs {lr_p.item():.5f}")
print(f"    KV fork consistent: {abs(lc_s.item()-lc_p.item())<1e-3 and abs(lr_s.item()-lr_p.item())<1e-3}")

# (2) what each PATH feeds the loss: shared = response-only logp; plain = ALL tokens (prompt included)
full_c = compute_log_probs_with_lengths(m, chosen, cl); full_r = compute_log_probs_with_lengths(m, rejected, rl)
print(f"[2] plain path (batch>=2) sums prompt+response: chosen {full_c.item():.4f} (response-only {lc_p.item():.4f}); both then divided by FULL length {CL}/{RL}")
for beta in (0.05, 2.0):
    L_shared, _ = simpo_loss(m, chosen, rejected, cl, rl, beta=beta, gamma=0.5, prompt_length=PL, chosen_length_py=CL, rejected_length_py=RL)
    L_plain, _ = simpo_loss(m, chosen, rejected, cl, rl, beta=beta, gamma=0.5)
    print(f"[3] beta={beta:<4} loss shared(b=1)={L_shared.item():.5f}  plain(b>=2)={L_plain.item():.5f}   (ln2={0.693147:.5f})")

# (4) gradient magnitude vs beta on the production (shared) path
def loss_fn(model, beta):
    l, _ = simpo_loss(model, chosen, rejected, cl, rl, beta=beta, gamma=0.5, prompt_length=PL, chosen_length_py=CL, rejected_length_py=RL); return l
from mlx.utils import tree_flatten
norms = {}
for beta in (0.05, 2.0):
    lg = nn.value_and_grad(m, lambda model: loss_fn(model, beta))
    l, g = lg(m); mx.eval(g)
    norms[beta] = sum((v**2).sum().item() for _, v in tree_flatten(g)) ** 0.5
    print(f"[4] beta={beta:<4} loss={l.item():.5f} grad-norm={norms[beta]:.6f}")
print(f"    grad-norm ratio 2.0/0.05 = {norms[2.0]/max(norms[0.05],1e-12):.1f}x")
# (5) does the loss actually respond to a step? one SGD step on the shared path at lr 5e-6 vs 2e-4
import mlx.optimizers as optim
for beta, lr in ((0.05, 5e-6), (2.0, 5e-6), (2.0, 2e-4)):
    mm = Model(args); mx.eval(mm.parameters()); opt = optim.SGD(learning_rate=lr)
    lg = nn.value_and_grad(mm, lambda model: loss_fn(model, beta))
    l0, g = lg(mm); opt.update(mm, g); mx.eval(mm.parameters()); l1, _ = lg(mm)
    print(f"[5] beta={beta:<4} lr={lr:<6} loss {l0.item():.6f} -> {l1.item():.6f}  (delta {l1.item()-l0.item():+.2e})")
