# Routing-Aware (Expert-Level) Steering for gemma-4-26b-a4b

**Date:** 2026-07-09 · **Branch:** `feat/mlx-steering` · **Status:** measured; expert mode shipped opt-in, residual remains default

## Question

Residual-stream persona steering was believed unusable at mid/late layers on
the MoE production model (gemma-4-26b-a4b): the 2026-07-05 session observed
token-repetition collapse at L22 (α=21, both directions), forcing the
early-layer (L2) workaround. RASA (arXiv 2602.04448) attributes this failure
to expert-routing disruption; SteerMoE (arXiv 2509.09660, ICLR 2026) proposes
steering at the expert level instead — identify trait-differential experts by
comparing expert-activation frequencies between trait-positive and
trait-negative generations, then bias only those experts' routing.

We built the SteerMoE pipeline end-to-end and measured it head-to-head
against residual steering. **The premise largely inverted**: the collapse
is real but stochastic and only at 2× dose (Finding 2), warmth is strongly
routing-legible (Finding 1, supports SteerMoE), yet expert-level biasing
did not actually steer the trait (Finding 3, honest negative).

## Architecture facts (measured, not assumed)

- All **30 decoder layers** of gemma-4-26b-a4b-it-4bit route: each has its own
  `Router` (128 experts, top-8, per-layer `router.proj` quantized at 8-bit).
- Each layer's MoE block is `h1(dense MLP) + h2(experts)`, and `Router` reads
  the **same post-attention hidden state** that residual injection perturbs —
  the mechanistic basis for the RASA claim.
