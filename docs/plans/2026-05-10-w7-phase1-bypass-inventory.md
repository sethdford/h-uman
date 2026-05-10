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

## Snapshot (2026-05-10, post AutoDream `ad_sqlite` + facade DB threading)

Per-file match counts for `hu_graph_` (includes type names like `hu_graph_t` and calls like `hu_graph_find_entity`). Regenerated with `bash scripts/w7-phase1-graph-bypass-inventory.sh`.

| Count | Path |
|------:|------|
| 18 | `src/agent/world_model.c` |
| 14 | `src/agent/autodream.c` |
| 7 | `src/agent/world_model_bridge.c` |
| 7 | `src/agent/self_rag_atomic.c` |
| 7 | `src/agent/cli.c` |
| 6 | `src/persona/persona_deltas.c` |
| 6 | `src/persona/delta_observer.c` |
| 5 | `src/agent/belief_reverify_runner.c` |
| 4 | `src/agent/anticipatory.c` |
| 3 | `src/agent/response_verifier.c` |
| 1 | `src/agent/scheduler.c` |
| 1 | `src/agent/retrieval_planner_llm.c` |
| 1 | `src/agent/autodream_runner.c` |
| 1 | `src/agent/agent.c` |
| **81** | **total** |

**Recent deltas:** **AutoDream** — `hu_autodream_run_on_facade` now resolves SQLite via **`hu_memory_facade_sqlite_db`** when available (`ad_sqlite`); phases take an explicit `sqlite3 *` so DDL / community summaries / edge reweight share that handle. **`hu_graph_sqlite_connection`** remains only as the graph-only fallback inside `ad_sqlite` and for **`hu_autodream_summarize_community` / `hu_autodream_read_community_summary`** (public graph-only entrypoints). **Anticipatory** — temporal/causal/entity via **`hu_memory_facade_query_*`** + **`hu_memory_facade_read`** (entity BY_NAME); **`hu_anticipatory_analyze`** opens a short-lived facade. **Belief re-verify** — **`hu_memory_facade_get_relation_belief`** / **`set_relation_belief`**. **World-model bridge** — self-RAG abstention records negatives with **`hu_negative_memory_add_facade`** (W9 API on `hu_memory_facade_t *`); post-verify relation belief loop no longer uses a throwaway **`hu_memory_facade_graph_handle`** guard. Remaining **`world_model_bridge.c`** hits are **`hu_w7_facade_open(hu_graph_t *)`** and payload casts (**`hu_graph_entity_t`** / **`hu_graph_relation_t`**). **`counterfactual.c`** / **`case_based.c`** have zero matches in this inventory.

`src/feeds/`: **0** matches (no direct graph API in `.c` / `.h` under that tree at this snapshot).

## Exception policy (Phase 1 exit until hot-path migration completes)

The execution plan allows **documented exceptions** instead of an immediate “&gt;80% line reduction” metric. Until each item is removed, the following patterns are **explicitly allowed**:

1. **`hu_memory_facade_graph_handle(m)` → `hu_graph_*`**  
   Runners and W9 code that already take `hu_memory_facade_t *m` but call the graph for operations not yet exposed on the facade query/record API (e.g. specialized SQL, world-model rebuild). Owner: W7/W9. Removal: as `hu_memory_facade_read` / `write` (or thin **`hu_memory_facade_*`** delegates like temporal/causal/relation-belief) cover each call shape.

2. **`hu_graph_sqlite_connection`** for SQLite DDL / batch maintenance  
   Public accessor in `human/memory/graph.h` when `HU_ENABLE_SQLITE`; the internal `hu_graph__db_handle` in `src/memory/graph.c` is not part of the public contract. **`autodream.c`** uses **`hu_memory_facade_sqlite_db`** when a facade is supplied (`ad_sqlite`); graph-only callers still hit **`hu_graph_sqlite_connection`** via that helper or the public summarize/read APIs that only take **`hu_graph_t *`**. **`case_based.c`** / **`response_verifier.c`** no longer open the DB directly for their primary paths. Owner: W7 + W2 consolidation. Removal: optional facade overloads for summarize/read, or keep graph-only public APIs and accept inventory noise from type names.

3. **Persona delta producers** (`persona_deltas.c`, `delta_observer.c`)  
   Still graph-adjacent for W5 delta persistence; migration is a dedicated pass once facade `HU_MEM_PERSONA_DELTA` paths cover all writer/reader shapes. Owner: persona + W7.

4. **`hu_persona_load` at agent init** (`agent.c`)  
   Documented in-source: no `hu_memory_facade_t *` at init time; deferred to W9 single-load. Owner: agent init + W9.

## Recommended migration order (next PRs)

1. ~~**anticipatory**~~ — **Done:** **`hu_memory_facade_query_temporal` / `query_causal`**, entity **`hu_memory_facade_read`** (BY_NAME), graph **`hu_anticipatory_analyze`** delegates via short-lived `hu_memory_facade_open`.
2. **autodream** — **partial:** facade runs use **`hu_memory_facade_sqlite_db`** for all in-cycle DDL / raw SQL (quarantine scan, community summaries, edge reweight, run log). **Quarantine release** uses **`hu_memory_facade_write`** (`HU_MEM_RELATION`) on the facade path; **`hu_autodream_run(graph-only)`** still calls **`hu_graph_upsert_relation_with_belief`** for release. Public **`hu_autodream_summarize_community`** / **`hu_autodream_read_community_summary`** remain graph-pointer APIs. Remaining inventory hits are mostly **`hu_graph_t`** parameters, **`hu_graph_relation_t`**, and the graph-only upsert branch.
3. ~~**belief_reverify_runner**~~ — **Done for graph calls:** relation list via **`hu_memory_facade_read`**; belief round-trip via **`hu_memory_facade_get_relation_belief`** / **`set_relation_belief`**. Remaining inventory hits are payload types only.
4. **world_model.c** / **world_model_bridge** — `hu_world_model_build` now lists negative memory via **`hu_negative_memory_list_facade`** and reads residue / goals SQLite through **`hu_memory_facade_sqlite_db`** (same DB as the graph). Graph-only **`hu_negative_memory_list(g, …)`** remains for callers without a facade handle. Further work: model residue/goals as facade kinds or thin delegates if needed.
5. **self_rag_atomic** / **response_verifier** — remaining `hu_graph_*` hits are mostly **`hu_graph_relation_t`** types for payloads/receipts; optional follow-up: typedef aliases in a facade-only header to drive the inventory script toward “calls only” if desired.
6. **`src/agent/cli.c`** — next high-count agent surface after autodream / world_model.

**Done in-tree (reference):** `HU_MEM_CASE` + **`hu_case_*`** on the facade; verifier scoring via **`HU_MEMORY_REL_VERIFIER_SCAN`** + **`hu_graph_list_relations_verifier_scan`**; atomic correction path uses **`hu_memory_facade_read`** (`HU_MEM_RELATION` list); anticipatory + belief re-verify paths above.

## Proof obligations

- After each migration slice: `./build/human_tests --suite=W7` (or subsystem suite named in the PR) + no new dual-include violations (`bash scripts/check-memory-v2-header-collision.sh`).
- Full gate before merge: `./build/human_tests` (G1).
