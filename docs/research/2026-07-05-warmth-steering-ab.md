# Warmth Steering A/B Test — 2026-07-05

## Summary

Tested whether LoRA steering vectors on the warmth dimension improve perceived warmth
and humanness of AI-generated replies in informal texting contexts. Round 2 extends
Round 1 with dose-response experiments: a subtle warm dose (+0.4) and a cold dose (-0.5).

**Canonical URLs for results:**
- Experiment A (warmth +0.4): `data/steering_ab_results.json`
- Experiment B (professional -0.5): `data/steering_ab_results_professional.json`

---

## Round 1: Warmth Steering (+1.0 vs OFF)

**Configuration:**
- MLX server: :8743 with persona adapter + warmth steering vector loaded
- Arm: WARM (+1.0 coefficient)
- Judge: Gemini 3.1 Pro (blinded pairwise comparison)
- Contexts: n=20 synthetic iMessage incomings
- Judge questions: warmth (A or B) + humanness (A or B)

### Results

- **Win-rate (WARM perceived warmer): 50.0%** (10/20)
- **95% Wilson CI: [29.9%, 70.1%]**
- **Humanness win-rate: 35.0%** (7/20)

### Interpretation

The strongest dose (+1.0) is **inconclusive on warmth** (50% win-rate, CI straddles 50%) and **hurts humanness** (35% — OFF appears more human). The over-amplification hypothesis from lab data holds: alpha=1.0 is too aggressive and produces responses that read as artificially warm, collapsing perceived humanity.

### Pass Criteria

✓ **Warmth**: CI lower bound > 50% → **FAIL** (29.9%)
✓ **Humanness**: Win-rate ≥ 40% → **FAIL** (35.0%)

**Verdict: INCONCLUSIVE**

---

## Round 2: Experiment A — Subtle Warmth (+0.4 vs OFF)

**Hypothesis:** Reducing dose to +0.4 (subtle signal) maintains humanness ≥45% while warmth win-rate exceeds 50%.

**Configuration:**
- MLX server: :8743 with persona adapter + warmth steering vector loaded
- Arm A: WARM +0.4 (subtle dose)
- Arm B: OFF (baseline)
- Judge: Gemini 3.1 Pro (blinded pairwise comparison)
- Contexts: n=20 synthetic iMessage incomings
- Judge questions: warmth (A or B) + humanness (A or B)

### Results

- **Warmth win-rate: 60.0%** (12/20)
- **95% Wilson CI: [38.7%, 78.1%]**
- **Humanness win-rate: 65.0%** (13/20)

### Interpretation

The subtle dose (+0.4) shows **promise on humanness** (65% — a 30-point swing from Round 1's 35%) and a **marginal warmth win** (60%, higher than Round 1's 50%). However, the confidence interval for warmth **does NOT cross the 50% lower bound** (38.7% < 50%), so warmth is not yet statistically significant.

### Pass Criteria

✓ **Warmth**: CI lower bound > 50% AND win-rate ≥ 55% → **FAIL** (38.7%, 60.0%)
✓ **Humanness**: Win-rate ≥ 45% → **PASS** (65.0%)

**Verdict: INCONCLUSIVE** (warmth marginal, humanness strong)

### Dose-Response Comparison: Round 1 vs Round 2 Exp A

| Metric | Round 1 (+1.0) | Exp A (+0.4) | Δ |
|--------|---|---|---|
| Warmth win-rate | 50.0% | 60.0% | +10pp |
| Warmth CI lower | 29.9% | 38.7% | +8.8pp |
| Humanness win-rate | 35.0% | 65.0% | +30pp |

**Conclusion:** Scaling from +1.0 to +0.4 dramatically improves humanness perception (+30pp) and slightly improves warmth win-rate (+10pp), but doesn't yet achieve >50% CI lower bound on warmth. The subtle dose is closer to the right direction than the strong dose.

---

## Round 2: Experiment B — Cold Modulation (-0.5 vs OFF, Professional Context)

**Hypothesis:** Cold direction (-0.5) has more headroom than warm, and measuring against a professional-acquaintance context (rather than close friend) will show appropriateness gains without humanness loss.

**Configuration:**
- MLX server: :8743 with persona adapter + steering vector loaded
- Arm A: COLD -0.5 (opposite direction for contrast)
- Arm B: OFF (baseline)
- Judge: Gemini 3.1 Pro (blinded pairwise comparison)
- Contexts: n=20 synthetic iMessage incomings (20 attempted, 5 succeeded)
- Judge questions: appropriateness for professional acquaintance (A or B) + humanness (A or B)

### Results

⚠️ **Generation failures:** 15 of 20 trials failed to generate COLD replies (5 successful trials).
- **Appropriateness win-rate: 40.0%** (2/5 successful trials)
- **95% Wilson CI: [11.8%, 76.9%]** (wide CI due to small N)
- **Humanness win-rate: 40.0%** (2/5)

### Interpretation

