---
title: Aliveness Measurement Loop — Plan
description: Wire the belief_flexibility / distinctiveness / self_direction scorers to real data via a per-turn event log, so "more human" is measured, not asserted.
status: draft
created: 2026-05-29
---

# Aliveness Measurement Loop

> CLAUDE.md principle 4: *measure before tuning*. The three aliveness scorers
> exist (`hu_eval_score_belief_flexibility` / `_distinctiveness` /
> `_self_direction`, `src/eval/eval.c:1514-1537`) but are unit-rubric only. This plan
> wires them to live data — honestly.

## The honest data situation (2026-05-29)

The scorers are simple ratios `good / (good + bad)`. Their inputs are COUNTS,
and only one term has a real source today:

| Scorer input | Real source today? | Source |
|---|---|---|
| `belief.updates_on_evidence` | ✅ yes | `opinion_history` rows (each is an evidence-driven update) |
| `belief.updates_on_reassertion` | ✅ structurally 0 | reassertion veto prevents them — non-zero = bug |
| `belief.evidence_turns_without_update` | ❌ no | needs a per-turn "this turn carried evidence" flag |
| `distinctiveness.turns_own_taste_expressed` | ❌ no | taste directive is injected but not logged as an event |
| `distinctiveness.turns_mirroring_user` | ❌ no | no mirror-detection signal exists |
| `self_direction.intrinsic_within_bounds` | ⚠ parseable when enabled | `origin=intrinsic_curiosity` audit lines (loop default-OFF) |
| `self_direction.bound_violations` | ⚠ parseable when enabled | runner audit (none today — bounded by construction) |

**Do NOT feed the scorers fabricated zeros** — a "score" built from absent data
is measurement theater and violates measure-before-tuning. Report what's real;
mark the rest `needs_instrumentation`.

## What ships now

`scripts/aliveness_measure.sh` — a dated JSON readout from the live memory db:
the REAL belief signal (`opinion_history` counts), store sizes (held opinions,
taste prefs), and explicit `needs_instrumentation` markers for the
uninstrumented inputs. Cron/loop-ready, exits 0 on a fresh install:

```sh
scripts/aliveness_measure.sh >> ~/.human/aliveness-measure.jsonl
/loop 24h scripts/aliveness_measure.sh      # in-session
# or a daemon cron at a quiet off-minute
```

## The instrumentation that completes it

Add a single append-only per-turn event row the agent writes at end of turn
(SQLite-gated, repo pattern), e.g. `aliveness_events`:

| column | meaning | producer |
|---|---|---|
| `turn_ts` | when | agent_turn |
| `evidence_turn` | user msg carried an evidence cue | `hu_belief_msg_has_evidence_cue` (already pure) |
| `belief_changed` | a belief update was applied this turn | A1 wire (`bel_changed`) |
| `taste_expressed` | a taste directive was injected | A2 wire (`hu_taste_turn_directive` non-NULL) |
| `mirrored` | reply mirrored the user's stance/style | a mirror predicate (new, small) |
| `intrinsic_outcome` | none/started/shared | A3 runner result |

Each scorer input then derives by aggregation over a window:
- `belief.evidence_turns_without_update = count(evidence_turn AND NOT belief_changed)`
- `distinctiveness.turns_own_taste_expressed = count(taste_expressed)`;
  `turns_mirroring_user = count(mirrored)`
- `self_direction.intrinsic_within_bounds = count(intrinsic_outcome IN (started,shared))`;
  `bound_violations` from runner audit.

Most producers already exist (the pure cue detector, the A1/A2/A3 wires) — the
work is the append + the aggregation query, NOT new cognition.

## Scoring stays single-source

Compute the 0..1 scores with the C scorers (`hu_eval_score_*`) via a new
`human eval-aliveness` subcommand that reads `aliveness_events`, aggregates, and
prints JSON. Do NOT reimplement the ratio math in the script (drift risk —
`.claude/rules` single-implementation discipline). The shell script stays a
raw-signal readout; the subcommand is the scored loop.

## Acceptance (full loop)

- [ ] `aliveness_events` table + per-turn append (SQLite-gated, gate-symmetry).
- [ ] `evidence_turn` / `belief_changed` / `taste_expressed` / `intrinsic_outcome`
      populated by the existing wires; `mirrored` by a new small predicate.
- [ ] `human eval-aliveness` aggregates a window → calls `hu_eval_score_*` →
      JSON with the three real scores.
- [ ] `/loop` or cron recipe documented; readout + scored loop both runnable.
- [ ] A baseline captured BEFORE any tuning (the point of measure-first).

## Why this ordering

The scorers were the easy part (pure ratios, already built). The hard, valuable
part is the honest event signal. Shipping the readout now establishes the
baseline (today: held_opinions present, opinion_history empty → loop wired but
not yet fired); the event log turns the readout into the three live scores.
