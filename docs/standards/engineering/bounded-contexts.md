---
title: Bounded Contexts
---

# Bounded Contexts

h-uman's `src/` directories are feature-folders; the real domain boundaries are
fewer. This is the canonical map. The full audit, rationale, and refactor
roadmap live in
[docs/plans/2026-05-29-ddd-bounded-contexts/README.md](../../plans/2026-05-29-ddd-bounded-contexts/README.md).

**Cross-references:** [principles.md](principles.md), [anti-patterns.md](anti-patterns.md), [../security/data-privacy.md](../security/data-privacy.md)

---

## Modeled Person — one context, three layers

```
persona/    expression: identity, voice, style, boundaries
cognition/  perception: emotion, trust, attachment, presence
behavior/   decision:   relational acts, intensity, modulation
```

These three directories are **one bounded context**, not three. They share two
aggregate roots — `hu_persona_t` (expression state) and `hu_personal_model_t`
(learned state, owned by `memory/`) — and have near-zero cross-includes. The
per-turn flow is unidirectional: **persona → cognition → behavior**.

Rule: do **not** add cross-includes between `cognition/` and `behavior/`.
Communicate through the aggregate roots. New "person" state belongs to one of
the two roots, not a fourth scattered struct. Enforced by
`scripts/check-modeled-person-layering.sh` (see
[`.claude/rules/modeled-person-layering.md`](../../../.claude/rules/modeled-person-layering.md)).
The canonical shape for modules in these three layers — pure predicate, thin
wire, repository-backed persistence, config gate, layer placement — is
[modeled-person-module-shape.md](modeled-person-module-shape.md), which also
carries the verified context map (which look-alike modules are legitimately
distinct vs the one true duplicate).

## Recall (Memory)

`memory/` is a bounded context behind the `hu_memory_t` vtable. Domain code
**never grabs the raw `sqlite3*` handle** (`hu_sqlite_memory_get_db()` /
`hu_memory_facade_sqlite_db()`); it reaches persistence through a memory
**repository** (`include/human/memory/<aggregate>_repo.h`). SQLite is legal only
in `src/memory/engines/` and `src/memory/repos/`. Enforced by
`scripts/check-sqlite-includer-ratchet.sh`. This is what keeps a non-SQL backend
(and the "runs anywhere / privacy-by-architecture" moat) possible.

## Conversation Orchestration

`agent/` + `daemon*.c`. The orchestration core. It must not know concrete
providers (inject the `hu_provider_t` vtable) or branch on channel identity by
string — channel knowledge lives in the Channels context behind
`hu_channel_behavior_class_for_name()`. Enforced by
`scripts/check-agent-core-boundary.sh`.

## Model Access (edge anti-corruption layer)

`providers/`, `channels/`, `tools/` depend on their vtable contracts and shared
infra — **never on each other**. This is the codebase's cleanest hexagonal
boundary; protect it. Enforced by `scripts/check-edge-context-isolation.sh`.

## Evaluation vs Benchmarking — distinct, do not merge

- `eval/` (+ `eval.c`) — the **runtime** task-runner, live in the agent loop
  (consistency scoring, shape classification). 256 includers.
- `evaluation/` (+ `cli_evaluation.c`) — the **offline** W16 benchmark suite,
  CI-only. 15 includers.

These are different generations with different consumers, bridged via
`evaluation_legacy_bridge.c`. Keep them separate. (The shared word "eval" is a
known ubiquitous-language hazard; a future rename of the runtime one to
`scoring/` is tracked in the Phase-0 plan appendix.)

## Supporting subdomains

- **Configuration** (`config_*.c`, `hu_config_t`) — a data-transfer object.
  Agent code should accept a narrow `hu_agent_app_config_t` projection, not the
  65-substruct god-DTO.
- **Learning Loop** (`intelligence/` + `reflection/`) — the reflect→improve
  closed loop, separate from per-turn execution.
- **Knowledge Resources** (`data/`) — static blobs/loaders; an input source to
  memory ingest, not a persistence layer.
