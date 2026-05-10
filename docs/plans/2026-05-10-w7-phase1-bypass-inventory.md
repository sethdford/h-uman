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

## Snapshot (2026-05-10, post facade row typedefs + `hu_memory_facade_free_listed_entities`)

Per-file match counts for `hu_graph_` substring (includes type names like `hu_graph_t` and calls like `hu_graph_find_entity`). **`human/memory/memory.h`** now documents W7-first aliases (**`hu_memory_entity_row_t`**, **`hu_memory_relation_row_t`**) and a list-entity free helper — those symbols do **not** appear in this `src/agent` / `src/persona` scan. Regenerate: `bash scripts/w7-phase1-graph-bypass-inventory.sh`.

| Count | Path |
|------:|------|
| 13 | `src/agent/autodream.c` |
| 7 | `src/agent/cli.c` |
| 6 | `src/persona/persona_deltas.c` |
| 6 | `src/persona/delta_observer.c` |
| 4 | `src/agent/world_model.c` |
| 1 | `src/agent/world_model_bridge.c` |
| 1 | `src/agent/anticipatory.c` |
| 1 | `src/agent/agent.c` |
| **39** | **total** |

**Recent deltas:** Agent/persona call sites that only needed v1 row **payload shapes** or **entity list teardown** now use **`hu_memory_*`** typedefs / **`hu_memory_facade_free_listed_entities`** instead of spelling **`hu_graph_entity_t`** / **`hu_graph_relation_t`** / **`hu_graph_entities_free`** in those TUs. **`hu_provenance_render`** takes **`const hu_memory_relation_row_t *`**. **`world_model_bridge.c`** inventory is down to **`hu_w7_facade_open(hu_graph_t *, …)`** only. **Remaining bulk:** **`autodream.c`** (graph-only upsert + public **`hu_graph_t *`** APIs + `ad_sqlite` fallback), **`cli.c`** (graph open/close for offline tools), **persona** modules (graph-scoped delta APIs). **`counterfactual.c`** / **`case_based.c`** remain at zero matches in this inventory.

`src/feeds/`: **0** matches (no direct graph API in `.c` / `.h` under that tree at this snapshot).

## Exception policy (Phase 1 exit until hot-path migration completes)

The execution plan allows **documented exceptions** instead of an immediate “&gt;80% line reduction” metric. Until each item is removed, the following patterns are **explicitly allowed**:

1. **`hu_memory_facade_graph_handle(m)` → `hu_graph_*`**  
   Runners and W9 code that already take `hu_memory_facade_t *m` but call the graph for operations not yet exposed on the facade query/record API (e.g. specialized SQL, world-model rebuild). Owner: W7/W9. Removal: as `hu_memory_facade_read` / `write` (or thin **`hu_memory_facade_*`** delegates like temporal/causal/relation-belief) cover each call shape.

2. **`hu_graph_sqlite_connection`** for SQLite DDL / batch maintenance  
   Public accessor in `human/memory/graph.h` when `HU_ENABLE_SQLITE`; the internal `hu_graph__db_handle` in `src/memory/graph.c` is not part of the public contract. **`autodream.c`** uses **`hu_memory_facade_sqlite_db`** when a facade is supplied (`ad_sqlite`); graph-only callers still hit **`hu_graph_sqlite_connection`** via that helper or the public summarize/read APIs that only take **`hu_graph_t *`**. **`case_based.c`** does not open the DB directly for its primary path. Owner: W7 + W2 consolidation. Removal: optional facade overloads for summarize/read, or keep graph-only public APIs.

3. **Persona delta producers** (`persona_deltas.c`, `delta_observer.c`)  
   Still graph-adjacent for W5 delta persistence; migration is a dedicated pass once facade `HU_MEM_PERSONA_DELTA` paths cover all writer/reader shapes. Owner: persona + W7.

4. **`hu_persona_load` at agent init** (`agent.c`)  
   Documented in-source: no `hu_memory_facade_t *` at init time; deferred to W9 single-load. Owner: agent init + W9.

## Recommended migration order (next PRs)

1. ~~**anticipatory**~~ — **Done:** **`hu_memory_facade_query_temporal` / `query_causal`**, entity **`hu_memory_facade_read`** (BY_NAME), graph **`hu_anticipatory_analyze`** delegates via short-lived `hu_memory_facade_open`.
2. **autodream** — **partial:** same as prior snapshot; largest remaining **`hu_graph_*`** bucket under `src/agent/` (graph-only upsert + public graph-pointer APIs + `ad_sqlite` fallback + comment).
3. ~~**belief_reverify_runner**~~ — **Done:** facade reads + belief set/get; payload rows use **`hu_memory_relation_row_t`** in-source (no **`hu_graph_*`** substring in that TU).
4. ~~**world_model.c** / **world_model_bridge** (cast noise)~~ — **Done for listed-entity free + row typedefs:** **`hu_memory_facade_free_listed_entities`**; **`world_model.c`** graph hits reduced to graph-only **`hu_negative_memory_*`** / **`hu_graph_sqlite_connection`** entrypoints. Bridge: **`hu_w7_facade_open(hu_graph_t *)`** only.
5. ~~**self_rag_atomic** / **response_verifier**~~ — **Done:** use **`hu_memory_relation_row_t`** (and **`memory.h`**) instead of spelling graph relation types in agent TUs.
6. **`src/agent/cli.c`** + **persona (`persona_deltas.c`, `delta_observer.c`)** — next surfaces: CLI graph lifecycle; persona delta APIs still take **`hu_graph_t *`** and **`hu_graph_sqlite_connection`** until W5 facade write shapes land.

**Done in-tree (reference):** `HU_MEM_CASE` + **`hu_case_*`** on the facade; verifier scoring via **`HU_MEMORY_REL_VERIFIER_SCAN`** + **`hu_graph_list_relations_verifier_scan`**; atomic correction path uses **`hu_memory_facade_read`** (`HU_MEM_RELATION` list); anticipatory + belief re-verify paths above.

## Proof obligations

- After each migration slice: `./build/human_tests --suite=W7` (or subsystem suite named in the PR) + no new dual-include violations (`bash scripts/check-memory-v2-header-collision.sh`).
- Full gate before merge: `./build/human_tests` (G1).
