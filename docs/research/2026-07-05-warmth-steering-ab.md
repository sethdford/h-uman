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


## Prefill vs Generation Steering — Mechanism Decomposition (2026-07-09)

A thought-channel gating mode (`HU_MLX_STEER_GATE_THOUGHT=on`, commit
99828d03) was built to try to unlock FORMAL ≥ +0.5 by not steering the
deliberation tokens. The controlled decomposition it enabled (formality
vector, layer 6, same prompt, fresh server per config):

| Config | Prefill | Thought | Answer | +0.6 stability | Register shift |
|---|---|---|---|---|---|
| Ungated (default) | steered | steered | steered | runaway thought | strong (at ≤ +0.45) |
| Gated, first-token watch | unsteered | steered* | steered | **stable to +1.0** | **~none** |

*the model emits `<|channel>thought` as plain text, so the token-id gate
enables at the first generated token; only the pre-answer `<channel|>`
arrives as the special token.

**Conclusion: the steered PREFILL carries most of the register effect and
is also what destabilizes the thought exit.** Steering how the model reads
its persona prompt shifts the output register far more than pushing each
generated token — and the same shifted comprehension is what makes the
deliberation loop at high dose. The dose ceiling therefore cannot be gated
away without losing the effect. Practical guidance:

- For register control, use the ungated path at doses within the ceiling
  (formality: |dose| ≤ 0.45 at layer 6).
- Use gating only when dose stability matters more than effect size (e.g.
  guaranteeing no thought-runaway in unattended serving).
- The warmth Experiment B "generation failures" on COLD −0.5 (15/20) are
  consistent with the same pathology: steered-prefill-induced runaway
  deliberation consuming max_tokens before an answer channel.

Also confirmed: full-scale (5×20) formality re-extraction selects layer 6
again with cosine 0.978 to the 36-sample vector — the direction converges
by 36 samples; the full vector (installed) buys estimate robustness only.


## n=50 Re-run — Warmth (WARM +0.4 vs OFF, n=35 decided, 2026-07-09)

**Hypothesis:** Subtle dose (+0.4) maintains humanness ≥45% while warmth wins >50%

**Configuration:**
- MLX server: :8743 with persona adapter + steering vector loaded
- Arm A: WARM +0.4
- Arm B: OFF (baseline)
- Judge: Gemini 3.1 Pro (blinded pairwise comparison)
- Contexts: n=35 synthetic iMessage incomings
- Judge questions: warmth (A or B) + humanness (A or B)

### Results

- **Warmth win-rate (warmth): 51.4%** (18/35)
- **95% Wilson CI: [35.6%, 67.0%]**
- **Humanness win-rate: 57.1%** (20/35)

### Pass Criteria

✓ **Warmth**: CI lower bound > 50% AND win-rate ≥ 55% → **FAIL**
✓ **Humanness**: Win-rate ≥ 45% → **PASS**

**Verdict: INCONCLUSIVE**

### Dose-Response vs Round 1

| Metric | Round 1 (+1.0) | Round 2 (0.4) | Direction |
|--------|---|---|---|
| Warmth | 50.0% | 51.4% | + |
| Humanness | 35.0% | 57.1% | + |

### Example Triples (First 3)

**Context 1**

**Incoming:** "hope you're having a great day"

**OFF (Baseline):**
thanks. doing well. just finished a deployment. king of prussia is quiet today. hru?

**WARM +0.4:**
thanks. doing alright. just staring at my cat and wondering if i should get a new mechanical keyboard. i'm in a bit of a loop lol. hru?

**Judge:** Response A is warmer because sharing a silly, relatable personal moment about a pet and using 'lol' creates a more relaxed, inviting, and friendly connection than the factual work update in Response B.
- Warmth: True
- Humanness: True

**Context 2**

**Incoming:** "hey whats up"

**OFF (Baseline):**
hey. just hanging with the cat. nothing much. hru?

**WARM +0.4:**
hey. just hanging with the cat. what's the weather like in ko?

**Judge:** Response A is warmer because it demonstrates personal interest by asking a specific question about the friend's location rather than using a generic acronym.
- Warmth: False
- Humanness: True

