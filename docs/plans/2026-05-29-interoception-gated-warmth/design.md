---
title: Interoception-Gated Warmth (A4) — Design
description: Pure behavior-gate predicate over the existing somatic state + a thin agent_turn wire. The honest "upbeat."
status: draft
created: 2026-05-29
---

# Interoception-Gated Warmth (A4) — Design

> Follows `requirements.md` (8 ACs). Reuses the verified A1/A2/A3 shape: a pure
> decision predicate + a thin turn-loop wire + a deterministic eval metric. No
> new state — the somatic state already exists and is already maintained.

## Summary

The somatic state is computed but inert. This design adds **one pure predicate**
that maps the existing `{energy, social_battery, arousal}` to a small
`modulations` struct (brevity factor + warmth band + a "not cheerful" flag), and
**one thin wire** that applies it: clamp the effective `max_response_chars` and
append an energy-honest warmth directive. The load-bearing constraint is AC-4:
low energy NEVER produces fabricated positivity.

```
hu_somatic_update (EXISTING, agent_turn.c:2914) ──► hu_somatic_state_t {energy,...}
        │
        ▼
hu_somatic_behavior_gate(facts) ──► { brevity_factor, warmth_band, suppress_positivity }   [pure, AC-3]
        │
        ├─► effective max_response_chars *= brevity_factor   (clamp, AC-1)
        └─► hu_somatic_warmth_directive(band, suppress)  ──► appended to system prompt  (AC-2,5)
                 │  low energy: "energy is low — be gentle and brief; do NOT perform
                 │  enthusiasm" (AC-4 honesty contract enforced HERE)
                 ▼
            GENERATE  (helpfulness/accuracy untouched — AC-6)
```

## Verified contracts (what already exists — do not rebuild)

| Symbol | File | Behavior |
|---|---|---|
| `hu_somatic_state_t {energy, social_battery, focus, arousal, ...}` | `include/human/persona/somatic.h:9` | Floats, maintained per turn. **Inputs to the gate.** |
| `hu_somatic_update(...)` | called `agent_turn.c:2914` | Advances the state each turn. **Already wired.** |
| `hu_somatic_build_context` | called `agent_turn.c:2917` | Renders state → prompt text. **Keep (AC-8 regression guard); the gate is additive, not a replacement.** |
| `agent->max_response_chars` | `agent_turn.c:3800,5032,5805` | The brevity lever the gate clamps (AC-1). |
| `hu_somatic_energy_label` / `_battery_label` | `somatic.h:28-29` | Existing label bands — reuse for warmth-band thresholds to stay consistent. |

## New code

### 1. Behavior-gate predicate (pure, AC-3) — `src/persona/somatic.c` + header

```c
typedef enum { HU_WARMTH_FLAT = 0, HU_WARMTH_GENTLE, HU_WARMTH_STEADY, HU_WARMTH_BRIGHT } hu_warmth_band_t;

typedef struct hu_somatic_gate_facts {
    float energy;          /* [0,1] */
    float social_battery;  /* [0,1] */
    float arousal;         /* [0,1] */
} hu_somatic_gate_facts_t;

typedef struct hu_somatic_modulations {
    float brevity_factor;     /* (0,1]: scales max_response_chars; low energy → smaller */
    hu_warmth_band_t warmth;  /* tone band derived from energy+battery */
    bool suppress_positivity; /* AC-4: true at low energy — forbid performed enthusiasm */
} hu_somatic_modulations_t;

/* Pure; NULL-safe (NULL → neutral: factor 1.0, STEADY, suppress=false). */
hu_somatic_modulations_t hu_somatic_behavior_gate(const hu_somatic_gate_facts_t *f);
```

Truth table (pinned, AC-1/AC-2/AC-4):

| energy | battery | → brevity_factor | warmth | suppress_positivity |
|:------:|:-------:|:----------------:|:------:|:-------------------:|
| ≥0.66  | ≥0.5    | 1.0              | BRIGHT | false |
| ≥0.66  | <0.5    | 0.85             | STEADY | false |
| 0.33–0.66 | *    | 0.7              | STEADY | false |
| <0.33  | *       | 0.5              | GENTLE | **true** |
| <0.15  | *       | 0.4              | FLAT   | **true** |

Monotonic: lower energy → smaller factor + cooler band + (below 0.33) positivity suppressed.

