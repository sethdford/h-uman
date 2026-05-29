---
title: Interoception-Gated Warmth (A4) — Requirements
description: Make somatic energy GATE behavior (warmth + brevity track real internal state), explicitly distinguished from sycophancy. The honest form of "upbeat."
status: draft
created: 2026-05-29
---

# Interoception-Gated Warmth (A4) — Requirements

> The "upbeat" lever, grounded honestly. Decided 2026-05-29: warmth must track
> the agent's *real* computed energy state, never a forced-cheerfulness knob —
> the latter is exactly the sycophancy the rest of the architecture fights.

## Problem

`src/persona/somatic.c` already computes a rich interoceptive state
(`hu_somatic_state_t { energy, social_battery, focus, arousal }`), maintained
per turn (`hu_somatic_update` at `agent_turn.c:2914`). But it is **advisory
only** — `hu_somatic_build_context` renders it to prompt text and hopes the LLM
honors it. Energy does not actually *gate behavior*. So "tired" is a suggestion
the model can ignore, and "upbeat" has no anchor: nothing ties brightness to a
genuine internal state, which invites decoupled, fabricated positivity.

## Goal

Make the somatic state a **behavioral gate**, not a prompt hint: real energy
modulates reply brevity and tone-warmth deterministically, with warmth that is
*authentic* (tracks energy) rather than *performed* (constant cheerfulness).
Never at the cost of helpfulness, accuracy, or safety.

## Acceptance Criteria

- **AC-1 — Energy gates brevity (behavior, not text).** Low energy
  deterministically tightens the effective `max_response_chars`; high energy
  permits fuller replies. The clamp is applied in code, not merely advised in
  the prompt. Pinned by a test asserting the effective cap differs across
  energy bands for identical input.
- **AC-2 — Warmth tracks energy.** A warmth directive is derived from energy +
  social_battery: high → brighter/warmer tone; low → gentler, briefer, and
  explicitly NOT cheerful. The mapping is monotonic and pinned by tests.
- **AC-3 — Pure behavior-gate predicate.** The decision lives in a pure
  function `hu_somatic_behavior_gate(facts) → modulations` (no agent state, no
  I/O), per `.claude/rules/security-predicate-extraction.md`. Full truth table
  unit-tested.
- **AC-4 — Anti-sycophancy honesty contract (load-bearing).** At low energy the
  warmth directive MUST NOT inject fabricated positivity ("so excited!",
  "absolutely love this!"). A contract test forbids forced-positive/AI-opener
  strings in the low-energy directive. Warmth is tone *colour*, never a claim
  of feeling and never an override of accuracy.
- **AC-5 — Thin wire in agent_turn.** The modulations are applied at one seam:
  clamp the effective brevity cap + append the warmth directive, reusing the
  existing somatic update site (`agent_turn.c:2914`) and the realloc-append
  prompt pattern A1/A2 use.
- **AC-6 — Never degrades helpfulness.** Gating modulates length/tone ONLY. It
  never refuses a needed answer, never drops correctness, and is fully
  suppressible. A test asserts a substantive question still gets a complete,
  correct answer at lowest energy (just shorter/gentler).
- **AC-7 — `warmth_authenticity` eval metric.** A deterministic scorer beside
  `belief_flexibility`/`distinctiveness`/`self_direction`: high when expressed
  warmth tracks the energy state, low when decoupled (cheerful-while-depleted =
  the sycophancy signature). Rubric tests both extremes.
- **AC-8 — Full gate.** Full suite green + 0 ASan; gate-symmetry + test-ref
  pass; regression guard on the somatic→prompt path so existing behavior
  (energy/battery labels in context) is preserved, not replaced.

## Non-goals

- A user-facing "cheerfulness" slider (decoupled positivity is the anti-pattern).
- Changing how `hu_somatic_update` computes energy (inputs unchanged).
- Latency/typing-delay modulation (possible follow-up; AC-1 is brevity only for v1).
- Any claim of subjective feeling — warmth is a behavioral/tone layer, governed
  by `docs/standards/ai/` disclosure.

## Review gate (must pass before code)

An honesty/anti-sycophancy review (mirror A2's gate): confirm energy-gated
warmth serves the user and cannot become manipulation or fabricated affect.
Append the note to `design.md`. No code until it passes.