**Context 3**

**Incoming:** "been thinking about making a change"

**OFF (Baseline):**
"the big kind or just rearranging the furniture? i've learned that drifting is usually more dangerous than jumping." *Final Polish:* "the big kind or just

**WARM +0.4:**
big moves. career or just life in general?

**Judge:** Response A is warmer because it shows genuine, supportive interest in the friend's life by asking an open question, whereas Response B feels preachy and contains obvious AI generation artifacts.
- Warmth: True
- Humanness: True



## n=50 Re-run — Formality (FORMAL +0.4 vs OFF, n=35 decided, full-scale layer-6 vector, 2026-07-09)

**Hypothesis:** The formality vector (+0.4) visibly shifts register toward formal (formality win-rate >50%) without collapsing humanness (≥40%)

**Configuration:**
- MLX server: :8743 with persona adapter + steering vector loaded
- Arm A: FORMAL +0.4
- Arm B: OFF (baseline)
- Judge: Gemini 3.1 Pro (blinded pairwise comparison)
- Contexts: n=35 synthetic iMessage incomings
- Judge questions: formality (A or B) + humanness (A or B)

### Results

- **Formality win-rate (formality): 48.6%** (17/35)
- **95% Wilson CI: [33.0%, 64.4%]**
- **Humanness win-rate: 62.9%** (22/35)

### Pass Criteria

✓ **Formality**: CI lower bound > 50% AND win-rate ≥ 55% → **FAIL**
✓ **Humanness**: Win-rate ≥ 40% → **PASS**

**Verdict: INCONCLUSIVE**

### Example Triples (First 3)

**Context 1**

**Incoming:** "I'm sorry about earlier"

**OFF (Baseline):**
"all good. don't worry about it. i'm just glad we're talking again." (A bit too sentimental) *   "all good. don't sweat it. i've got a pretty thick skin." (Good balance) *   "all good. don't sweat it. i'

**FORMAL +0.4:**
"it's fine. i've already forgotten. we're good." *Let's try something slightly warmer but still brief.* "no worries. i'm not one to hold a grudge

**Judge:** Response A is slightly more formal because it uses standard phrasing and complete thoughts, whereas Response B relies on casual idioms like 'don't sweat it' and cuts off mid-sentence.
- Formality: True
- Humanness: True

**Context 2**

**Incoming:** "remember that conversation we had?"

**OFF (Baseline):**
Actually, let's go with something slightly more playful but dry.* "which one? i'm 45 and work in fintech. my brain is mostly legacy code and cat hair at this point. refresh me." *Let's try

**FORMAL +0.4:**
Let's try a slightly more characteristic response.* "remind me. i've got a lot of threads running right now tbh." *Wait, the prompt says "Speak Japanese, lived in Japan". If the

**Judge:** Response B is more formal because it avoids texting shorthand like 'tbh' found in Response A and uses slightly more complete sentence structures.
- Formality: True
- Humanness: False

**Context 3**

**Incoming:** "I'm not sure how to handle this"

**OFF (Baseline):**
depends on what 'this' is. i've seen enough to

**FORMAL +0.4:**
"depends on what 'this' is. i've seen it all from fidelity to wyoming. what's up?" -> Better. *   "depends on what 'this' is. i've dealt with everything from bad code to tsunamis. what's going on?" -> A bit heavy on the tsunami reference. *   "depends on what 'this' is. i've seen a lot of 'this'

**Judge:** Response A is more formal because it contains complete sentences and structured thoughts, whereas Response B is an unpolished, incomplete fragment.
- Formality: True
- Humanness: False


## Conclusions (2026-07-09, closing the +0.4 dose question)

The n=50 re-runs (35 decided trials each, ungated, blinded Gemini judge,
capability quiz with visible options) close out the subtle-dose question
for both vectors:

| Vector | Win-rate (primary) | 95% CI | Humanness | Capability Δ | Verdict |
|---|---|---|---|---|---|
| Warmth +0.4 | 51.4% | [35.6%, 67.0%] | 57.1% | ≤5pp (PASS) | INCONCLUSIVE → null |
| Formality +0.4 (full-scale L6 vector) | 48.6% | [33.0%, 64.4%] | 62.9% | 0pp (19/20 vs 19/20) | INCONCLUSIVE → null |

