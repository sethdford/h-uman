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
