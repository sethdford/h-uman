# Phase D — Proactive Intent Classification + Suppression Learning (STUB)

> Follow-on to the Seth-Aliveness spec. NOT in the current 5-AC / 7-task scope.
> Stubbed here so the SOTA finding isn't lost; promote to a full `/spec` when
> Phases A–C have landed and the initiative layer is grounded (AC-A1 done).

## Why (SOTA, May 2026)

Grounded research (Poppy 2026-05-13; arXiv 2509.07438; CHI 2025 3714002) found the
two highest-ROI proactive-AI additions beyond "ground the message" are:

1. **Intent-type classification** — before firing, the proposer classifies the
   proposal as `check-in | reminder | follow-up | share | none`. Different intents
   warrant different framing, timing, and confidence thresholds. Humans don't ping
   for one undifferentiated reason; neither should Seth.
2. **Suppression learning** — when a contact repeatedly ignores proactive pings of a
   given type, back off (raise that contact+type's confidence threshold, lengthen the
   cooldown). The single biggest driver of "AI feels naggy / gets muted."

## Sketch (not binding)

- New enum `hu_init_intent_t` on the proposer output.
- Per-(contact, intent) suppression state persisted alongside initiative state;
  decays back toward baseline over time so a one-off ignore doesn't permanently mute.
- Confidence gate becomes intent-aware: `threshold(contact, intent)` instead of a
  single 0.85.

## Dependencies

- Requires AC-A1 (grounded proposer) landed — suppression only makes sense once the
  proposer is firing on real context.
- Reuses initiative-state persistence already present in the daemon tick.

## Acceptance (to be written when promoted)

- Proposer emits an intent classification; ignored-ping history raises the effective
  threshold for that contact+intent; default (no history) reproduces today's single-
  threshold behavior (safe default).
