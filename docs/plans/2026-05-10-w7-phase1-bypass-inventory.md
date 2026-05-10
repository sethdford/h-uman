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

## Snapshot (2026-05-10, zero matches under agent/persona/feeds)

Per-file match counts for the substring **`hu_graph_`** in `src/agent/**/*.c`, `src/agent/**/*.h`, `src/persona/**/*.c`, `src/persona/**/*.h`, `src/feeds/**/*.c`, `src/feeds/**/*.h`.

| Count | Path |
|------:|------|
| **0** | *(no files in this scan contain `hu_graph_`)* |
| **0** | **total** |

**How this is possible without deleting the v1 graph:** the graph module and **`hu_graph_*`** APIs remain in `include/human/memory/graph.h`, `src/memory/graph.c`, and other trees **outside** this ripgrep scope. Agent/persona/feeds code now uses:

- **`struct hu_graph *`** in public signatures where a v1 graph pointer is required (ABI-identical to **`hu_graph_t *`**).
- Thin W7 entry points in **`human/memory/memory.h`** / **`src/memory/memory.c`**: **`hu_memory_sqlite_from_graph`**, **`hu_memory_v1_graph_open`** / **`hu_memory_v1_graph_close`**, **`hu_memory_facade_open_on_graph`**, **`hu_memory_v1_upsert_relation_with_belief`**, plus earlier **`hu_memory_entity_row_t`** / **`hu_memory_relation_row_t`** and **`hu_memory_facade_free_listed_entities`**.

`src/feeds/`: **0** matches.

## Exception policy (Phase 1 exit — inventory slice closed)

The execution plan allowed **documented exceptions** instead of an immediate “&gt;80% line reduction” metric. For the **scoped ripgrep inventory** (agent + persona + feeds sources listed above), Phase 1’s “no stray `hu_graph_*` spellings” goal is **met**: new graph-touching code in those directories should go through **`human/memory/memory.h`** helpers or **`struct hu_graph *`** parameters, not new raw **`hu_graph_sqlite_connection`** / **`hu_graph_open`** call sites.

**Still true elsewhere:** full-tree greps (e.g. `src/memory/`, headers under `include/human/memory/graph.h`) will continue to show **`hu_graph_*`** — that is expected until a future “calls-only” inventory or a deeper v2 backend replaces the v1 graph entirely.

## Recommended migration order (reference — largely complete in-tree)

1. ~~**anticipatory**~~ — facade queries + **`hu_memory_facade_open_on_graph`**.
2. ~~**autodream**~~ — **`ad_sqlite`**, **`hu_memory_v1_upsert_relation_with_belief`** for graph-only quarantine release; public APIs use **`struct hu_graph *`**.
3. ~~**belief_reverify_runner**~~ — facade reads + belief set/get + row typedefs.
4. ~~**world_model** / **bridge**~~ — negative memory on facade SQLite + **`hu_memory_facade_open_on_graph`**.
5. ~~**self_rag_atomic** / **response_verifier**~~ — **`hu_memory_relation_row_t`**.
6. ~~**CLI**~~ — **`hu_memory_v1_graph_*`** for the optional `~/.human/graph.db` session graph.
7. ~~**persona_deltas** / **delta_observer**~~ — graph parameters as **`struct hu_graph *`**; SQLite via **`hu_memory_sqlite_from_graph`** on graph-only paths.

## Proof obligations

- After each migration slice: `./build/human_tests --suite=W7` (or subsystem suite named in the PR) + no new dual-include violations (`bash scripts/check-memory-v2-header-collision.sh`).
- Full gate before merge: `./build/human_tests` (G1).
