---
title: "Memory + Graphs Roadmap — Overview"
created: 2026-05-10
status: closed
parent: 2026-03-08-better-than-human.md
related:
  - 2026-03-10-human-fidelity-phase3-superhuman-memory.md
  - 2026-03-10-human-fidelity-phase7-deep-memory.md
  - 2026-03-21-elastic-memory-episodic.md
last_audit: 2026-05-25
---

# Memory + Graphs Roadmap — Overview

## Why this exists

`docs/plans/2026-03-08-better-than-human.md` is marked complete. Phase 3 superhuman memory and Phase 7 deep memory landed too. The graph already has Leiden community detection, temporal events, causal links, conflict detection, and Ebbinghaus retention scoring (`include/human/memory/graph.h`). h-uman has shipped most of the building blocks the field is still talking about.

What's left is the delta between *rich memory* and *trustworthy memory*. Five 2026 SOTA primitives (Zep/Graphiti, Mem0, MAGMA, Cognee `memify`, Claude Code AutoDream) point at the same gaps:

1. **Bitemporal correctness** — separate "when was this true" from "when did we notice." Graph today only tracks ingest time (`first_seen`/`last_seen`).
2. **Conflict resolution at write time** — `hu_graph_detect_conflict`/`hu_graph_reconsolidate` exist (`src/memory/graph.c`) but are only invoked from the daemon's periodic job, not on every fact write.
3. **Background self-refinement** — `consolidation_engine.c` runs from `daemon.c` and `main.c`, but it's a one-shot dedupe, not the AutoDream / Cognee `memify` pattern of nightly prune + reweight + derive + summarize.
4. **Multi-graph cross-traversal** — entity/emotional/contact subgraphs exist in parallel. They aren't first-class peers with cross-edges.
5. **Self-RAG inline + provenance + agent-writable persona + eval** — verification, GDPR receipts, persona deltas, LoCoMo/LongMemEval scoring.

Memory poisoning defense (per Unit 42 / A-MemGuard 2025) is also missing as a distinct write-time control.

## The 6 workstreams

Each is its own branch and PR. Each leaves the system better than before. Order is by dependency, not by ease — W1 must land before W2 and W3 because both consume bitemporal edges. W6's eval harness is sequenced last so every prior workstream gets a quantified lift score.

| # | Branch | Spec | Subprojects bundled |
|---|--------|------|---------------------|
| W1 | `feat/memory-w1-bitemporal-foundation` | `2026-05-10-w1-bitemporal-foundation.md` | Bitemporal edges + write-time conflict resolver + write-trust score + LoCoMo skeleton |
| W2 | `feat/memory-w2-background-consolidation` | `2026-05-10-w2-background-consolidation.md` | AutoDream consolidator subagent + community summaries + life-chapter summarization |
| W3 | `feat/memory-w3-multi-graph-topology` | `2026-05-10-w3-multi-graph-topology.md` | Promote episodic/emotional/concept to peer graphs; cross-graph traversal; case-based planning |
| W4 | `feat/memory-w4-self-rag-provenance` | `2026-05-10-w4-self-rag-provenance.md` | Self-RAG inline verification + provenance receipts + GDPR memory-view UI |
| W5 | `feat/memory-w5-agent-writable-persona` | `2026-05-10-w5-agent-writable-persona.md` | Persona overlay deltas + procedural memory deltas + persona-evolver subagent |
| W6 | `feat/memory-w6-eval-memrl-redteam` | `2026-05-10-w6-eval-memrl-redteam.md` | LoCoMo + LongMemEval harness + memory-poisoning red-team + MemRL write rewards |
| Sk | `docs/skills-pack-memory-craft` | `2026-05-10-skills-pack-memory-craft.md` | 12 new skill-registry entries (memory craft + epistemic hygiene) |

The skills pack is a parallel docs-only branch that can land any time after W1.

## Cross-cutting principles

Applies to every workstream:

