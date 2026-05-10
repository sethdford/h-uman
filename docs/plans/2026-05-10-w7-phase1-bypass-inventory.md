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

## Snapshot (2026-05-10, post anticipatory + belief-reverify facade slice)

Per-file match counts for `hu_graph_` (includes type names like `hu_graph_t` and calls like `hu_graph_find_entity`). Regenerated with `bash scripts/w7-phase1-graph-bypass-inventory.sh`.

| Count | Path |
|------:|------|
| 23 | `src/agent/world_model.c` |
| 18 | `src/agent/autodream.c` |
| 8 | `src/persona/persona_deltas.c` |
| 7 | `src/agent/cli.c` |
| 6 | `src/persona/delta_observer.c` |
| 6 | `src/agent/world_model_bridge.c` |
| 5 | `src/agent/self_rag_atomic.c` |
| 5 | `src/agent/belief_reverify_runner.c` |
| 4 | `src/agent/anticipatory.c` |
| 3 | `src/agent/response_verifier.c` |
| 2 | `src/agent/scheduler.c` |
| 1 | `src/agent/retrieval_planner_llm.c` |
| 1 | `src/agent/autodream_runner.c` |
| 1 | `src/agent/agent.c` |
| **90** | **total** |

**Recent deltas:** **Anticipatory** — temporal/causal/entity lookup go through **`hu_memory_facade_query_*`**, **`hu_memory_facade_read`** (entity BY_NAME), and graph-only **`hu_anticipatory_analyze`** opens a short-lived facade. **Belief re-verify runner** — list path was already **`hu_memory_facade_read`** (`HU_MEM_RELATION`); belief read/write now use **`hu_memory_facade_get_relation_belief`** / **`hu_memory_facade_set_relation_belief`** (thin graph delegates). Remaining **`belief_reverify_runner.c`** hits are **`hu_graph_relation_t`** payload casts plus a comment referencing v1 list ordering. **`counterfactual.c`** / **`case_based.c`** no longer appear in this inventory (zero `hu_graph_` substring matches under the scanned trees at this snapshot).

`src/feeds/`: **0** matches (no direct graph API in `.c` / `.h` under that tree at this snapshot).

## Exception policy (Phase 1 exit until hot-path migration completes)

The execution plan allows **documented exceptions** instead of an immediate “&gt;80% line reduction” metric. Until each item is removed, the following patterns are **explicitly allowed**:

1. **`hu_memory_facade_graph_handle(m)` → `hu_graph_*`**  
   Runners and W9 code that already take `hu_memory_facade_t *m` but call the graph for operations not yet exposed on the facade query/record API (e.g. specialized SQL, world-model rebuild). Owner: W7/W9. Removal: as `hu_memory_facade_read` / `write` (or thin **`hu_memory_facade_*`** delegates like temporal/causal/relation-belief) cover each call shape.

2. **`hu_graph__db_handle` for SQLite DDL / batch maintenance**  
   **`autodream.c`** (and similar) still use the internal DB handle where quarantine / batch SQL is not on the facade yet (see inline `TODO(W7)` in `autodream.c`). **`case_based.c`** / **`response_verifier.c`** no longer open the DB directly for their primary paths. Owner: W7 + W2 consolidation. Removal: facade write + backend support for quarantine promotion and remaining raw SQL.

3. **Persona delta producers** (`persona_deltas.c`, `delta_observer.c`)  
   Still graph-adjacent for W5 delta persistence; migration is a dedicated pass once facade `HU_MEM_PERSONA_DELTA` paths cover all writer/reader shapes. Owner: persona + W7.

4. **`hu_persona_load` at agent init** (`agent.c`)  
   Documented in-source: no `hu_memory_facade_t *` at init time; deferred to W9 single-load. Owner: agent init + W9.

## Recommended migration order (next PRs)

1. ~~**anticipatory**~~ — **Done:** **`hu_memory_facade_query_temporal` / `query_causal`**, entity **`hu_memory_facade_read`** (BY_NAME), graph **`hu_anticipatory_analyze`** delegates via short-lived `hu_memory_facade_open`.
2. **autodream** — largest raw-SQL + `hu_graph__db_handle` / `hu_graph_upsert_relation_with_belief` bypass; depends on quarantine + batch SQL on the facade (or documented thin delegates).
3. ~~**belief_reverify_runner**~~ — **Done for graph calls:** relation list via **`hu_memory_facade_read`**; belief round-trip via **`hu_memory_facade_get_relation_belief`** / **`set_relation_belief`**. Remaining inventory hits are payload types only.
4. **world_model.c** / **world_model_bridge** — keep in sync with W9; migrate stragglers as `hu_world_model_load` + invalidation contract stabilize.
5. **self_rag_atomic** / **response_verifier** — remaining `hu_graph_*` hits are mostly **`hu_graph_relation_t`** types for payloads/receipts; optional follow-up: typedef aliases in a facade-only header to drive the inventory script toward “calls only” if desired.
6. **`src/agent/cli.c`** — next high-count agent surface after autodream / world_model.

**Done in-tree (reference):** `HU_MEM_CASE` + **`hu_case_*`** on the facade; verifier scoring via **`HU_MEMORY_REL_VERIFIER_SCAN`** + **`hu_graph_list_relations_verifier_scan`**; atomic correction path uses **`hu_memory_facade_read`** (`HU_MEM_RELATION` list); anticipatory + belief re-verify paths above.

## Proof obligations

- After each migration slice: `./build/human_tests --suite=W7` (or subsystem suite named in the PR) + no new dual-include violations (`bash scripts/check-memory-v2-header-collision.sh`).
- Full gate before merge: `./build/human_tests` (G1).
