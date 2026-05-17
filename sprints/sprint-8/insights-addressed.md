# Sprint 8 Smoke #3 — Addressing the 4 insights as action items

The four findings from Smoke Run #3 are interrelated. This doc converts each
from observation to **a code change, a metric, or a calibrated threshold** —
otherwise they're just clever observations that don't ship.

---

## Insight 1 — "Iter 60 wins on BOTH dimensions"

> Iter 60 has highest fidelity Δ (+0.019) AND best coherence (40% pad vs 70-80% elsewhere). The other checkpoints score lower fidelity AND are more broken.

### What this means
- The "best adapter" decision must be multi-dimensional, not single-metric.
- A scalar gate (delta > 0.05) is the wrong shape for the actual quality surface.
- The Pareto frontier is the right abstraction.

### Action taken — `scripts/pareto_picker.py`

Implements multiplicative scoring:
```
pareto_score = fidelity_delta * (1 - pad_failure_rate)
```

With three-tier classification:
| Verdict | Rule |
|---|---|
| **PROMOTE** | Δ ≥ +0.03 AND pad_rate ≤ 10% |
| **DEFER** | Δ ≥ +0.01 AND pad_rate ≤ 50% |
| **REJECT** | otherwise |

### Verified against real data

```
Checkpoint        Δ        pad rate   Pareto score   Verdict
iter60_DPO       +0.019    40%        +0.0114        DEFER  ← best
iter200_DPO      +0.046    80%        +0.0092        REJECT  ← would be picked by scalar gate, wrong!
iter40_DPO       +0.003    77%        +0.0007        REJECT
iter20_DPO       -0.008    73%        -0.0080        REJECT
iter80_DPO       -0.001    70%        -0.0010        REJECT
```

The picker **correctly inverts** the original ranking — iter 200 looks better
on raw delta but loses on Pareto because 80% of its outputs are broken.

