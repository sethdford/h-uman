# Sprint 8 Smoke Run #3 — Early-stopping sweep: SFT vs 4 DPO checkpoints

**Date:** 2026-05-16 (continuation of Smoke Run #2)
**Goal:** Find the empirical DPO early-stopping sweet spot by training to
iter 80 with checkpoints every 20, then evaluating ALL FOUR checkpoints
against the same SFT baseline.

## What ran

- SFT baseline: `seth-sft-iter80` (rank=32, layers=16, US-8.6 fix in place)
- Fresh DPO run: 80 iters, `--save-every 20`, `--steps-per-eval 20`
- 4 DPO checkpoints saved: iter 20, 40, 60, 80
- 30-prompt held-out eval via `mlx_lm.generate` against each adapter
- Each scored vs SFT via `human ml lora-ab`
- Failure modes counted: `<pad>` token leakage, very-short responses

## Results table

```
Checkpoint        Fidelity   Δ vs SFT   <pad> fails   short fails
─────────────────────────────────────────────────────────────────────
SFT baseline      0.600      0.000      0/30          0/30
iter 20 DPO       0.592      -0.008     22/30 (73%)   0/30
iter 40 DPO       0.604      +0.003     23/30 (77%)   1/30
iter 60 DPO       0.619      +0.019     12/30 (40%)   0/30  ← BEST
iter 80 DPO       0.599      -0.001     21/30 (70%)   3/30
```

Reference from Smoke #2 (same SFT, but DPO continued to iter 200):
```
iter 200 DPO      0.647      +0.046     24/30 (80%)   ?      Overfit
```

## Key findings

### Finding 1: Iter 60 is the empirical sweet spot

It's the only DPO checkpoint that:
- Has a POSITIVE fidelity delta (+0.019)
- Has substantially BETTER coherence (40% pad-fail vs 70-80% elsewhere)

The other checkpoints score lower fidelity AND are more broken. There's
a U-shape in failure rate (high at both extremes, lower in middle) that
the val-accuracy metric did not predict.

### Finding 2: Training metrics MISLED us about the sweet spot

The training-loop val accuracy peaked at iter 20/40 (89.8%) and dropped
at iter 60 (78.1%). Based on val accuracy alone, the "right" early-stopping
point would have been iter 40. But generation quality at iter 40 is WORSE
(23/30 pad fails) than at iter 60 (12/30).

**The classification-loss validation metric is NOT predictive of
generation quality.** This is a meta-finding for Sprint 8 US-8.10:
the early-stopping signal must be generation-time, not classification-time.

### Finding 3: Even the best is not production-ready

Iter 60 has:
- +0.019 fidelity (well below the +0.05 production gate)
- 12/30 = **40% pad-leakage failure rate** — not shippable

So while we now know the loop CAN produce a positive delta with cleaner
output, the production threshold (+0.05) is unmet and the failure rate
is too high.

### Finding 4: Smoke #2's +0.046 was misleading

The original 200-iter run scored +0.046 (just below the +0.05 gate) — but
80% of those outputs were `<pad>`-broken garbage. The synthetic fingerprint
rewarded the broken output's terseness and lowercase ratio without
penalizing the gibberish.

**This empirically confirms FU-7.6.b CRITICAL:** the synthetic
lexical-surface fingerprint cannot distinguish broken from coherent output.

## Updated Sprint 8 backlog

| Story | Status after Smoke #3 |
|---|---|
| US-8.2 (NLL backend) | **MEGA-CRITICAL P0** — without coherence-aware scoring, the gate misranks adapters (200-iter scored higher than 60-iter despite being more broken) |
| US-8.10 (DPO early stopping) | **REDEFINE** — val accuracy is the wrong signal; must use generation-time signal (small-sample pad-fail rate during training every N iters) |
| US-8.11 (NEW) | Coherence-rate-aware early-stopping gate; train loop should sample N=10 generations every 20 iters and abort if `<pad>` rate > 30% |
| US-8.12 (NEW) | Adapter promotion gate must require BOTH (fidelity Δ > 0.05) AND (coherence_failure_rate < 0.10) — a checkpoint that maximizes one while failing the other is not shippable |
| US-8.7 (longer training) | **DEMOTED again** — even iter 60 of 80 is past the sweet spot per generation metrics; problem is not under-training |
| US-8.6 | **DONE** (490b6359) |

## The U-shape: iter 20 vs 60 vs 80

Why is iter 20 worse than iter 60?
- At iter 20, DPO has only seen ~80 training examples
- The model has shifted toward chosen but not enough to be coherent under DPO's
  KL penalty against the reference
- It's in an unstable middle zone where the policy diverges from reference
  without consistent direction

Why is iter 80 worse than iter 60?
- At iter 80, `chosen_r=-8.867` — the model is pushing ALL completions
  (chosen AND rejected) away from reference, just rejected further
- This is reward hacking
- Output generation collapses into pad-token loops

Iter 60 is the brief plateau between these two failure modes.

## Reproducibility

```bash
# Re-run the sweep (assuming SFT baseline already at seth-sft-iter80):
python3 -m mlx_lm_lora.train --train --train-mode dpo --train-type lora \
  --model mlx-community/gemma-4-e2b-it-4bit \
  --data ~/.human/training-data/dpo_finetune \
  --adapter-path ~/.human/training-data/adapters/seth-dpo-early80 \
  --resume-adapter-file ~/.human/training-data/adapters/seth-sft-iter80/adapters.safetensors \
  --reference-model-path mlx-community/gemma-4-e2b-it-4bit \
  --iters 80 --batch-size 2 --num-layers 16 --learning-rate 1e-5 \
  --max-seq-length 2048 --grad-checkpoint \
  --beta 0.1 --dpo-cpo-loss-type sigmoid \
  --save-every 20 --steps-per-report 5 --steps-per-eval 20 \
  -c ~/.human/training-data/adapters/seth-sprint8-prod-sft-e2b/sft_train_config.yaml

# Then split per-iter into separate dirs (see ab_sweep.py header for the trick)
# and run scripts/ab_sweep.py for the full Pareto table.
```

## Honest one-line verdict

**Iter 60 is the sweet spot empirically — but "sweet spot" produces 40% broken outputs at +0.019 fidelity. Sprint 8 must wire coherence-aware scoring (US-8.2 + US-8.11 + US-8.12) before any "DPO works" claim is defensible.**
