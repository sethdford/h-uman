# Sprint 8 Smoke Run #2 — Production-size training with US-8.6 fix

**Date:** 2026-05-16 (continuation of session that closed Sprint 7)
**Compared to Smoke #1:** US-8.6 patch landed (`490b6359`); training uses
rank=32/layers=16 properly via `-c <yaml>` instead of the rejected
`--lora-parameters` CLI flag.

## What ran

```
SFT: mlx_lm_lora.train --train-mode sft --train-type lora
  - Model: mlx-community/gemma-4-e2b-it-4bit
  - Iters: 800 planned, actual 80 (session interrupted)
  - Per-iter time: 3-5s (with 11s validation every 20 iters)
  - rank=32, num-layers=16, QKVO target modules (US-7.4 spec)
  - Loss: 4.974 -> 1.704 at iter 80 (66% reduction in 80 iters)
  - Adapter: 117 MB at iter 80 (rank=32/layers=16 vs 14.6 MB at rank=8/layers=8)

DPO: mlx_lm_lora.train --train-mode dpo --train-type lora
  - Resumed from iter-80 SFT adapter
  - Reference: mlx-community/gemma-4-e2b-it-4bit (base)
  - Iters: 200 planned, completed
  - Per-iter time: ~3-5s
  - beta=0.1, sigmoid loss
  - Iter 1 val: loss 1.914, accuracy 55.7% (chance), margin -0.231
  - Iter 200: loss 0.087, training accuracy 100%, margin 21+
  - chosen_r went NEGATIVE (-13.4 by iter 170) — reward-hacking signal
  - Adapter: 111 MB

Eval: 30 held-out prompts from dpo_finetune/valid.jsonl
  - Generated with mlx_lm.generate, temp=0.0 (deterministic)
  - Both adapters loaded same way; same base model
  - Scored via `human ml lora-ab` (synthetic fingerprint)
```

## Headline numbers

```
SFT mean fidelity:  0.600  (30 prompts, 0 skipped)
DPO mean fidelity:  0.647  (30 prompts, 0 skipped)
Delta:             +0.046  (Sprint 7 gate threshold was +0.05)

Persona baseline mean: 0.691 (upper bound from synthetic fingerprint)
```

**The DPO adapter scores measurably higher than SFT (+0.046, ~7.8% relative).**
This is the FIRST positive delta we've measured end-to-end on real data
with the US-8.6 fix in place.

**BUT** the gate threshold (+0.05) was not met, by a hair. The system
would correctly reject this adapter from auto-promotion.

## The failure-mode breakdown

| Metric | SFT | DPO | Read |
|---|---|---|---|
| `<pad>` token leakage in output | 0/30 | **24/30 (80%)** | DPO is producing gibberish |
| Responses starting lowercase | 11/30 | 11/30 | No micro-style shift |
| Mean response length | 212 chars | 188 chars | DPO slightly terser (Seth-ward) |
| Coherent on hard prompts | Most | ~50% | DPO degrades on out-of-distribution |

**The fidelity score went UP while 80% of outputs are visibly broken.**

## Why this is the most important data point so far

1. **It confirms Sprint 7 auditor §2 (FU-7.6.b critical):** the synthetic
   fingerprint is too weak — it scores broken `<pad>`-leakage output higher
   than coherent assistant output, because the fingerprint only measures
   lexical surface (lowercase ratio, abbreviation ratio, length). It does
   not measure coherence.

2. **It validates Sprint 7 critic findings on US-7.5/7.6:** the
   `check-lora-ab.sh` gate as currently designed gives misleading numbers.
   The +0.046 delta number means nothing without coherence checks.

3. **It shows the loop works mechanically:** mine → SFT → DPO → A/B
   compare end-to-end produces a real number from real adapters. The
   infrastructure debt from Sprint 7 (US-8.6 CLI flag bug) was a real
   blocker; fixing it unblocked everything else.

4. **It reveals the actual hardest problem:** preventing DPO from collapsing
   into reward hacking. At 200 iters on 331 pairs the model overfit. The
   `chosen_r` going negative was the leading indicator. Early stopping or
   much lower learning rate / smaller iter count is needed.

## Sprint 8 implications (updated)

Smoke Run #2 promotes/clarifies the Sprint 8 backlog:

| Story | Updated assessment |
|---|---|
| **US-8.2** (NLL backend) | **CRITICAL P0** — without real perplexity, the gate is misleading. The +0.046 result we just saw should never have been computable on visibly broken output. |
| **US-8.9** (bootstrap personal_model.bin) | **MORE IMPORTANT** — synthetic fingerprint is the wrong upper bound (0.691). Real personal model from Seth's chat would penalize gibberish appropriately. |
| **US-8.7** (longer training run) | **DEMOTED** — turns out 200 iters is too MANY for DPO at this corpus size. Sprint 8 needs to find the sweet spot, not push harder. |
| US-8.10 (NEW) | **DPO early-stopping based on val accuracy / val loss / chosen_r divergence detection** — the warning signals are all there in training output; surface them as gates. |
| US-8.6 (CLI flag) | **DONE** in 490b6359 — verified by this run. |

## Reproducibility

This run produced:
- `~/.human/training-data/adapters/seth-sft-iter80/adapters.safetensors`  (117 MB, SFT only at iter 80)
- `~/.human/training-data/adapters/seth-dpo-iter80plus/adapters.safetensors` (111 MB, DPO 200 iters on top)
- `sprints/sprint-8/evidence/ab30_sft.json` (30 SFT responses)
- `sprints/sprint-8/evidence/ab30_dpo.json` (30 DPO responses)

Eval command:
```bash
./build/human ml lora-ab --persona seth \
  --before sprints/sprint-8/evidence/ab30_sft.json \
  --after sprints/sprint-8/evidence/ab30_dpo.json
# delta: +0.046 (synthetic fingerprint)
```

## Honest one-line verdict

**The loop works. The training works. The gate metric is misleading. Sprint 8 must wire a coherence-aware fidelity score (US-8.2) or every future delta number is potentially deceiving.**
