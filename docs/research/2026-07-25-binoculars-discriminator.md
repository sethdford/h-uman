---
title: "Binoculars persona discriminator — base/adapted cross-perplexity AI-tell"
created: 2026-07-25
status: final
audience: maintainers, research
---

# Binoculars Persona Discriminator — 2026-07-25

Follow-up to the "detector-as-judge" item in `2026-07-25-sota-gap-analysis.md`
(not yet committed at time of writing — link once it lands). Fully-local,
persona-specific statistical AI-tell using the Binoculars cross-perplexity method
(arXiv 2401.12070), with the model pair adapted to h-uman's situation: **base model
vs base+LoRA** instead of two unrelated pretrained models.

## Summary

**The discriminator works — well above the wiring bar.** On the 07-24 nightly's 47
perfectly-paired trials (94 texts):

| Metric | Value |
|---|---|
| Per-message ROC AUC (dirA, obs=base) | **0.845** (Hanley-McNeil 95% CI 0.764–0.925) |
| Windowed AUC, k=3 / k=5 / k=10 | **0.982 / 0.996 / 1.000** |
| Paired win-rate (real outranks its paired AI reply) | **44/47 = 93.6%** |
| Direction winner | **dirA: observer=base, performer=adapted** (0.845 vs dirB 0.677) |
| Threshold @ ~5% FPR | score < **0.9643** → flag AI (TPR 31.9%) |
| Threshold @ ~10% FPR | score < **1.0613** → flag AI (TPR 53.2%) |

AI text scores **lower** — same sign as the paper. Real Seth: mean 1.348 (sd 0.268);
AI: mean 1.028 (sd 0.177).

Interpretation: at message length (median 6–9 tokens) the generator's output is still
statistically separable from real Seth — the 53.2% Gemini-judge fool rate does NOT mean
the output beats statistical detection. Over a 5–10 message window the separation is
essentially total. This is now the cheapest fully-local adversarial signal we have.

## Method

Binoculars score for text `x` (reply tokens only, conditioned on the incoming message
through the chat template):

```
logPPL_obs(x)   = -mean_i log P_obs(x_i | ctx, x_<i)
XEnt(perf→obs)  = -mean_i Σ_v P_perf(v | ctx, x_<i) · log P_obs(v | ctx, x_<i)
score           = logPPL_obs / XEnt(perf→obs)
```

- **Model pair**: base = `mlx-community/gemma-4-31b-it-8bit`; adapted = base +
  `seth-lora-v5-8bit-20260718-105251` (the production adapter). Both direction
  assignments were computed on the same run; **dirA (observer=base,
  performer=adapted) wins decisively** and is the shipped default.
- **Conditioning**: unlike the paper (raw documents), each text is scored as the
  assistant turn given the incoming message via the chat template. This matches how
  the AI side was generated and how the LoRA was trained; raw 6-word perplexity
  carries far less signal (plain `logppl_base` AUC: 0.648).
- **Compute**: direct mlx_lm scoring, one 31B model resident at a time (two-pass with
  an on-disk float16 log-softmax cache), never touching the live :8741 server. Full
  94-text calibration ≈ 12 min wall on the Studio, sequential single-text forwards.

Why dirA wins: the AI corpus was generated *by* the adapted model. With the performer
≈ the actual generator, the cross-entropy denominator explains away the generator's
own predictability and the ratio isolates "machine-likeness given this generator."
dirB collapses toward a generic perplexity test (0.677), and simple per-token
Δlog-likelihood (adapted−base) only reaches 0.661.

## Score variants measured

| Score | Per-msg AUC | Windowed k=5 | Notes |
|---|---|---|---|
| **dirA binoculars (obs=base)** | **0.845** | **0.996** | shipped default |
| dirB binoculars (obs=adapted) | 0.677 | 0.690 | rejected direction |
| Δlog-lik per token (adapted−base) | 0.661 | — | simplest contrast, not enough |
| logPPL base only | 0.648 | 0.755 | generic detector baseline |
| logPPL adapted only | 0.632 | 0.662 | |

All variants score AI lower than real. Windowed scores pool token-level
numerator/denominator sums over k same-class messages (equivalent to concatenation),
400 sampled windows per class; windows overlap, so windowed AUCs are separability
estimates rather than independent-sample statistics — but k=10 separated in all 400
draws.

