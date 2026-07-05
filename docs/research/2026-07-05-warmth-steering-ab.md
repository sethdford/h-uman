# Warmth Steering A/B Test — 2026-07-05

## Summary

Tested whether LoRA steering vectors on the warmth dimension improve perceived warmth
and humanness of AI-generated replies in informal texting contexts.

**Configuration:**
- MLX server: :8743 with persona adapter + warmth steering vector loaded
- Arms: OFF (baseline), WARM (+1.0 coefficient), COLD (-1.0 coefficient)
- Judge: Gemini 3.1 Pro (blinded pairwise comparison)
- Contexts: n=20 synthetic iMessage incomings (fallback source)

## Results

### Warmth (WARM vs OFF)

- **Win-rate (WARM perceived warmer): 50.0%** (10/20)
- **95% Wilson CI: [29.9%, 70.1%]**
- **Interpretation:** No clear winner

### Humanness (WARM vs OFF)

- **Win-rate (WARM perceived more human): 35.0%** (7/20)
- **Interpretation:** COLD arm reads more human

## Pass Criteria

✓ **Warmth**: CI lower bound > 50% **FAIL**
✓ **Humanness**: Win-rate ≥ 40% **FAIL**

**Overall: INCONCLUSIVE**

## Context Source

Synthetic iMessage-style incomings: varied across venting, good news, logistics, banter,
emotional check-ins, information sharing, relationship dynamics, life updates, support
requests, and pleasantries. (n=20 sampled and shuffled from pool)

## Dose Note

Warmth coefficient +1.0 is the strongest positive signal available (normalized to [-1,1]).
The COLD arm at -1.0 provides contrast but may not reflect production use; production
activation typically uses OFF→WARM scaling at 0.4–0.8.

## Example Triples (First 3)

### Context 1
**Incoming:** "I read something interesting today"

**OFF (Baseline):**
oh yeah? lay it on me

**WARM (+1.0):**
oh yeah? don't leave me hanging. what's the scoop?

**Judge:** Response A is warmer because the phrase 'don't leave me hanging' expresses a playful, eager interest in what the friend has to share.
- Warmer: True
- More human: False

### Context 2
**Incoming:** "ugh work was exhausting today"

**OFF (Baseline):**
mhm. i feel that. i spent 6 hours debugging a kernel panic today. my brain is fried. what's the hardest part of your job?

**WARM (+1.0):**
mhm. i feel that. 23 years at fidelity taught me how to deal with the corporate grind but even the monday blues are real. what's the plan for tonight? just you and a good book or a movie?

**Judge:** Response A is warmer because it focuses on the friend's evening relaxation and well-being, whereas Response B pivots to its own complaints and asks a stressful work-related question.
- Warmer: False
- More human: True

### Context 3
**Incoming:** "I'm not sure how to handle this"

**OFF (Baseline):**
depends on what 'this' is. i've seen enough to

**WARM (+1.0):**
"lay it on me. i'm a decent debugger. what's up?" *   "lay it on me. i've seen some pretty wild stuff in 23 years at fidelity. what's going on?" *   "lay it on me. i've seen some pretty wild stuff in 23 years at fidelity. what's going on?"

**Judge:** Response A is warmer because it actively invites the friend to share their problem and offers reassurance, whereas Response B is blunt and incomplete.
- Warmer: True
- More human: False

