# Modeled-Person Layering — persona → cognition → behavior, Unidirectional

The `persona/`, `cognition/`, and `behavior/` directories are ONE bounded
context (the "Modeled Person"), not three. They model a person across three
layers with a unidirectional per-turn flow:

```
persona/    expression  (identity, voice, style, boundaries)
   │
   ▼
cognition/  perception  (emotion, trust, attachment, presence)
   │
   ▼
behavior/   decision    (relational acts, intensity, modulation)
```

The layers communicate ONLY through the two aggregate roots — `hu_persona_t`
(`human/persona.h`) and `hu_personal_model_t` (`human/memory/personal_model.h`)
— plus shared `human/core/*` and `human/data/*` infra.

## The rule (forbidden include directions)

A file in one layer must NOT `#include` another layer's header in these
directions:

- `src/cognition/` (or `include/human/cognition/`) → `human/behavior/*`  — sibling cross-talk
- `src/behavior/` → `human/cognition/*`  — sibling cross-talk
- `src/persona/` → `human/cognition/*`  — the expression root must not depend on perception
- `src/persona/` → `human/behavior/*`  — the expression root must not depend on decision

New "person" state belongs to one of the two aggregate roots, not a fourth
scattered struct. If `behavior/` needs a cognition result, it reads it off the
aggregate root the cognition layer wrote — it does not include the cognition
header.

## Why

This is the one Modeled-Person boundary the DDD plan only *documented*
(`docs/standards/engineering/bounded-contexts.md`) — every other context
boundary has an enforcing ratchet. Measured baseline 2026-05-29: all four
directions are **0** (clean). The guard freezes that; it fails only on growth.
Sibling cross-talk (cognition↔behavior) is the coupling that would collapse the
three layers into a mud-ball and defeat the unidirectional flow.

## Ratchet, not absolute

Baselines are frozen at the measured count (currently 0/0/0/0). Lower a baseline
to lock any future drop; the guard only fails when a count grows past its
ceiling.

## Enforcement

`scripts/check-modeled-person-layering.sh` (scans both `src/<layer>/*.c` and
`include/human/<layer>/*.h`), wired into `.githooks/pre-commit` (fires when a
persona / cognition / behavior source or header is staged).

## Related

- `docs/standards/engineering/bounded-contexts.md` — "Modeled Person — one context, three layers"
- `docs/standards/engineering/modeled-person-module-shape.md` — the canonical shape for modules in these layers
- `.claude/rules/edge-context-isolation.md` — the sibling ratchet this is modeled on
- `.claude/rules/agent-core-boundary.md` — the orchestration-core boundary (Conversation context)
