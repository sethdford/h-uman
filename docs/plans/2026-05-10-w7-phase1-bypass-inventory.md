---
title: "W7 Phase 1 — Direct graph bypass inventory"
created: 2026-05-10
status: active
parent: 2026-05-10-memory-v2-execution-plan.md
related:
  - 2026-05-10-w7-memory-facade.md
  - 2026-05-10-w7-type-collision-cleanup.md
---

# W7 Phase 1 — Direct `hu_graph_*` bypass inventory

**Purpose (execution plan §Phase 1 step 1.1):** enumerate remaining `hu_graph_*` touchpoints under `src/agent/`, `src/persona/`, and `src/feeds/` so migrations to `hu_memory_facade_read` / `hu_memory_facade_write` can be sequenced without guesswork.

**Regenerate counts:** from repo root,

```bash
bash scripts/w7-phase1-graph-bypass-inventory.sh
```

## Snapshot (2026-05-10, regenerated)

Per-file match counts for `hu_graph_` (includes type names like `hu_graph_t` and calls like `hu_graph_find_entity`). Regenerated with `bash scripts/w7-phase1-graph-bypass-inventory.sh`.

| Count | Path |
|------:|------|
| 23 | `src/agent/world_model.c` |
| 18 | `src/agent/autodream.c` |
| 10 | `src/agent/belief_reverify_runner.c` |
| 8 | `src/persona/persona_deltas.c` |
| 7 | `src/agent/cli.c` |
| 6 | `src/persona/delta_observer.c` |
| 6 | `src/agent/self_rag_atomic.c` |
| 6 | `src/agent/case_based.c` |
| 5 | `src/agent/world_model_bridge.c` |
| 5 | `src/agent/scheduler.c` |
| 5 | `src/agent/response_verifier.c` |
| 5 | `src/agent/anticipatory.c` |
| 4 | `src/agent/counterfactual.c` |
| 1 | `src/agent/autodream_runner.c` |
| **109** | **total** |

**Delta vs prior snapshot:** `src/agent/self_rag.c` and `src/agent/agent.c` now report **0** `hu_graph_*` matches (callers use `hu_memory_facade_t` / W7 bridge; graph access is indirect). `case_based.c` dropped 8→6; `self_rag_atomic.c` rose 2→6 as atomic path mirrors facade + `hu_graph__db_handle` patterns. Net total unchanged at 109 until remaining modules shed direct graph symbols.

`src/feeds/`: **0** matches (no direct graph API in `.c` / `.h` under that tree at this snapshot).

## Exception policy (Phase 1 exit until hot-path migration completes)

The execution plan allows **documented exceptions** instead of an immediate “&gt;80% line reduction” metric. Until each item is removed, the following patterns are **explicitly allowed**:

1. **`hu_memory_facade_graph_handle(m)` → `hu_graph_*`**  
   Runners and W9 code that already take `hu_memory_facade_t *m` but call the graph for operations not yet exposed on the facade query/record API (e.g. specialized SQL, temporal queries). Owner: W7/W9. Removal: as `hu_memory_facade_read` / `write` gains equivalent kinds.

2. **`hu_graph__db_handle` for SQLite DDL / batch maintenance**  
   `autodream.c`, `case_based.c`, `response_verifier.c` use the internal DB handle for tables not yet routed through facade writes (see inline `TODO(W7)` in `autodream.c`). Owner: W7 + W2 consolidation. Removal: facade write + backend support for quarantine promotion / case tables.

3. **Persona delta producers** (`persona_deltas.c`, `delta_observer.c`)  
   Still graph-adjacent for W5 delta persistence; migration is a dedicated pass once facade `HU_MEM_PERSONA_DELTA` paths cover all writer/reader shapes. Owner: persona + W7.

4. **`hu_persona_load` at agent init** (`agent.c`)  
   Documented in-source: no `hu_memory_facade_t *` at init time; deferred to W9 single-load. Owner: agent init + W9.

## Recommended migration order (next PRs)

1. **response_verifier** + **case_based** — **partial:** public APIs take `hu_memory_facade_t *`; remaining matches are `hu_graph__db_handle` / helpers until facade kinds cover case + verifier tables. Next: route writes through `hu_memory_facade_write` and drop raw SQL touchpoints.
2. **self_rag_atomic** — align with facade read/write kinds for claim scoring so `hu_graph_*` count can fall without duplicating verifier SQL.
3. **anticipatory** — replace temporal/causal reads with `hu_memory_facade_read` queries once neighbor/window kinds cover the call shape.
4. **autodream** — largest raw-SQL + `hu_graph_upsert_relation_ex` bypass; depends on quarantine write semantics on the facade.
5. **world_model.c** — keep in sync with W9 spec; many calls are already behind facade for reads; remainder tracked per function.

## Proof obligations

- After each migration slice: `./build/human_tests --suite=W7` (or subsystem suite named in the PR) + no new dual-include violations (`bash scripts/check-memory-v2-header-collision.sh`).
- Full gate before merge: `./build/human_tests` (G1).
