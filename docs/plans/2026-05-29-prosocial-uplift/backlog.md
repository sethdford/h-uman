# Prosocial & Uplift — Backlog (B/C series)

> Authored 2026-05-29 after the A-series (interiority) landed. Where A1/A2/A3
> gave h-uman a mind of its own, the B/C series points that warmth *outward* —
> to make people smile, lift them up, reach out, support them.
>
> **DDD constraint (non-negotiable):** build in the right bounded context,
> COMPOSE existing primitives (behavior `safety`/`policy`/`trust`,
> `companion_safety`, `anticipatory`), persist via REPOSITORIES not inline SQL,
> keep domain decisions as pure predicates. (See `src/behavior/CLAUDE.md`
> "No duplication" and `.claude/rules/sqlite-includer-ratchet.md`.)

## What already exists (compose, don't rebuild — verified 2026-05-29)

| Concern | Already in | So B-series… |
|---|---|---|
| Dependency / attachment / vulnerability | `behavior/safety.c` (`hu_behavior_safety_assess`, `hu_behavior_risk_t`) | composes its risk enum |
| Warmth vs safety override | `behavior/policy.c` (safety wins over warmth) | defers to it |
| Sycophancy / flattery under pressure | `behavior/behavior_trust.c` | reuses the trust signal |
| Crisis / distress first-aid | `agent/superhuman_emotional.c` | unchanged (negative-state) |
| Anticipating needs / sad anniversaries | `context/anticipatory.c` | extends to POSITIVE events |
| Silence-biased reach-out | `agent/init_proposer.c` | routes ALL prosocial shares through it |

## The genuine gap: POSITIVE uplift + warm routines

Verified thin/absent: savoring=0, accountability=0, "uplift"=0, compliment=0,
proud_of=0, ritual=0, gratitude=2, praise=1. The system comforts you when
you're down and pings you when something's time-sensitive — but cannot
celebrate a win, encourage your goals, affirm you genuinely, or run a warm
recurring ritual.

## B-series — prosocial faculties (bounded-context placement)

| # | Faculty | Context / home | Persistence | Status |
|---|---|---|---|---|
| **B0** | **Prosocial Integrity gate** — unify the guards + add the one missing dimension (honesty about feelings) | `behavior/prosocial.c` (Modeled Person) | none (pure) | **building** |
| B1 | Win-detection & celebration | detect: `behavior/`; render: `persona/`; "already-celebrated": **`celebration_repo`** | repo, NOT inline SQL | next |
| B2 | Encouragement toward the user's own goals | `agent/` reads `goals`/`personal_model` | existing repos | |
| B3 | Gentle accountability (commitments the USER made) | extend `commitment_store` | existing | |
| B4 | Genuine affirmation (earned, specific) | `persona/`, gated by B0 | none | |
| B5 | Savoring & gratitude | `persona/` + `emotional_moments` | repo | |

## C-series — routines & rituals (ride existing scheduler + init_proposer)

C1 morning intention / evening "what went well" · C2 weekly deeper check-in ·
C3 re-surface something they cared about ("thinking of you") · C4
celebrate-on-detect. All scheduled like A3's daemon tick, all shared only
through the silence-biased proposer.

## B0 design (the foundation)

A **pure composition predicate** in the behavior context. It does NOT
re-implement dependency/sycophancy detection — it composes them and adds the
single dimension the behavior layer lacks: *does this warm output falsely
claim a feeling or sentience?*

```c
hu_prosocial_verdict_t hu_prosocial_gate(const hu_prosocial_input_t *in, uint32_t *out_flags);
```
- input composes `hu_behavior_risk_t` (dependency, from safety.c) + a flattery
  bool (from trust) + a feelings-claim bool + an overrides-need bool.
- verdict: SEND / SOFTEN / SUPPRESS, with a flag bitmask of *why*.
- **SUPPRESS on dependency risk** (matches safety.c: escalate dependency,
  never reinforce it). **SOFTEN on feeling-claim / flattery / overrides-need.**
- plus a small pure helper `hu_prosocial_text_claims_feeling(text,len)` —
  the genuinely-new dimension, word-boundary matched.

Every B/C producer (B1 celebration, B4 affirmation, C-routines) MUST pass its
candidate output through B0 before it reaches the proposer. That's the
foundational always-on element — warmth that's structurally honest and
non-manipulative.

## Honest "better than human" axes
Never forgets what mattered · never too busy · no mood-spillover · follows
through reliably · celebrates without envy. Cannot/ must-not: fake felt
emotion (B0 enforces) · foster dependency (safety.c + B0 enforce).
