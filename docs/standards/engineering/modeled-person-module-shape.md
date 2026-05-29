---
title: Modeled-Person Module Shape
---

# Modeled-Person Module Shape

The canonical shape for a module in the **Modeled Person** bounded context
(`persona/` expression · `cognition/` perception · `behavior/` decision — see
[bounded-contexts.md](bounded-contexts.md)). These modules encode human
characteristics, behaviors, traits, and routines. They drifted into five
different shapes (pure-predicate, inline-policy, raw-sqlite, untested, silent);
this standard unifies them so a new "person" module is normal, testable, and
DDD-clean by construction. It consolidates five existing rules into one template
— it does not replace them.

**Cross-references:** [bounded-contexts.md](bounded-contexts.md),
[principles.md](principles.md), [naming.md](naming.md), [testing.md](testing.md)

---

## The canonical shape

A human-characteristic module is built from these parts, in this order:

1. **A pure decision predicate** — the heart of the module is a function that
   takes plain facts (no agent state, no I/O, no SQLite) and returns a decision
   (an enum, a bool, or a small modulations struct). Reproducible, exhaustively
   unit-testable without a database or a turn. Rule:
   [`security-predicate-extraction`](../../../.claude/rules/security-predicate-extraction.md).
   *Exemplars:* `hu_belief_update_decide` (`src/agent/belief_update.c`),
   `hu_taste_express_decide` (`src/persona/taste.c`),
   `hu_intrinsic_should_start` (`src/agent/intrinsic_drive.c`).

2. **A thin wire at ONE seam** — the predicate is invoked from a single place
   (typically the `agent_turn.c` realloc-append directive pattern, or a daemon
   tick). The wire derives the facts, calls the predicate, applies the result.
   No policy logic in the wire — it only plumbs. Keeps the decision testable and
   the call site auditable.

3. **Persistence via a repository, never raw `sqlite3*`** — module state that
   outlives a turn goes through a `hu_<aggregate>_repo_t` interface
   (`include/human/memory/<aggregate>_repo.h`), not a raw handle from
   `hu_sqlite_memory_get_db()`. SQL lives only in `src/memory/repos/`. Rule:
   [`sqlite-includer-ratchet`](../../../.claude/rules/sqlite-includer-ratchet.md).
   *Exemplar:* `src/memory/repos/boundary_repo_sqlite.c`.

4. **Config gate with a one-shot log** — a module that can be turned off is
   gated on `cfg.<area>.enabled` (default chosen deliberately; default-OFF for
   anything autonomous) and emits exactly ONE operator-visible log line on first
   tick when disabled, naming the config key. No silent no-ops. Rule:
   [`silent-config-gated-subsystems`](../../../.claude/rules/silent-config-gated-subsystems.md).
   *Exemplar:* `hu_intrinsic_run_tick`'s disabled one-shot log.

5. **Word-boundary classifiers** — any string→bucket routing uses word-boundary
   matching, never naive substring (`"informal"` must not match `"formal"`; see
   the global `substring-classifier-pitfalls` rule). When a classifier returns
   score AND flags, gate on BOTH
   ([`classifier-score-plus-flag-gate`](../../../.claude/rules/classifier-score-plus-flag-gate.md)).

6. **A test, gate-symmetric with its source** — every module has a
   `tests/test_<module>.c` that references a production symbol and pins the
   predicate truth table (happy + edge + the previously-misrouted input). If the
   source is feature-gated, the test is gated symmetrically. Rules:
   [`test-source-gate-symmetry`](../../../.claude/rules/test-source-gate-symmetry.md),
   [`test-references-production-symbol`](../../../.claude/rules/test-references-production-symbol.md).

7. **Layer-correct placement** — expression→`persona/`, perception→`cognition/`,
   decision→`behavior/`. No cognition↔behavior cross-includes; communicate
   through the aggregate roots (`hu_persona_t`, `hu_personal_model_t`). Enforced
   by [`modeled-person-layering`](../../../.claude/rules/modeled-person-layering.md).

## New-module audit checklist

- [ ] Core decision is a pure predicate (facts in → decision out, no I/O/state)
- [ ] Predicate truth table is unit-tested (incl. the adversarial/edge row)
- [ ] Wired at exactly one seam; the wire contains no policy
- [ ] Any persistence goes through a `*_repo_t`, not raw `sqlite3*`
- [ ] If gateable: `cfg.<area>.enabled` + one-shot disabled log naming the key
- [ ] Any string→bucket routing is word-boundary, not substring
- [ ] `tests/test_<module>.c` exists, references a production symbol, gate-symmetric
- [ ] Lives in the correct layer; no forbidden cross-layer include
- [ ] Frees every allocation (ASan clean); no silent failures

## Layer placement quick reference

| If the module… | Layer | Dir |
|---|---|---|
| renders identity / voice / style / boundaries into output | expression | `src/persona/` |
| perceives/classifies emotion, trust, attachment, novelty, presence | perception | `src/cognition/` |
| decides a relational act, intensity, modulation, or gate | decision | `src/behavior/` |
| persists person-state across turns | (root + repo) | `src/memory/repos/` behind a `*_repo_t` |

---

## Modeled-Person context map (verified-distinct clusters)

Several module names look duplicative but were verified (2026-05-29, by reading)
to occupy distinct layers/scopes. They are **legitimately distinct — do not
merge**; this map records the distinction so it isn't re-litigated.

**Mood / emotion triad** — three orthogonal concerns:
- `src/persona/mood.c` — global mood persistence + decay (expression, SQLite-backed)
- `src/context/mood.c` — per-contact emotion-tag aggregation for the prompt (perception, analytic)
- `src/context/emotional_state.c` — online keyword classification of the current message (perception, per-turn)

**Humor pair** — different questions:
- `src/cognition/`-side humor (perception): which humor *type* fits (callback, misdirection, …)
- `src/persona/humor.c` (expression): *appropriateness* gating (serious vs light markers)

**Trust pipeline** — four stages, four layers (a pipeline, not duplication):
- `src/agent/channel_trust.c` — input classification: channel origin → trust tier
- `src/intelligence/trust.c` — learning: update global trust level from observations
- `src/behavior/` trust (`trust_prompt` / `behavior_trust`) — decision: calibrate the response action
- `src/memory/write_trust.c` — persistence: weight fact writes by source reliability

**Relationship cluster** — partly distinct, partly a true duplicate:
- `src/persona/relationship.c` — stage guidance (expression)
- `src/context/rel_dynamics.c` — rolling-metrics persistence (orthogonal)
- `src/agent/rel_dynamics.c` ≈ `src/agent/relationship_dynamics.c` — **the latter is a
  thin-wrapper clone** (identical velocity weights + mode logic). These belong
  in `behavior/` and should be deduplicated + relocated (tracked in
  `docs/plans/2026-05-29-ddd-bounded-contexts/` follow-on / the Modeled-Person
  normalization plan).

**Self vs other vs world** — distinct subjects:
- `src/agent/self_model.c` — the AGENT's behavioral self-observation
- `src/memory/personal_model.c` — the learned model of the USER (aggregate root)
- `src/intelligence/world_model.c` — environment/world state
