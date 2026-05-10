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

## Snapshot (2026-05-10, post case / verifier / atomic facade slice)

Per-file match counts for `hu_graph_` (includes type names like `hu_graph_t` and calls like `hu_graph_find_entity`). Regenerated with `bash scripts/w7-phase1-graph-bypass-inventory.sh`.

| Count | Path |
|------:|------|
| 23 | `src/agent/world_model.c` |
| 18 | `src/agent/autodream.c` |
| 10 | `src/agent/belief_reverify_runner.c` |
| 8 | `src/persona/persona_deltas.c` |
| 7 | `src/agent/cli.c` |
| 6 | `src/persona/delta_observer.c` |
| 6 | `src/agent/scheduler.c` |
| 6 | `src/agent/anticipatory.c` |
| 5 | `src/agent/world_model_bridge.c` |
| 5 | `src/agent/self_rag_atomic.c` |
| 4 | `src/agent/counterfactual.c` |
| 4 | `src/agent/case_based.c` |
| 3 | `src/agent/response_verifier.c` |
| 1 | `src/agent/retrieval_planner_llm.c` |
| 1 | `src/agent/autodream_runner.c` |
| 1 | `src/agent/agent.c` |
| **108** | **total** |

**Delta vs prior 109-row snapshot:** **`HU_MEM_CASE`** backend + **`hu_case_record` / `hu_case_recall`** cut raw graph/SQL in `case_based.c` (6→4; remaining hits are type names / includes). **Verifier** now scores via **`hu_memory_facade_read`** + **`HU_MEMORY_REL_VERIFIER_SCAN`** (5→3; mostly **`hu_graph_relation_t`** in receipts). **Atomic self-RAG** correction path uses facade relation list (6→5). **`self_rag.c`** remains at **0**. Net **109 → 108** until the next slice (anticipatory, autodream, belief runner, …).

`src/feeds/`: **0** matches (no direct graph API in `.c` / `.h` under that tree at this snapshot).

## Exception policy (Phase 1 exit until hot-path migration completes)

The execution plan allows **documented exceptions** instead of an immediate “&gt;80% line reduction” metric. Until each item is removed, the following patterns are **explicitly allowed**:

1. **`hu_memory_facade_graph_handle(m)` → `hu_graph_*`**  
   Runners and W9 code that already take `hu_memory_facade_t *m` but call the graph for operations not yet exposed on the facade query/record API (e.g. specialized SQL, temporal queries). Owner: W7/W9. Removal: as `hu_memory_facade_read` / `write` gains equivalent kinds.

2. **`hu_graph__db_handle` for SQLite DDL / batch maintenance**  
   **`autodream.c`** (and similar) still use the internal DB handle where quarantine / batch SQL is not on the facade yet (see inline `TODO(W7)` in `autodream.c`). **`case_based.c`** / **`response_verifier.c`** no longer open the DB directly for their primary paths. Owner: W7 + W2 consolidation. Removal: facade write + backend support for quarantine promotion and remaining raw SQL.

3. **Persona delta producers** (`persona_deltas.c`, `delta_observer.c`)  
   Still graph-adjacent for W5 delta persistence; migration is a dedicated pass once facade `HU_MEM_PERSONA_DELTA` paths cover all writer/reader shapes. Owner: persona + W7.

4. **`hu_persona_load` at agent init** (`agent.c`)  
   Documented in-source: no `hu_memory_facade_t *` at init time; deferred to W9 single-load. Owner: agent init + W9.

## Recommended migration order (next PRs)

1. **anticipatory** — replace temporal/causal reads with `hu_memory_facade_read` once neighbor/window (or new) kinds cover the call shape.
2. **autodream** — largest raw-SQL + `hu_graph_upsert_relation_ex` bypass; depends on quarantine write semantics on the facade.
3. **belief_reverify_runner** — narrow graph surface via facade reads/writes where relation belief updates have a stable kind.
4. **world_model.c** / **world_model_bridge** — keep in sync with W9; migrate stragglers as `hu_world_model_load` + invalidation contract stabilize.
5. **self_rag_atomic** / **response_verifier** — remaining `hu_graph_*` hits are mostly **`hu_graph_relation_t`** types for payloads/receipts; optional follow-up: typedef aliases in a facade-only header to drive the inventory script toward “calls only” if desired.

**Done in-tree (reference):** `HU_MEM_CASE` + **`hu_case_*`** on the facade; verifier scoring via **`HU_MEMORY_REL_VERIFIER_SCAN`** + **`hu_graph_list_relations_verifier_scan`**; atomic correction path uses **`hu_memory_facade_read`** (`HU_MEM_RELATION` list).

## Proof obligations

- After each migration slice: `./build/human_tests --suite=W7` (or subsystem suite named in the PR) + no new dual-include violations (`bash scripts/check-memory-v2-header-collision.sh`).
- Full gate before merge: `./build/human_tests` (G1).
