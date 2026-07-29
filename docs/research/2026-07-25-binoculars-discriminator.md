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

## Corpus finding: real Seth ground truth carried a stray leading byte

> **Rate correction (2026-07-26).** The "20%" below is the **conservative
> guard's** count, which deliberately over-flags: it also catches legitimate
> openers like `'“Professional misconduct”'` and `'#7225'`. The *true* corruption
> rate, measured by the exact signature — a printable leading byte whose ordinal
> equals the message's own utf-8 byte length — is **7.5%** of assistant targets
> in the affected training set (202/2676), against a **0.3–0.4% baseline
> coincidence** floor. Real and material, but smaller than first reported. Prefer
> the signature test over the guard regex when quantifying; the guard exists to
> refuse pairs cheaply, not to measure.

Surfaced by the miner's first shadow run (exactly what shadow mode is for). **131
of 644** (20.3% by the conservative guard) `seth_reply` values in
`data/imessage/ground_truth.jsonl` — and 9 of the 47 `real_seth` values in the
07-24 trials — begin with a stray character before the real text:

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
2. ~~**The fool rate is probably overstated.**~~ **DISPROVEN, measured
   2026-07-27.** The guess was that a corrupted `real_seth` reads as broken and
   pushes the judge toward the AI option, inflating the fool rate. Measured
   within the 07-24 run, the effect runs the **other way**:

   | 07-24 trials | fool rate |
   |---|---|
   | `real_seth` corrupted (n=8) | **25.0%** |
   | `real_seth` clean (n=39) | **59.0%** |

   Corruption made the human option *easier* to identify, not harder — mangled
   text reads as weird, and weird reads as human, whereas AI text is polished.
   So the corpus fix should if anything have *raised* the fool rate. (n=8 is
   small; the direction is clear, the magnitude is not.)
3. **AUC is NOT affected (checked).** Recomputing on the 38 clean pairs only:
   **0.832** vs 0.845 published — well inside the CI, and corrupted rows scored
   slightly *lower* (mean 1.309 vs 1.357), i.e. marginally harder to detect. The
   corruption was working mildly against the detector, not inflating it.

**RESOLVED upstream, 2026-07-25** (`4459b246a`, branch `claude/modest-agnesi-369cac`).
Two bugs in one decoder, both from a duplicated copy of
`scripts/blind_ab/imessage_text.py` that never received the `e80af898` fix:

1. Text was read starting **at the length byte**, which leaked as a leading
   character whenever it was printable (0x20–0x7E, i.e. byte lengths 32–126).
   `",I don’t know…"` — `0x2c` = 44 = that message's own byte length. Shorter
   messages had control-character lengths that a downstream `re.sub` stripped,
   so they decoded correctly *by accident*.
2. `\x86` was used as an end marker, but 0x86 is a legal UTF-8 **continuation**
   byte — so any message containing 😆 (`f0 9f 98 86`) or the `↩` that opens a
   reply-quote (`e2 86 a9`) was truncated mid-character and usually dropped.

Result: **131/644 (20.3%) → 4/690 (0.6%)**, and the corpus *grew by 46 rows* as
bug 2's dropped messages came back (27 now contain emoji). Of the 4 remaining
matches, 2 are **false positives of the conservative guard**, not corruption —
`'#7225'` and `'“Professional misconduct”'` are legitimate messages that merely
open with punctuation-then-word. Expect a **`corrupt_chosen` floor of ~0–2 per
run**; that is the guard being deliberately conservative, not corruption
returning. The guard stays regardless: it costs ~0.6% of pairs and is cheap
insurance, since `scripts/extract_imessage_pairs.py` **regenerates
`ground_truth.jsonl` on any invocation, including `--help`** — and the file is
untracked, so running the *unfixed* extractor from main re-corrupts it.

## Behavioral confirmation: the decoder bug reached the model (2026-07-26)

The corpus fix was verified at the *data* layer above. This is the confirmation it
changed **model behavior** — the part that actually matters.

`seth-lora-v6-8bit-20260725-114316` is the v5 recipe with **only the data
changed** (same base, iters, LR, layers, rank; `scale: 2.0` verified in
`adapter_config.json` both mid-run and at completion). Head-to-head via
`scripts/blind_ab/adapter_smoke_test.py`:

| category | n | v5 | v6 |
|---|---|---|---|
| persona | 6 | 4 | 5 |
| instruction | 6 | **1** | **4** |
| reasoning | 4 | 0 | 1 |
| **leading-byte artifact** | — | **5** | **0** |

The artifact count is the headline: v5 emitted the stray prefix in 5 of 16 replies
(`'.I’m good. Just getting through a mountain of emails'`), v6 in **none**. v5 also
leaked raw template tokens (`"<|turn>user\nI'm doing great! I'm doing amazing!…"`),
which is what a corrupted-target adapter degrading its base looks like. No
base-capability regressions; 4 checks moved v5-fail → v6-pass.