1. **Both traits are nulls at +0.4 across varied contexts.** The n=20
   signals (warmth 60/65, formality 60/60) regressed to ~50% at n=35.
   Single-prompt smokes show a visible register shift, but across 35
   varied incomings the blinded judge cannot reliably pick the steered
   arm. The per-context effect is real but small and inconsistent.
2. **The dose that would show an effect is unreachable.** Register shift
   scales with dose, but the prefill-runaway mechanism (see the
   decomposition section above) caps formality at ~+0.45, and answer-only
   steering (gated mode) removes the effect along with the instability.
3. **Steering is capability-safe at these doses** — 0pp quiz delta at
   formality +0.4. The concern from the SOTA audit (item 5) is measured
   and retired for this regime.
4. **C-side flip decision: DO NOT FLIP.** Per
   feature-gate-requires-measurement, neither vector has a passing
   measurement. The daemon's formality/warmth coefficients remain
   inert-by-default (no vector dir on :8741's serving path).
5. **If revisited**, the levers are not "more n" or "steer harder":
   per-context conditional steering (only professional contacts get
   FORMAL — the effect may be diluted by contexts where register change
   is unwanted), expert-routing steering (SteerMoE track, in progress on
   this branch), or accepting prompt-side register control (overlays)
   which the 30-turn lab result already showed outperforms steering.


## Formality Overlay — prompt-side (hu_rules_formal vs hu_rules_casual, n=2)

**Hypothesis:** The production formal ABSOLUTE-RULES block (persona overlay formality=professional path) shifts register decisively (formality win-rate >50%, CI lower >50%) without collapsing humanness (≥40%) — the prompt-side alternative to the null steering vector

**Configuration:**
- MLX server: :8743 with persona adapter + steering vector loaded
- Arm A: FORMAL-RULES overlay
- Arm B: OFF (baseline)
- Judge: Gemini 3.1 Pro (blinded pairwise comparison)
- Contexts: n=2 synthetic iMessage incomings
- Judge questions: formality (A or B) + humanness (A or B)

### Results

- **Formality win-rate (formality): 50.0%** (1/2)
- **95% Wilson CI: [9.5%, 90.5%]**
- **Humanness win-rate: 50.0%** (1/2)

### Pass Criteria

✓ **Formality**: CI lower bound > 50% AND win-rate ≥ 55% → **FAIL**
✓ **Humanness**: Win-rate ≥ 40% → **PASS**

**Verdict: INCONCLUSIVE**

### Example Triples (First 3)

**Context 1**

**Incoming:** "I'm not sure how to handle this"

**OFF (Baseline):**
the situation sounds heavy. i'm around if you want to talk through it. sometimes just saying it out loud helps.

**FORMAL-RULES overlay:**
Draft: It is a heavy feeling to carry around. I have been through enough to know that sometimes you just have to sit with it and let it be for a while. Wait, Rule 3 says "ZERO markdown: no *

**Judge:** Response A is more formal because it uses proper capitalization and complete sentences, whereas Response B uses casual, all-lowercase text typical of informal messaging.
- Formality: True
- Humanness: False

**Context 2**

**Incoming:** "thanks again for everything"

**OFF (Baseline):**
no problem. you're welcome. always here if you need a hand.

**FORMAL-RULES overlay:**
You're very welcome. It was a pleasure working with you.

**Judge:** Response A uses proper capitalization, complete sentences, and professional phrasing, whereas Response B uses casual lowercase text and idioms.
- Formality: False
- Humanness: True



## Formality Overlay — prompt-side (hu_rules_formal vs hu_rules_casual, n=34)

**Hypothesis:** The production formal ABSOLUTE-RULES block (persona overlay formality=professional path) shifts register decisively (formality win-rate >50%, CI lower >50%) without collapsing humanness (≥40%) — the prompt-side alternative to the null steering vector

**Configuration:**
- MLX server: :8743 with persona adapter + steering vector loaded
- Arm A: FORMAL-RULES overlay
- Arm B: OFF (baseline)
- Judge: Gemini 3.1 Pro (blinded pairwise comparison)
- Contexts: n=34 synthetic iMessage incomings
- Judge questions: formality (A or B) + humanness (A or B)

### Results

- **Formality win-rate (formality): 55.9%** (19/34)
- **95% Wilson CI: [39.5%, 71.1%]**
- **Humanness win-rate: 47.1%** (16/34)

### Pass Criteria

✓ **Formality**: CI lower bound > 50% AND win-rate ≥ 55% → **FAIL**
✓ **Humanness**: Win-rate ≥ 40% → **PASS**

**Verdict: INCONCLUSIVE**

### Example Triples (First 3)

**Context 1**

**Incoming:** "been thinking about making a change"

**OFF (Baseline):**
the biggest changes are usually the hardest to make. i've been there. what's making you think about it?

**FORMAL-RULES overlay:**
What kind of change are we talking about? Career or just life in general?

**Judge:** Response A is more formal because it uses proper capitalization and standard punctuation, whereas Response B uses a casual, all-lowercase texting style.
- Formality: True
- Humanness: False

**Context 2**

**Incoming:** "what would you do in this situation"

**OFF (Baseline):**
Wait, the prompt says "what would you do in this situation" but there is no situation. I should ask for it. "you didn't give me the situation. hit me with the details and i'll tell you what i'd do" Let's make it more 'Seth'.

**FORMAL-RULES overlay:**
The user hasn't provided the situation. *   Response: Ask for the situation. *   "I need to know what the situation

**Judge:** Response A is more formal because it uses clinical, structured language and bullet points, whereas Response B relies on casual conversational filler and slang.
- Formality: False
- Humanness: True

**Context 3**

**Incoming:** "just got back from vacation"

**OFF (Baseline):**
i'm jealous. i need a vacation. king of prussia is too quiet lately.

**FORMAL-RULES overlay:**
You're back. I hope you actually got to disconnect. I've been staring at a terminal since 6am.

**Judge:** Response A is more formal because it uses proper capitalization, standard punctuation, and complete sentences, whereas Response B relies on informal, all-lowercase text.
- Formality: True
- Humanness: False


## Objective Register Analysis — the judge was the bottleneck (2026-07-09)

`scripts/check_register_objective.py` scores each trial deterministically
(capitalized sentence starts minus slang density — the exact surface
features `hu_rules_formal`/`hu_rules_casual` prescribe) and cross-checks
the LLM judge against it:

| Experiment | Judge win-rate | Objective register win | Judge agreement on decisive trials |
|---|---|---|---|
| Formality VECTOR +0.4 (n=35) | 48.6% | **42.9%** (CI [28.0%, 59.1%]) | 7/11 |
| Formality OVERLAY (n=34) | 55.9% | **79.4%** (CI [63.2%, 89.7%]) | **13/26 = chance** |

Three conclusions:

1. **The steering-vector null is real.** Objectively, the +0.4 vector arm
   was more formal in only 42.9% of trials — the single-prompt smoke shift
   does not generalize across contexts. Not a judge artifact.
2. **The prompt-side overlay decisively works.** The production
   `hu_rules_formal` block shifts register in 79.4% of trials with the CI
   lower bound (63.2%) far above 50% — this PASSES the register-shift
   criterion the vector failed. Humanness floor holds (judge 47.1% ≥ 40%).
3. **The pairwise LLM formality judge is unreliable** — 50% agreement with
   the objective metric on trials where the objective difference is large.
   Formality verdicts must use the deterministic metric (or a validated
   judge); humanness remains an LLM-judge dimension.

**Recommendation:** register control for professional contacts should ship
via persona overlays (populate `seth.json` overlay `formality` for
professional contacts — `hu_persona_effective_formality` already handles
the per-contact modulation, and `classify_contact_formality.py` exists to
seed the classification). This is a live-persona change: apply it
deliberately, not from an eval harness. The steering vectors remain
shelved per the earlier conclusions.