- Router logits span ≈3 units (σ≈0.5 across experts' mean logits), so a
  ±2-logit bias is a ~4σ intervention at the selection boundary.

## Pipeline (all in `scripts/`, worktree `mlx-steering`)

| Stage | Script | Output |
|---|---|---|
| Profile | `moe_expert_profiler.py` | `results/moe_profile_warmth_human.npz` + summary JSON |
| Select+export | `moe_export_expert_steering.py` | `vectors/warmth_experts.npz` (24 experts, layers 2–26) |
| Smoke/dose sweep | `moe_expert_steering_smoke.py` | collapse metrics + texts per arm |
| Judged comparison | `moe_steering_compare.py` | warmth 0–100 per arm (trait judge) |
| Serving | `mlx-server.py` `"steering_mode": "expert"` | `_BiasedRouter` install/restore per request |

The profiler re-forwards the 36 pos/neg warmth extraction transcripts
(3 pairs × 6 questions × 2 conditions, same encode path as
`persona-steering-lab/src/extract.py`) teacher-forced, recording per-layer
router top-k selections + full recomputed router logits over response tokens,
split into thought-channel vs visible spans. It simultaneously rebuilds the
all-layer residual vectors (`v_all_hat`) — the original extraction only
persisted the L2 `v_hat`, and the L22 vector was needed for the collapse
repro arm.

## Finding 1 — warmth is strongly legible in routing (supports SteerMoE)

Two-proportion z-scores on activation frequency (pos vs neg, 8.5k vs 7.2k
response tokens): ~1,000 (layer, expert) cells pass |z|≥4 & |Δf|≥0.02; the
top experts are heavily differential and concentrate at **mid layers L10–L26**
— exactly where residual steering was believed forbidden:

| Layer/Expert | f(warm) | f(cold) | z |
|---|---|---|---|
| L13 E65 | 0.73 | 0.26 | +59 |
| L16 E40 | 0.56 | 0.14 | +54 |
| L14 E92 | 0.45 | 0.09 | +50 |
| L2 E109 (cold expert) | 0.13 | 0.42 | −42 |
| L22 E101 | 0.63 | 0.32 | +39 |

Caveat: frequency differentials partly reflect lexical differences between
warm/cold texts (routers route tokens). The visible-span profile (408 tokens,
noisier) ranks the same experts, which is reassuring but not dispositive.

## Finding 2 — the L22 collapse is REAL but stochastic and dose-gated, not categorical

With the same-procedure rebuilt L22 warmth vector, greedy generation:

- **α=±21 (the standard 0.22×norm dose): stable on all questions tested**
  (max immediate-repeat run = 1, top-unigram fraction ≤0.06 everywhere) with
  a clean warmth dose-response — "I am so sorry today was such a grind …"
  (+21) vs "I'm here. I'm sorry to hear that work was a struggle." (−21).
- **α=+42 (2×): collapsed on 1 of 4 questions** — the interview-nerves
  question degenerated into `eeeeeeee…` inside the thought channel, the
  same character/token-repetition family as the original 'own own own'
  observation. The other 3 questions produced the strongest warmth of any
  arm ("I've got you, and I'm not going anywhere … You are safe here").

So the 2026-07-05 "L22 is unusable" conclusion over-generalized a real but
**probabilistic, dose-dependent** failure: at the calibrated dose mid-layer
residual steering worked on every question we ran; at 2× it collapses
~25% of the time. The blanket L2-only restriction (and the extractor's
`--max-layer` cap) trades away most of the steering effect (see judge table)
to avoid a risk that is negligible at 1× dose — a per-dose guard (or a
repetition-abort in the server) is the better control.

## Finding 3 — expert steering is collapse-proof but did NOT steer warmth (honest negative)

Router-logit bias on the 24 selected experts (bias = strength × sign,
pre-top-k):

- **Zero repetition collapse at every tested strength** (±2, +4; max
  repeat-run = 1 in all 16 generations) — the SteerMoE stability claim
  holds on this model.
- **But no reliable trait shift.** At +2 the judged warmth went *below*
  baseline (63 vs 75, σ≈20); at −2 it was ≈baseline (73). With n=4 and a
  small local judge this is compatible with "no effect + routing-noise
  quality damage"; it is not compatible with "comparable to residual L22"
  (which moved +12/−15 symmetrically at the same n).
- **Strength 4 has its own failure mode:** the model never exits the thought
  channel (4/4 questions; deliberation elaborates until max-tokens). Same
  failure shape as large residual doses on the formality vector — and the
  mitigation composes: the server's thought-channel gate applies to
  `_BiasedRouter` (bias only after `<channel|>`).

Interpretation: frequency-differential experts are *correlates* of warm
text — routers select them **because** warm tokens are present. Forcing
them into the top-8 on neutral context does not synthesize warmth; it
mildly misroutes. Steering the residual stream instead changes the
representation the router reads, which moves both the content *and* the
routing coherently. (Follow-ups that could rescue expert steering:
visible-span-only expert sets, per-layer subsets, weight-mass-weighted
bias, or SteerMoE's full detector rather than our z-score selector.)

## Judged head-to-head (trait judge gemma-3-4b local, warmth 0–100, greedy, 4 questions)

| arm | exited thought | judged warmth (mean±σ) | max repeat-run |
|---|---|---|---|
| baseline | 4/4 | 75.0 ±10.0 | 1 |
| resid_L2 (shipped workaround) | 4/4 | 77.5 ±8.3 | 1 |
| **resid_L22 α=21** | 4/4 | **86.8 ±7.7** | 1 |
| resid_L22 α=42 | 3/4 | 94.0 ±1.4 (n=3) | **9 (collapse)** |
| resid_L22 α=−21 | 4/4 | 60.0 ±25.2 | 1 |
| expert_logit +2 | 4/4 | 63.0 ±20.4 | 1 |
| expert_logit +4 | 0/4 | — | 1 |
| expert_logit −2 | 4/4 | 73.0 ±20.0 | 1 |

Raw generations + scores: `persona-steering-lab/results/moe_compare_response_n4.json`.
Limitations: n=4 per arm, one trait, greedy decoding, small local judge
(ANTHROPIC_API_KEY unset during the run) — treat as directional, not
publication-grade. The collapse/exit counts are deterministic facts; the
judge means are the noisy part.

## Serving integration (shipped, opt-in)

`scripts/mlx-server.py` now loads `<trait>_experts.npz` specs from the
steering dir into a separate registry and honors `"steering_mode": "expert"`
per request: `_BiasedRouter` wraps each spec layer's `Router`, adds
`coeff × base_bias × sign` to that expert's logit before top-k, and restores
on scope exit. Residual KV vs expert-biased KV are mutually stale, so the
mode is folded into the prompt-cache signature. Thought-channel gating
applies to both modes. Residual mode remains the default; nothing changes
for existing clients. 68/68 unit tests (`scripts/test_mlx_steering.py`),
plus live E2E on a scratch :8747 instance (expert ±1.0 applied, coherent
replies, log line `(expert-routing mode) (thought-gated)`).

Per `feature-gate-requires-measurement.md`: expert mode ships **opt-in
per-request** (the OFF-equivalent); promotion to default requires a blind
A/B on real traffic, which this doc's judge numbers do not substitute for.

## Verdict

- **The premise inverted under measurement.** Mid-layer (L22) residual
  steering at the calibrated 0.22×norm dose is stable and by far the
  strongest warmth lever measured (+12/−15 vs baseline; the shipped L2
  workaround moves +2.5). The collapse is real but only appeared at 2×
  dose, stochastically (1/4 questions).
- **Expert-routing steering, as built (frequency-differential z-score
  selection, ±2-logit bias), is a negative result for trait control**: it
  never collapses, but it also didn't move warmth in the intended
  direction, and at high bias it traps the thought channel. It ships
  opt-in and instrumented, not as the replacement.
- **Recommended production change:** re-extract warmth WITHOUT the
  `--max-layer` early-layer cap, select by alignment (likely mid-layer),
  serve at 1× dose with the thought-gate on, and add a cheap
  repetition-abort guard in the server for the residual tail risk. Gate
  any default-on flip behind the blind A/B per
  `feature-gate-requires-measurement.md`.

## Repro

```bash
# profile (loads a private model copy; never touches :8741/:8743)
python3 scripts/moe_expert_profiler.py --trait warmth_human
# export server spec
python3 scripts/moe_export_expert_steering.py \
  --profile .../results/moe_profile_warmth_human.npz \
  --out .../vectors/warmth_experts.npz
# dose sweep
python3 scripts/moe_expert_steering_smoke.py --profile .../moe_profile_warmth_human.npz \
  --arms "baseline,resid_L22,resid_L22@42,expert_logit,expert_logit@4"
# judged comparison
python3 scripts/moe_steering_compare.py --profile .../moe_profile_warmth_human.npz
```