## Threshold validation on independent real messages

Thresholds above are calibrated on only 47 real texts, so the real-side false-positive
rate was re-measured on 100 held-out real Seth messages sampled from
`data/imessage/ground_truth.jsonl` (seed 42, overlap with the 47 trials excluded):

| Check | Calibration (n=47 real) | Holdout (n=100 real) |
|---|---|---|
| dirA mean / median / sd | 1.348 / 1.307 / 0.268 | 1.344 / 1.319 / 0.277 |
| FPR at threshold 0.9643 | 4.3% (by construction) | **5.0%** |
| FPR at threshold 1.0613 | 8.5% (by construction) | **10.0%** |

The real-side score distribution is stable across corpora and the calibrated
thresholds hold their FPR exactly on unseen real messages — the thresholds are safe
to reuse until the adapter changes.

## Data

- `data/eval_blinded_ab.json` — 47 trials from the 07-24 nightly (incoming +
  real_seth + ai_response), the paired calibration corpus. No
  `eval-blinded-ab-*.json` archives exist in `~/.human/logs/eval-archive/`, so 94
  texts is the full paired corpus today.
- `data/imessage/ground_truth.jsonl` — 644 real pairs; 100-message holdout for FPR
  validation (real side only).

## Caveats

1. **The detector is coupled to the adapter version.** dirA's power comes from the
   performer being the production generator. Retraining/promoting a new adapter
   shifts both distributions — thresholds must be recalibrated per adapter (cheap:
   one 94-text run), and the score is *not* a general AI detector for other models'
   text.
2. **Single-night corpus.** All 47 AI generations are from one nightly (07-24) under
   one gate configuration. AUC should be re-estimated as more nightly corpora
   accumulate before trusting the exact TPR numbers.
3. **Per-message thresholds are conservative at 5% FPR** (TPR 32%). The windowed
   score is where the discriminator is strong; per-message flags should be treated
   as noisy evidence, windows of ≥5 as near-certain.
4. **Judge-vs-detector gap is expected.** The Gemini judge measures "sounds human to
   a reader"; Binoculars measures "statistically typical of the generator." A reply
   can fool a reader and still sit squarely in the generator's typical set. The two
   metrics are complementary, not redundant.

## Wiring proposal (AUC > 0.65 → proposed; nothing touches promotion gates)

Per `feature-gate-requires-measurement`, the human rating tier remains the only
promotion keystone. Two measurement-side wirings:

1. **Nightly metric alongside the fool rate.** After `eval_blinded_ab.py` writes its
   trials JSON, run `binoculars_score.py --pairs <that file>` and record per-message
   AUC + windowed k=5 AUC + mean dirA scores per class next to `fool_rate` in the
   nightly registry. Cost ≈ 12 min GPU *after* the nightly finishes (stay out of the
   3am window; run it as the last nightly step, or on the 07:30 deploy-check slot).
   Trend to watch: windowed AUC drifting *down* toward 0.5 = genuine statistical
   humanness progress, invisible to the judge today.
2. **DPO rejected-sample miner.** Generations scoring below the 5%-FPR threshold
   (confidently-AI, score < 0.9643 at current calibration) are exactly the "most
   machine-typical" outputs. Pair them as `rejected` against the trial's `real_seth`
   as `chosen`, source-tagged `binoculars_miner`, into the existing dpo_pairs
   pipeline. Ships OFF; first run in shadow (log candidate pairs, write nothing)
   per the gate contract.

Not proposed: any per-message runtime gate on the send path (too noisy at 32% TPR,
and it would double per-turn GPU cost).

## Reproduce

```bash
~/.human/venv/bin/python scripts/blind_ab/binoculars_score.py --selftest
~/.human/venv/bin/python scripts/blind_ab/binoculars_score.py \
  --pairs data/eval_blinded_ab.json --windows 3,5,10 \
  --cache-dir /tmp/binoc_cache --out /tmp/binoculars_results.json
```

`scripts/blind_ab/binoculars_score.py` also takes `--texts file.json` (list of
`{text, context?, label?}`), `--text "..."` for one-offs, `--no-context` for
paper-style raw scoring, and `--stage base|adapted|combine` for resumable runs.