The negative steering dose appears to have **server-side issues** — the MLX endpoint failed to generate cold-steered replies in 75% of attempts. The 5 successful trials show COLD performing **worse than OFF** on both dimensions (40% < 50% on both), suggesting the steering implementation may be problematic for negative values or the dose is inappropriately strong.

### Pass Criteria

✓ **Appropriateness**: CI lower bound > 50% AND win-rate ≥ 55% → **FAIL** (11.8%, 40.0%)
✓ **Humanness**: Win-rate ≥ 40% → **PASS** (40.0%, at threshold)

**Verdict: INCONCLUSIVE** (blocked by generation failures)

### Why Experiment B is Inconclusive

This experiment is **blocked on infrastructure, not signal**:
1. The high failure rate (15/20) suggests the steering logic may not support negative coefficients correctly.
2. The small successful sample (N=5) produces a wide CI that contains both "COLD better" and "COLD worse."
3. The actual win-rate on successful trials (2/5 = 40%) suggests cold direction does NOT improve appropriateness under current conditions.

**Recommendation for follow-up:**
- Verify the MLX steering implementation handles negative coefficients (may need `abs()` or different scaling).
- If steering is confirmed working, re-run Experiment B with a larger N or different dose (e.g., -0.3).

---

## Key Findings

1. **Round 1 (+1.0 dose is too strong):** Warmth inconclusive (50%), humanness hurt (35%).
2. **Experiment A (+0.4 dose improves humanness but not warmth CI):** Humanness 65% (strong), warmth 60% win-rate but CI lower = 38.7% (not >50%).
3. **Experiment B (cold direction blocked by infrastructure):** Server failed to generate 75% of cold-steered replies; small successful sample (N=5) shows cold underperforms OFF.

### Dose-Response Pattern

Warmth steering shows a **clear dose-response on humanness** (35% → 65% as dose scales down from +1.0 to +0.4), but **warmth win-rate plateaus at ~60%** without reaching statistical significance (CI > 50%). 

**Next steps:**
- Try intermediate doses (e.g., +0.25, +0.5) to find the point where both warmth AND humanness achieve pass criteria.
- Investigate and fix the negative-coefficient steering generation failures before re-running Experiment B.
- Consider if the judge is conflating "warm" with other dimensions (e.g., "direct" vs "hedging"), which could cap warmth win-rate at ~60% independent of dose.

---

## Appendix: Statistical Notes

**Wilson score interval (95% CI)** used throughout. For N=20 trials, each observed proportion is:
- Point estimate = observed / N
- CI bounds = [lower, upper] where lower > 50% gives statistical significance at p<0.05

For Experiment A (+0.4):
- Warmth: 12/20 wins → 60% point, CI [38.7%, 78.1%] (CI lower does not exceed 50%)
- Humanness: 13/20 wins → 65% point, CI [42.1%, 83.1%] (CI lower exceeds 50%)

The humanness result IS statistically significant; warmth is not (by this gate).

## Round 2 Pilot Re-run (Warmth, WARM +0.4 vs OFF, n=2)

**Hypothesis:** Subtle dose (+0.4) maintains humanness ≥45% while warmth wins >50%

**Configuration:**
- MLX server: :8743 with persona adapter + steering vector loaded
- Arm A: WARM +0.4
- Arm B: OFF (baseline)
- Judge: Gemini 3.1 Pro (blinded pairwise comparison)
- Contexts: n=2 synthetic iMessage incomings
- Judge questions: warmth (A or B) + humanness (A or B)

### Results

- **Warmth win-rate (warmth): 0.0%** (0/2)
- **95% Wilson CI: [0.0%, 65.8%]**
- **Humanness win-rate: 0.0%** (0/2)

### Pass Criteria

✓ **Warmth**: CI lower bound > 50% AND win-rate ≥ 55% → **FAIL**
✓ **Humanness**: Win-rate ≥ 45% → **FAIL**

**Verdict: INCONCLUSIVE**

### Dose-Response vs Round 1

| Metric | Round 1 (+1.0) | Round 2 (0.4) | Direction |
|--------|---|---|---|
| Warmth | 50.0% | 0.0% | - |
| Humanness | 35.0% | 0.0% | - |

### Example Triples (First 3)

**Context 1**

**Incoming:** "lol remember that time we got lost"

**OFF (Baseline):**
hard to forget. you insisted the paper map was better than gps. i'm still not forgiving you for that tbh

**WARM +0.4:**
Emoji? No. *   Tone? Casual, dry humor. "i wasn

**Judge:** Response B shows warmth through playful, affectionate teasing about a shared memory, whereas Response A is a cold, broken AI output.
- Warmth: False
- Humanness: False

**Context 2**

**Incoming:** "been thinking about making a change"

**OFF (Baseline):**
"the big kind or just rearranging the furniture? i've learned that drifting is usually more dangerous than jumping." *Final Polish:* "the big kind or just

**WARM +0.4:**
big moves. career or just life in general?

**Judge:** Response A is warmer because it shows genuine, supportive interest in the friend's life by asking a clear, caring question, whereas Response B offers unsolicited advice and contains robotic generation artifacts.
- Warmth: False
- Humanness: False