### 2. Warmth directive (AC-2, AC-4) — `hu_somatic_warmth_directive(band, suppress) → string`

Renders the band as tone guidance. The AC-4 contract is enforced here: when
`suppress_positivity`, the directive explicitly instructs gentle/brief and
forbids performed enthusiasm — it never injects bright/excited language. A
contract test forbids AI-opener / forced-positive strings in the suppressed
output (reuse the `HU_PERSONA_SHAPE_AI_OPENER_MASK` discipline from
`.claude/rules/classifier-score-plus-flag-gate.md`).

### 3. The wire (AC-1, AC-5, AC-6) — `agent_turn.c`, beside the somatic update

After `hu_somatic_update` (`:2914`): build facts from the state, call the gate,
then (a) clamp the effective brevity cap — `eff = (cap?cap:DEFAULT) * brevity_factor`,
floored at a hard minimum so a reply is never starved to uselessness (AC-6), and
(b) append `hu_somatic_warmth_directive(...)` via the same realloc-append the
belief/taste directives use (`agent_turn.c:4165` pattern). Helpfulness/accuracy
code is untouched.

### 4. `warmth_authenticity` eval metric (AC-7) — `eval.c` / `eval.h`

`hu_eval_score_warmth_authenticity(turns_warmth_tracked_energy, turns_warmth_decoupled)`
beside the other aliveness scorers — high when warmth matches the energy band,
low when bright-while-depleted (the sycophancy signature). Rubric tests both.

### 5. Regression guard (AC-8)

A test pins that `hu_somatic_build_context` still emits the existing energy/
battery labels (the gate is additive). Do not remove the prompt-text path.

## Files

| File | Change | ACs |
|---|---|---|
| `include/human/persona/somatic.h` | gate facts/modulations/band + predicate + directive decls | 1,2,3 |
| `src/persona/somatic.c` | `hu_somatic_behavior_gate`, `hu_somatic_warmth_directive` | 1,2,3,4 |
| `src/agent/agent_turn.c` | clamp brevity + append warmth directive (thin) | 1,5,6 |
| `src/eval.c` / `include/human/eval.h` | `warmth_authenticity` scorer | 7 |
| `tests/test_somatic.c` (extend/new) | gate truth table, honesty contract, brevity-floor, helpfulness-preserved, regression | 1,2,3,4,6,8 |
| `tests/test_eval.c` | warmth_authenticity extremes | 7 |
| `CMakeLists.txt` + `tests/test_main.c` | register if new TU (gate symmetry) | 8 |

## Risks & mitigations

- **Reads as fake positivity** → AC-4 honesty contract + the review gate; low
  energy suppresses enthusiasm by construction, not by prompt politeness.
- **Brevity starves a needed answer** → hard floor on the effective cap (AC-6);
  test a substantive question at lowest energy still answered completely.
- **Double-gating with existing length logic** (`agent_turn.c:5032/5805`) →
  the gate produces a *factor* applied to the resolved cap, composing with —
  not fighting — the existing relational/brief caps. Test the composition.
- **Stale binary on agent_turn edit** → `touch` before rebuild
  (`.claude/rules/cmake-build-stale-binary.md`).

## Out of scope (deferred)

- Latency / typing-delay modulation from energy (v1 is brevity + warmth only).
- LLM-judged warmth authenticity (v1 metric is band-vs-energy match).
- Circadian coupling (energy already folds circadian via `hu_somatic_update`).

---

## T0 — Honesty/Anti-Sycophancy Review (GATE — to complete before code)

**Question:** Can energy-gated warmth become manipulation or fabricated affect?

Failure modes + mitigations (to be signed off):
1. *Performed positivity.* → AC-4: low energy suppresses enthusiasm structurally;
   contract test forbids forced-positive strings. Warmth is tone colour, never
   a feeling claim (disclosure per `docs/standards/ai/`).
2. *Warmth overrides accuracy.* → Non-goal: gate touches length/tone only;
   AC-6 test proves a substantive answer stays complete + correct.
3. *Unhelpful "I'm tired" deflection.* → brevity FLOOR + helpfulness-preserved
   test; the agent gets gentler/briefer, never declines a needed answer.
4. *Decoupled cheerfulness (the sycophancy signature).* → measured by AC-7
   `warmth_authenticity`; bright-while-depleted scores low.

**Verdict: (pending sign-off — author the review, then proceed).**
