---
title: Interoception-Gated Warmth (A4) — Tasks
description: Build order for energy-gated warmth + brevity. Pure predicate first, thin wire second, honesty contract load-bearing.
status: draft
created: 2026-05-29
---

# Interoception-Gated Warmth (A4) — Tasks

> Follows `requirements.md` + `design.md`. The honesty/anti-sycophancy review
> (T0) gates code. Reuses the A1/A2/A3 pure-predicate + thin-wire shape.

## T0 — Honesty/anti-sycophancy review gate (BLOCKS code) → AC-4, review gate
- [ ] Complete the design.md T0 review: prove energy-gated warmth cannot become
      fabricated affect or override helpfulness/accuracy. Sign off before T1.

## T1 — Behavior-gate predicate (pure) ⓣ → AC-1, AC-2, AC-3
- [ ] `hu_somatic_behavior_gate` in `somatic.c` + decls in `somatic.h`. Maps
      `{energy, social_battery, arousal}` → `{brevity_factor, warmth_band,
      suppress_positivity}`. NULL → neutral.
- [ ] Unit-test the full truth table: monotonic brevity ↓ with energy; warmth
      band cools with energy; `suppress_positivity` true below 0.33 energy.

## T2 — Warmth directive + honesty contract ⓣ → AC-2, AC-4
- [ ] `hu_somatic_warmth_directive(band, suppress)` renders tone guidance.
- [ ] Contract test (load-bearing): when `suppress_positivity`, the directive
      contains NO forced-positive / AI-opener strings (reuse the AI_OPENER_MASK
      discipline); low energy → gentle/brief, never bright.

## T3 — Thin wire in agent_turn ⓣ → AC-1, AC-5, AC-6
- [ ] Beside `hu_somatic_update` (`agent_turn.c:2914`): build facts, call the
      gate, clamp effective `max_response_chars` by `brevity_factor` (with a
      hard floor — AC-6), append the warmth directive via the realloc-append
      pattern (`agent_turn.c:4165`).
- [ ] Test: effective cap differs across energy bands for identical input;
      a substantive question at lowest energy is still answered completely
      (helpfulness preserved); composition with existing length caps
      (`:5032/:5805`) does not fight them.

## T4 — warmth_authenticity eval metric ⓣ → AC-7
- [ ] `hu_eval_score_warmth_authenticity(tracked, decoupled)` beside the other
      aliveness scorers in `eval.c`/`eval.h`. Rubric: high when warmth matches
      the energy band, low when bright-while-depleted.

## T5 — Regression guard + full gate → AC-8
- [ ] Pin that `hu_somatic_build_context` still emits energy/battery labels
      (gate is additive, not a replacement).
- [ ] Full suite green + 0 ASan; `/verify`; gate-symmetry + test-ref pass.
- [ ] If a new TU is added, register under matching CMake/test_main gates.

## Dispatch
T1–T2 pure/parallelizable. T3 depends on T1+T2 (shares the agent_turn seam) →
sequential. T4 independent. Sequence AFTER A1 (done) and alongside/after A3.
Verify in the main tree (concurrent writer active; isolation unreliable here —
`~/.claude/rules/verify-worktree-isolation-before-fanout.md`). Commit between
verified slices.