**v6 is still NOT promotable on voice.** It emits degenerate loops and a truncated
fragment (`'ing'`), which the corruption hypothesis does not explain — that points
elsewhere, plausibly sampling parameters. v5 remains the served adapter.

### Scope: this bug did NOT affect every corpus
A GLM-shaped adapter trained the same week was initially assumed to share the
confound. Measured directly, its training data sits at **0.3%** — the baseline
coincidence floor, i.e. clean. The discriminator was never *when* a dataset was
built but **which extractor built it**: only `extract_imessage_pairs.py` and
`persona_style_card.py` carried the buggy decoder copies, while
`export_seth_triples.py`, `m3_extract_corpus.py`, `harvest_imessage_voice.py`, and
`classify_contact_formality.py` were already on the shared fixed decoder from
`e80af898`. Before attributing any downstream result to this bug, **measure that
dataset's signature rate** rather than reasoning from build timestamps.

## The fool-rate drop was a harness artifact, not a regression (2026-07-27)

The Gemini-judge fool rate fell 53.2% (07-24) → 28.6% (07-25) → 16.3% and 20.0%
(two 07-27 runs) while the Binoculars AUC barely moved (0.845 → 0.801). **That
divergence is the finding**, and it is what a second independent signal is for.

Cause: `10faedaee` (07-26 11:00) removed *"Abbreviate (gonna, tbh, idk, hru)"*
from the harness's own `SETH_SYSTEM_PROMPT`. Measured across the trials:

| | 07-24 (pre-fix) | 07-27 (post-fix) |
|---|---|---|
| AI replies containing "tbh" | **59.6%** | **0.0%** |
| real Seth containing "tbh" | **0.0%** | 0.0% |

The harness was coaching the model into a caricature of a texter, and the LLM
judge reads stereotypical texting markers as authentically human — so the model
"fooled" the judge by being *less* like Seth, who never used the word in this
corpus. **The 53.2% was substantially manufactured by the eval's own prompt.**

The same commit pushed the other way by giving the model the conversation thread
(it had been comparing a context-free AI reply against a context-rich human one;
the judge explicitly cited *"a LACK OF CONVERSATIONAL MEMORY"*). Its author
measured 0% → 16.7% from that half alone. Net of both, ~16–20% is the honest
number and 53.2% never was.

**Implication for this detector.** Binoculars stayed flat across a 33-point swing
in the judge metric because the model's *statistical typicality* genuinely did not
change — only surface lexical markers did. That is the intended behaviour, and it
is the clearest evidence so far that the two metrics measure different things:
the judge is sensitive to surface style (and therefore to prompt wording), while
Binoculars tracks distributional fit. Do not treat a fool-rate move as an
adapter-quality signal without checking whether the harness prompt changed.

## Caveats

1. **The detector is coupled to the SERVING BASE, not just the adapter version.**
   dirA's power comes from the performer *being* the production generator.
   Recalibrate when **either** half changes:

   | change | effect |
   |---|---|
   | new adapter, same base | distributions shift; recalibrate thresholds (one 94-text run) |
   | **different serving base** | the pair no longer straddles the generator — scores are **meaningless**, not merely shifted |

   This doc originally said "per adapter", and that was wrong in a way that
   produced a believable number. On **2026-07-28** `:8741` had flipped to
   GLM-4.5-Air-4bit while the detector pair was still gemma. The run emitted
   per-message AUC **0.5998** (calibrated 0.845), `mean_ai` drifting 1.028 →
   **1.245** toward `mean_real`, and `ai_frac_below_fpr5_thr` collapsing 0.37 →
   **0.07**. Read naively: "output is now nearly indistinguishable." Actually:
   "this detector cannot characterise this generator." A base flip does not
   degrade the score gracefully — it silently converts it into noise centred
   near 0.5, which is exactly the value that reads as success.

   **Guarded since `d93bfeadb`.** `eval_blinded_ab.py` compares the model id
   reported in the *generation responses* against `HU_BINOCULARS_BASE` (default
   gemma) and writes `{"skipped": reason}` instead of a score on mismatch. It
   fails closed on an unverifiable precondition and on a mid-run serving flip.
   `HU_BINOCULARS_ALLOW_BASE_MISMATCH=1` overrides, loudly.

   Note the guard reads the generation response, *not* `GET /v1/models`: the MLX
   server is single-threaded, so a metadata probe queues behind in-flight
   generation and times out on a busy box — a probe-based guard would fail
   spuriously and silently disable the metric.

   The score is also *not* a general AI detector for other models' text.
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
   adapter promotion **or serving-base change** + recalibration (see Caveat 1).
   Trend to watch: windowed AUC drifting *down* toward 0.5 = genuine statistical
   humanness progress, invisible to the judge today.

   **Read a falling AUC together with the guard.** Since `d93bfeadb` a base
   mismatch produces `{"skipped": …}` rather than a low score, so a *number*
   near 0.5 now means what it should. Before that commit it could equally mean
   the detector had been decoupled from the generator — the 2026-07-28 run is
   the worked example.
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
