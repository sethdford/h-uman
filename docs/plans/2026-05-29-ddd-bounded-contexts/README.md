# DDD Bounded-Context Rationalization — Roadmap Index

> **Status:** Plan / not started. Authored 2026-05-29 from a 5-agent DDD audit
> of the codebase. This directory is the canonical source of truth for the
> bounded-context refactor; each `phase-N-*.md` is an independently-executable
> implementation plan.

**Goal:** Convert h-uman's 41 feature-folders into ~10 well-defined bounded
contexts with real boundaries, fixing the clean-architecture violations that
block the product's "runs anywhere / privacy-by-architecture" moat.

---

## The three systemic findings (verified)

Directory sprawl is a symptom. These are the diseases:

| # | Thread | Diagnosis | Blast radius (verified) |
|---|---|---|---|
| **T1** | SQLite is ambient, not abstracted | `hu_memory_t` vtable is a read/write dispatcher; domain code reaches past it to raw SQL | **113 files** `#include <sqlite3.h>` across **15** dirs; **33** call `hu_sqlite_memory_get_db()` across **11** dirs |
| **T2** | Orchestration is a god-context | `daemon.c` = 14,750 LOC / 6 jobs; `agent/` = 154 flat files / ~8 sub-domains | ~330 files |
| **T3** | Boundaries leak in practice | agent core knows channels by string `memcmp`; instantiates providers via factory include | `agent_turn.c:382-405` (7 memcmp) + 5 factory-include sites |

### What is NOT broken (do not "fix" — verified clean)

- `eval/` (runtime, 256 includes, live in agent loop) vs `evaluation/` (W16 offline benchmark, 15 includes, CI-only) are **legitimately distinct generations**, bridged via `evaluation_legacy_bridge.c`. Only the *naming* is a hazard.
- `provider/` (shared schema) vs `providers/` (vtable contracts) is an **intentional** split.
- `hu_config_t` is a **DTO** (correct dependency direction), not a domain leak — but 73 includers is a coupling risk.
- The 8-dir "humanness" cluster has **near-zero cross-coupling** — over-partitioned, not tangled. Two real aggregate roots: `hu_persona_t`, `hu_personal_model_t`.

---

## Target bounded-context map

| Target context | Absorbs (current) | Aggregate root | Phase |
|---|---|---|---|
| Conversation Orchestration | `agent/`, `daemon*.c`, `bus.c`, routing | turn lifecycle / `hu_agent_t` | 1, 2, 4 |
| Modeled Person | `persona/` + `cognition/` + `behavior/` | `hu_persona_t` | 0 (doc) |
| Learning Loop | `intelligence/` + `reflection/` | closed loop | 4 |
| Recall (Memory) | `memory/` (117) | `hu_memory_t` + `hu_personal_model_t` | 3 |
| Knowledge Resources | `data/` (54) | dataset/blob loaders | 0 (rename) |
| Evaluation (runtime) | `eval/` + `eval.c` | `hu_eval_suite_t` | 0 (rename) |
| Benchmarking (offline) | `evaluation/` + `cli_evaluation.c` | `hu_evaluation_t` | — keep |
| Calibration | `calibration/` | `hu_calibration_*` | — keep |
| Model Access (edge ACL) | `providers/` `channels/` `tools/` | the 3 vtables | 0 (guard) |
| Configuration (supporting) | `config_*.c` | `hu_config_t` | 4 (facade) |

---

## Phases (execution order)

Ordered so each phase makes the next **safer**, not by raw severity. Cheap,
reversible boundary-clarifying moves first; the high-blast-radius memory
re-layer last, after the daemon is small enough to make it tractable.

| Phase | Plan | Risk | Effort | Unblocks | One-line |
|---|---|---|---|---|---|
| **0** | [phase-0-erosion-guards-and-renames.md](phase-0-erosion-guards-and-renames.md) | Low | Days | All | Path-scoped erosion-guard rules + ubiquitous-language renames + Modeled-Person doc |
| **1** | [phase-1-channel-behavior-class.md](phase-1-channel-behavior-class.md) | Low-Med | 1-2 wk | M6 | Add `behavior_class` to channel vtable; delete the 7 hardcoded `memcmp` |
| **2** | [phase-2-daemon-split.md](phase-2-daemon-split.md) | Med | 2-4 wk | M4 | Carve 14,750-LOC `daemon.c` into 5 single-responsibility files |
| **3** | [phase-3-memory-query-interface.md](phase-3-memory-query-interface.md) | High | 4-8 wk | **M2/M3/moat** | `hu_memory_query_t` interface; migrate 33 raw-SQL sites; domain/store split |
| **4** | [phase-4-repackaging.md](phase-4-repackaging.md) | Low-Med | Ongoing | maintainability | `agent/` sub-package dirs + `hu_config_t` facade |

## Why this is the SOTA path

The product thesis stakes the moat on *privacy-by-architecture* and *runs
anywhere* (M2 personal model, M3 on-device learning). **T1 structurally
contradicts that thesis**: the personal model — the moat itself — is welded
to SQLite via 113 files. A Redis-only or pure-on-device target cannot run
without this refactor. Phase 3 is therefore the SOTA-critical move; Phases
0-2 exist to make Phase 3 survivable. **The bounded-context work *is* the
moat work.**

## Execution

Each phase plan uses the `superpowers:writing-plans` format (TDD, bite-sized
steps, exact paths). Recommended: execute via `superpowers:subagent-driven-development`
(fresh subagent per task, review between). Build/verify against the **production
binary** per `.claude/rules/cmake-build-stale-binary.md` (`touch` source before
rebuild), and run the **full** `./build/human_tests` suite before each commit
per `.claude/rules/quality-gates.md`.