### Where this lands in Sprint 8
- **US-8.12 (new from Smoke #3):** adapter promotion gate must require both delta
  AND coherence. `scripts/pareto_picker.py` is the implementation. Wire it into
  `check-lora-ab.sh` so the CI gate becomes coherence-aware.
- The W14 cron (US-7.5) must promote based on this rule, not on raw delta.

---

## Insight 2 — "Training metrics LIED about the sweet spot"

> Val accuracy peaked at iter 20/40 (89.8%) and dropped at iter 60 (78.1%) — yet iter 60 produces the BEST generations. Classification val accuracy is NOT predictive of generation quality.

### What this means
The DPO classification task ("which of these two is preferred?") has a
different shape than the generation task ("write a coherent response in
Seth's voice"). High classification accuracy can correlate with adapter
weights drifting far from base — which is good for classification but
bad for generation coherence.

### The signal that actually works — mined from `/tmp/dpo80.log`

Train `chosen_r` (the reward signal on the chosen completion at training time):

| Iter | train chosen_r | val_acc | val_loss | gen pad rate |
|------|---|---|---|---|
| 5  | +17.6 | -    | -     | -      |
| 10 | +15.9 | -    | -     | -      |
| 20 | +20.4 | 0.898 | 0.519 | 22/30 (bad) |
| 25 | **+4.3** ← big drop | - | - | - |
| 35 | +9.2  | -    | -     | -      |
| 40 | +8.9  | 0.898 | 0.418 | 23/30 (bad) |
| 45 | +8.7  | -    | -     | -      |
| 50 | +9.9  | -    | -     | -      |
| 55 | +7.5  | -    | -     | -      |
| **60** | **+7.3** ← stable plateau | 0.781 | 0.784 | **12/30 (best)** |
| 65 | **+3.2** ← CLIFF | - | - | - |
| 70 | +2.1  | -    | -     | -      |
| 75 | **-4.5** ← SIGN FLIP | - | - | - |
| 80 | -8.9  | 0.781 | 0.740 | 21/30 (bad) |

**The plateau iter 35-60** (`chosen_r` ~+7 to +9) corresponds to the generation
quality sweet spot. The **cliff at iter 65-80** corresponds to the collapse.

### Stopping rule that would have worked

Three candidate rules, all checked against this run's data:

```
RULE A:  Stop when train_chosen_r drops below 50% of its plateau-window mean.
         Window = last 5 reports.
         Mean(iter 35-55) ≈ +8.8.  50% = +4.4.
         iter 65: chosen_r = +3.2 → STOP HERE (1 iter past iter 60 sweet spot)

RULE B:  Stop when train_chosen_r decreases >50% over 10 iters.
         iter 60 → iter 65: 7.3 → 3.2  = -56% decrease → STOP at iter 65

RULE C:  Stop when val_chosen_reward goes negative.
         iter 80: -0.037 → STOP — but this is 20 iters LATE (past the cliff)
```

**Rule A or B both catch iter 65 — adapter from iter 60 (one window before)
would be the promoted artifact.** Rule C is too lagging.

### Where this lands in Sprint 8
- **US-8.10 (redefined):** early-stopping signal is `train_chosen_r` plateau-break
  detection (Rule A or B), NOT val accuracy / val loss.
- Implementation: post-step callback in the DPO training loop that computes
  the trailing-5 mean of `chosen_r`. If current value < 0.5 × plateau mean,
  set `should_stop = True`. Save the adapter from the prior window as the
  final artifact.
- This is **MUCH cheaper than the proposed US-8.11** (re-sampling 10 generations
  every 20 iters). The signal is already in the training output — we just
  haven't been reading it.

---

## Insight 3 — "There's a U-shape: bad early, good in middle, bad late"

> The 200-iter run's <pad> failure rate (80%) confirms iter 60 is well past the chosen-reward inflection point but before total decoder collapse.

### What this means
Reward hacking in DPO has a characteristic two-phase failure:
1. **Phase 1 (iter 0-30):** model swings wildly trying to satisfy the
   preference signal. `chosen_r` oscillates, output is unstable.
2. **Phase 2 (iter 30-60):** model converges on a stable solution that
   matches preferences without diverging too far from reference. `chosen_r`
   plateaus. Output is best.
3. **Phase 3 (iter 60+):** model starts pushing `rejected_r` arbitrarily low
   to maximize margin, dragging `chosen_r` with it. Output collapses.

The middle phase is the only one worth shipping. The early phase looks
correct on val accuracy (89.8% at iter 20) but is unstable. The late phase
looks correct on margin metric (margin > 20 at iter 80) but is broken.

### What we learned about WHY this happens

The DPO objective is:
```
L = -log σ(β * [ (logπ_w/|y_w| - logπ_ref(y_w)/|y_w|) -
                 (logπ_l/|y_l| - logπ_ref(y_l)/|y_l|) ])
```

The gradient prefers ANY direction that increases the LOG-RATIO between
chosen and rejected. With small data (331 pairs), there's nothing in the
loss to penalize "pushing both completions away from reference" — only the
DIFFERENCE matters. So the optimizer learns the cheapest path: drive both
toward -infinity, just rejected faster.

This is the **reward-hacking failure mode** documented in DPO follow-up
papers (e.g., R-DPO, IPO). The fix in those papers is either:
- KL-regularize against the reference more aggressively
- Use IPO loss instead of sigmoid loss (replaces log-ratio with squared
  difference, has no extrapolation singularity)
- Switch to ORPO (single-stage, no reference model, no extrapolation
  failure mode)

### Where this lands in Sprint 8
- **US-8.10 enhancement:** the stopping rule we derived in Insight 2 is the
  cheap fix. Catches the cliff at iter 65.
- **US-8.13 (NEW):** try `--dpo-cpo-loss-type ipo` instead of `sigmoid` —
  the mlx_lm_lora CLI accepts this. IPO has a quadratic penalty that bounds
  the log-ratio, eliminating the reward-hacking failure mode in principle.
- **US-7.10 SimPO is also relevant here** — SimPO has no reference model
  so no "pushing away from reference" failure mode. But US-7.10's
  `train_step` is still stubbed out per the Sprint 7 audit (FU-7.10.a).

---

## Insight 4 — "+0.019 is below the +0.05 production gate. Lift is small and failure rate unacceptable for production."

> Sprint 8's US-8.2 (real perplexity NLL backend) is beyond critical — without coherence-aware scoring, the gate cannot distinguish "small lift on clean output" from "small lift on garbage."

### What this means
Three options to handle the gap between current results and production:
1. **Lower the gate** — accept smaller deltas as the new normal
2. **Wire better metrics** — make the gate coherence-aware so deltas are honest
3. **Train better** — get the delta higher AND the failure rate lower

These aren't mutually exclusive — Sprint 8 should do (2) and start (3).
Option (1) alone is dishonest (Sprint 7 auditor would catch it).

### Calibration: what should the gate actually be?

Based on Smoke #3 evidence (the first real data we have):

```
                 Current  Realistic-now  Production-aim
fidelity_delta   ≥ +0.05  ≥ +0.01        ≥ +0.03
pad_failure      (none)   ≤ 50%          ≤ 10%
gate type        scalar   Pareto         Pareto

What passes:     nothing  iter 60 only   nothing yet
```

The **Realistic-now** gate is what `pareto_picker.py` implements (DEFER
threshold). It's calibrated to admit iter 60 — the best-known empirical
checkpoint. This lets us track progress without lying about production
readiness.

The **Production-aim** gate is the bar we need to hit before claiming
"digital twin shippable to a real user". Sprint 8 + Sprint 9 work should
chip away at it.

### Concrete production gate (replaces Sprint 7's +0.05 single-metric gate)

Wire `pareto_picker.py` into `check-lora-ab.sh` so the gate output becomes:

```bash
scripts/check-lora-ab.sh \
  --base ~/.human/training-data/adapters/seth-sft \
  --adapter ~/.human/training-data/adapters/seth-dpo \
  --pareto

# Emits JSON:
# {"delta": 0.019, "pad_rate": 0.40, "pareto_score": 0.0114,
#  "verdict": "DEFER", "promotion_gate": "Δ≥0.03 AND pad≤10%"}
# Exit codes: 0=PROMOTE, 1=DEFER, 2=REJECT
```

US-7.5 (W14 cron) consumes the exit code: promote only on 0, log on 1,
discard candidate on 2.

### Where this lands in Sprint 8
- **US-8.1 (real DPO vs SFT gate) — UPDATED scope:** the gate now emits
  Pareto verdict, not raw delta. The wiring already exists; the metric
  must change.
- **US-8.2 (NLL backend) — still CRITICAL P0:** real perplexity adds a THIRD
  dimension to the Pareto frontier (delta × coherence × perplexity-vs-base).
  Without it, "coherence" is just "no pad tokens" — too coarse.
- **US-8.14 (NEW):** explicit gate-calibration story. Run the eval against
  a known-bad adapter (e.g. a 1000-iter overfit DPO) and a known-good
  adapter (when one exists). Verify the verdict matches operator intent.

---

## Consolidated Sprint 8 backlog after Smoke #3

Original 6 stories (US-8.1 through US-8.6) plus these new ones:

| Story | Source | Status |
|---|---|---|
| US-8.1 | Original | **UPDATED**: gate emits Pareto verdict, not scalar delta |
| US-8.2 | Original | **MEGA-CRITICAL P0**: real NLL backend |
| US-8.3 | Original | P0 (simpo hide-or-wire) |
| US-8.4 | Original | P1 (step discriminator) |
| US-8.5 | Original | P1 (UTF-8 redactor) |
| US-8.6 | Original | **DONE** (490b6359) |
| US-8.7 | Smoke #1 | **DEMOTED**: 200 iters too many, not too few |
| US-8.8 | Smoke #1 | P1 (larger eval corpus, 100+ prompts) |
| US-8.9 | Smoke #1 | P1 (bootstrap personal_model.bin) |
| US-8.10 | Smoke #3 | **REDEFINED**: `train_chosen_r` plateau-break, not val acc |
| US-8.11 | Smoke #3 | **DEPRECATED** — Insight 2 found a cheaper signal (US-8.10 supersedes) |
| US-8.12 | Smoke #3 | NEW: promotion gate is Pareto (`pareto_picker.py` shipped) |
| US-8.13 | Smoke #3 | NEW: try IPO loss instead of sigmoid DPO to eliminate reward hacking |
| US-8.14 | Smoke #3 | NEW: gate calibration test with known-bad + known-good adapters |

The backlog shrunk by one story (US-8.11 → deprecated) and gained 3 better-grounded ones.

---

## Files added this iteration

- `scripts/pareto_picker.py` — coherence-aware promotion verdict (PROMOTE/DEFER/REJECT)
- `sprints/sprint-8/smoke-run-3.md` — full Smoke #3 narrative
- `sprints/sprint-8/insights-addressed.md` (this file) — actions per insight

## Honest one-line verdict

**Four insights, four action items: a Pareto picker, an early-stopping signal mined from training logs, a U-shape mechanism understood, and a calibrated gate proposal. Sprint 8 backlog now reflects reality, not aspiration.**
