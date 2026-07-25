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

The calibration corpus was the 47-trial 07-24 nightly run, which lived only as an
**uncommitted working-tree** `data/eval_blinded_ab.json`; every nightly overwrites
that path (it was replaced by a 28-trial run at 09:06 on 07-25, mid-session).
Copies are therefore archived outside the repo so these numbers stay reproducible:

| Archive (`~/.human/logs/eval-archive/`) | Contents |
|---|---|
| `binoculars-calibration-corpus-2026-07-24.json` | the 47 trials + merged `binoculars` block |
| `binoculars-scores-2026-07-24-calibration.json` | per-text scores for all 94 calibration texts |
| `binoculars-holdout-100-real-2026-07-25.json` | per-text scores for the 100-message real holdout |

All three deliberately avoid the `eval-<harness>-*.json` naming that
`nightly_eval.sh::archive_verdict` prunes to the newest 30. Re-run any figure in
this doc with `--pairs <the archived calibration file>`.
`data/imessage/ground_truth.jsonl` (644 real pairs) supplied the holdout, sampled
with seed 42 excluding the 47 trials.

## Corpus finding: 20% of real Seth ground truth carries a stray leading byte

Surfaced by the miner's first shadow run (exactly what shadow mode is for). **131
of 644** (20.3%) `seth_reply` values in `data/imessage/ground_truth.jsonl` — and
9 of the 47 `real_seth` values in the 07-24 trials — begin with a stray character
before the real text:

```
',I don’t know if I have there phone numbers'     '0Oh hey there! Jesus why don’t I…'
'%A ton, you know I got feelings for ya'          'HYeah works for me, I love a good bar…'
'.Edison and my brother and his family 8 people'  ';No he doesn’t want to come…'
```

The prefix varies (`. ; 9 J " - , 0 > % H ' ( =`), consistent with a length/type
byte leaking from the attributedBody decode in the extractor. Three consequences:

1. **Training (handled).** A corrupted `chosen` would teach the model to *emit*
   the artifact. `binoculars_to_dpo.py` refuses these pairs
   (`corrupt_chosen` counter); the guard is deliberately conservative since a
   false positive costs one skipped pair while a false negative poisons the corpus.
2. **The fool rate is probably overstated (unquantified).** The judge sees the
   corrupted string as the "human" option; a reply that opens with `%` reads as
   broken and pushes the judge toward the AI option. Some share of the 53.2%
   fool rate may be this artifact rather than genuine humanness. Not corrected
   here — it needs the extractor fix plus a re-judge.
3. **AUC is NOT affected (checked).** Recomputing on the 38 clean pairs only:
   **0.832** vs 0.845 published — well inside the CI, and corrupted rows scored
   slightly *lower* (mean 1.309 vs 1.357), i.e. marginally harder to detect. The
   corruption was working mildly against the detector, not inflating it.

The upstream fix belongs in the extractor, not here — note that
`scripts/extract_imessage_pairs.py` **regenerates `ground_truth.jsonl` on any
invocation, including `--help`**, so it must be handled deliberately.

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

1. **Nightly metric alongside the fool rate — WIRED (2026-07-25).**
   `eval_blinded_ab.py --binoculars` runs the scorer on the just-written trials
   JSON and merges a compact summary (`binoculars` key: per-message AUC, windowed
   k=5 AUC, class means, fraction of AI below the 5%-FPR threshold) into
   `data/eval_blinded_ab.json`. `nightly_eval.sh` passes the flag by default
   (`HU_NIGHTLY_BINOCULARS=0` to disable); it runs serially after the stage-3 gate
   measurement, skips invalid (<50%-judged) runs, is failure-isolated (`error` key,
   exit code unchanged), and never writes to `blind_ab_gate.json`. The 5%-FPR flag
   threshold is `HU_BINOCULARS_THR_FPR5` (default 0.9643) — re-set it after any
   adapter promotion + recalibration. Trend to watch: windowed AUC drifting *down*
   toward 0.5 = genuine statistical humanness progress, invisible to the judge
   today.
2. **DPO rejected-sample miner — WIRED OFF (2026-07-25).**
   `scripts/blind_ab/binoculars_to_dpo.py` pairs each below-threshold generation
   as `rejected` against the trial's `real_seth` as `chosen`, source-tagged
   `binoculars_miner`. `HU_NIGHTLY_BINOCULARS_DPO` selects 0=off (default),
   1=shadow (candidates appended to `~/.human/logs/eval-archive/binoc-dpo-candidates.jsonl`,
   no DB writes), 2=live. `--live` is opt-in even when invoked by hand.

   **Why the safety posture is strict here:** every consumer of `dpo_pairs`
   reads it *unfiltered* (`finetune-gemma.py:392`, `src/ml/dpo.c:598`), so a
   mined row enters the training corpus on the next run; and `src/ml/dpo.c:191`
   evicts FIFO by lowest id past `max_pairs`, so over-mining would delete older
   human-verified pairs. Hence `--max-insert` (default 25) and OFF-by-default.

   **Why it does not need judge_to_dpo's different-family gate:** that gate
   exists because a same-family judge's *verdict* becomes the training label.
   Here no verdict is ever a label — `chosen` is real Seth text from chat.db,
   and the Binoculars score only *selects* which rejected samples to train
   against. The genuine risk is different: the score is measured against the
   generator's current distribution, so iterating without recalibration could
   drift toward merely-atypical rather than Seth-like. Held in check by (a) the
   human-text anchor on `chosen`, (b) mandatory per-adapter recalibration,
   (c) the per-run cap.

   Shadow yield on the 07-24 corpus: 14 candidates from 47 trials → 9 after the
   corruption guard below, **3 of which the Gemini judge had been fooled by** —
   pairs no judge-driven miner can find.

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

The miner reads the per-trial scores merged by `--binoculars`, so it needs no
GPU pass of its own:

```bash
python3 scripts/blind_ab/binoculars_to_dpo.py --selftest
python3 scripts/blind_ab/binoculars_to_dpo.py --pairs data/eval_blinded_ab.json  # shadow
```