- **One concern per branch.** No mixed feature + refactor + infra. (`AGENTS.md` §6)
- **Vtable discipline.** No new vtables unless absolutely required. W2 community summaries adds one (`hu_summary_writer_t`).
- **`HU_IS_TEST` guards on side effects.** Subagents must be scriptable in tests without spawning real processes.
- **Binary size budget.** ~1750 KB MinSizeRel+LTO is the line. AutoDream ML-shaped helpers gated behind `HU_ENABLE_AUTODREAM`. MemRL gated behind existing `HU_ENABLE_ML`.
- **Zero ASan errors.** Every allocation freed; `SQLITE_STATIC` only.
- **Conventional commits.** Pre-commit hooks already enforce.
- **Test discipline.** Each workstream lands ≥1 boundary/failure-mode test per new public function. Schema migrations get round-trip tests.
- **GDPR / EU AI Act readiness.** W4 makes erasure user-facing; W6 makes audit trails queryable. Both target Aug 2026 EU AI Act applicability date.

## Schema migration policy

W1 introduces the first schema change. Establishes the pattern for W2/W3:

- New columns are added with `ALTER TABLE … ADD COLUMN … DEFAULT NULL` (SQLite-safe)
- Old rows get a synthetic `event_start = first_seen`, `event_end = NULL` ("still true unless superseded") on first read after migration
- Migration is idempotent and reversible (down-migration script lives next to up-migration)
- Schema version is bumped; old binaries refuse to open new DBs (read `schema_version` row, compare, error out)

## Success metrics (rolled up)

| Workstream | Primary signal | Threshold |
|-----------|----------------|-----------|
| W1 | LoCoMo temporal-Q subset | +5 pts vs current baseline |
| W2 | Recall accuracy on 30-day-old facts | maintained while DB row count stays bounded |
| W3 | LoCoMo multi-hop subset | +5 pts vs current baseline |
| W4 | Hallucination rate on factual claims (eval suite) | −50% vs baseline; provenance attached to every claim |
| W5 | Persona drift toward user (A/B blind eval) | measurable preference for persona-evolved variant |
| W6 | LoCoMo overall + MINJA-style attack success | LoCoMo published + attack rate <10% |
| Skills | Skill-registry coverage | 12 new skills live, `human-skills` index regenerated |

## Out of scope (explicit non-goals)

To keep each workstream small enough to ship in a single sprint:

- **Cross-user federated memory.** Each h-uman is local-first. W5's twin shared-memory tier adds household-scope, not cloud-scope.
- **Replacing existing engines.** SQLite stays the default; pgvector/qdrant integrations remain optional.
- **A new memory vtable.** All work extends `hu_memory_t` and `hu_graph_t` rather than defining new vtables.
- **A new agent loop.** All subagents (W2 AutoDream, W5 persona-evolver) plug into existing `src/agent/spawn.c` + `src/agent/dispatcher.c`.
- **GUI work beyond W4's memory-view.** Marketing site, native apps, and web dashboard get the W4 view; everything else stays cosmetic.
- **Cross-channel summarization beyond W2.** Per-channel summaries are W2; cross-channel narrative stitching is a future workstream (out of scope).

## Sequencing rules

- W1 must land before W2 and W3.
- W2 and W3 can run in parallel after W1, but should land sequentially in the same week to limit graph-schema churn.
- W4 depends on W1 (provenance receipts read bitemporal data) but is otherwise independent.
- W5 depends on W4 (persona-evolver writes a memory entry that needs provenance).
- W6 must land last so every prior workstream's lift can be measured.
- Skills pack ships any time after W1's branch is open (skills can reference W1 schema).

## Risks (rolled up)

| Risk | Mitigation |
|------|------------|
| Schema migration corrupts existing user DBs | Idempotent + reversible + round-trip tests + `schema_version` gate |
| AutoDream eats CPU on user devices | Idle-only schedule; binary-size and CPU budget tests; `HU_ENABLE_AUTODREAM` |
| Persona deltas drift agent away from user voice | W5 every delta is gated by user confirm; revert UX in same commit as write UX |
| LoCoMo eval requires API budget | W6 ships with both ADC-backed and offline judge variants |
| Multi-graph cross-edges create traversal blowup | W3 caps cross-graph depth; explicit budget per query |

## Reference index

- `include/human/memory/graph.h` — current graph API (entities, relations, communities, temporal, causal, conflict, retention)
- `src/memory/graph.c` — SQLite-backed implementation (1770+ lines)
- `src/memory/consolidation_engine.c` — current background consolidation
- `src/memory/personal_model.c` — unified user model scaffold (M2 mission anchor)
- `docs/plans/2026-03-08-better-than-human.md` — completed; this roadmap is the next phase
- Canvas (parallel, not committed): `~/.cursor/projects/.../canvases/memory-graph-roadmap.canvas.tsx`