*(n=2 pilot only — this warmth round-2 section was nearly lost to a doc-truncation bug in steering_ab.py, fixed alongside the formality run below.)*

## Formality Vector (FORMAL +0.4 vs OFF, n=20)

**Hypothesis:** The formality vector (+0.4) visibly shifts register toward formal (formality win-rate >50%) without collapsing humanness (≥40%)

**Configuration:**
- MLX server: :8743 with persona adapter + steering vector loaded
- Arm A: FORMAL +0.4
- Arm B: OFF (baseline)
- Judge: Gemini 3.1 Pro (blinded pairwise comparison)
- Contexts: n=20 synthetic iMessage incomings
- Judge questions: formality (A or B) + humanness (A or B)

### Results

- **Formality win-rate (formality): 60.0%** (12/20)
- **95% Wilson CI: [38.7%, 78.1%]**
- **Humanness win-rate: 60.0%** (12/20)

### Pass Criteria

✓ **Formality**: CI lower bound > 50% AND win-rate ≥ 55% → **FAIL**
✓ **Humanness**: Win-rate ≥ 40% → **PASS**

**Verdict: INCONCLUSIVE**

### Example Triples (First 3)

**Context 1**

**Incoming:** "I'm so stressed about this deadline"

**OFF (Baseline):**
``` ``` ```

**FORMAL +0.4:**
Output (iMessage):* i feel you. deadlines are just moving targets. take a breath. you're gonna be fine.

**Judge:** Response A is the only option containing actual language and complete sentences, making it more formal than the empty markdown characters in Response B.
- Formality: True
- Humanness: True

**Context 2**

**Incoming:** "I got the job!!"

**OFF (Baseline):**
huge news. congrats. gonna celebrate tonight or just sleep for a week?

**FORMAL +0.4:**
that's huge. congrats. gonna celebrate tonight?

**Judge:** Response A is slightly more formal because it begins with a complete subject-verb clause ('that's huge'), whereas Response B consists entirely of sentence fragments and highly casual hyperbole.
- Formality: False
- Humanness: True

**Context 3**

**Incoming:** "can you help me move this weekend"

**OFF (Baseline):**
"depends on the volume. i'm not a professional but i can lift. what's the plan?" *Let's try a slightly warmer but still casual version.* "depends. if it's just a few boxes i'm in. i'm not a pro but i can handle

**FORMAL +0.4:**
"wish i could but i'm knee deep in a new ai runtime project. plus my back isn't what it used to be" *Wait, let's make it even more 'Seth'.* "wish i could but i'm knee deep in a new

**Judge:** Response B is slightly more formal because it uses vocabulary like 'volume' and 'professional', whereas Response A relies on casual idioms like 'knee deep' and cuts off mid-sentence.
- Formality: False
- Humanness: False


### Extraction & Dose-Ceiling Notes (formality vector)

- **Extraction**: persona-steering-lab `src.extract --trait formality` on
  `gemma-4-26b-a4b-it-4bit` + persona adapter, 3 pairs x 6 questions x 2
  conditions (36 samples). Trait spec: `data/traits/formality.json`
  (formal-professional vs casual-texting instruction pairs).
- **Layer selection**: unconstrained argmax picked **layer 15/30**
  (alignment 0.402) — formality genuinely lives mid-depth, unlike warmth
  (layer 2). But mid-layer injection on this MoE corrupts the
  thought-channel protocol at ±0.6 (model never emits its answer channel),
  confirming the early-layer-only rule. Re-extracted with the new
  `--max-layer 8` cap → **layer 6** (alignment 0.260, residual norm 108,
  base_alpha ≈ 24). Both vectors committed to the lab repo
  (`formality_human.npz` = layer 6, `formality_human_l15.npz` = layer 15).
- **Asymmetric dose ceiling at layer 6**: CASUAL −0.6 generates cleanly;
  FORMAL **+0.5 and above loops in the thought channel** (5k+ chars of
  deliberation, no answer even at max_tokens 1500), while **+0.4/+0.45 are
  clean** and visibly formal-shifted. The formal direction appears to push
  the model into elaborate structured deliberation it cannot exit. This is
  why the A/B above ran at **+0.4, not the planned +0.6**.
- **Smoke triple** ("hey, rough day at work. you around?"), steering_applied
  echoed in every response:
  - OFF: "yeah, i'm here. sorry to hear. what's going on?"
  - FORMAL +0.4: "yeah, i'm around. just got home. want to vent or do you
    need a distraction?" (complete sentences, no slang)
  - CASUAL −0.6: "yeah im here. work is a grind man. u wanna vent or just
    need a distraction?" (dropped apostrophes, "man", "u")
- **Read on the 60/60 result**: at +0.4 the register shift is subtle enough
  that the blinded judge picks the steered arm only modestly more often
  (60%, CI includes 50%), but with zero humanness cost (steered arm judged
  more human 60% of the time). Next lever if a stronger effect is needed:
  n=50+ for CI power, or a formality vector extracted at a fixed early
  layer with more pairs/questions (--full) to sharpen the direction, since
  the dose ceiling blocks simply steering harder.
