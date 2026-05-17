---
title: "W3 — Multi-Graph Topology + Case-Based Planning"
created: 2026-05-10
status: complete
parent: 2026-05-10-memory-roadmap-overview.md
depends_on: 2026-05-10-w1-bitemporal-foundation.md
risk: medium
scope: src/memory/, include/human/memory/, src/agent/
last_audit: 2026-05-17
---

# W3 — Multi-Graph Topology + Case-Based Planning

## Goal

Promote h-uman's existing parallel subgraphs (entity, emotional, contact, episodic) to first-class peer graphs that share a typed cross-edge layer. Wire MAGMA-style cross-graph traversal into retrieval so multi-hop temporal questions ("what was I feeling the last time we talked about X with Casey") get answered by graph traversal rather than LLM guesswork. Wire `outcomes.c` + `episodic.c` into the planner so case-based reasoning happens automatically: the planner retrieves the K most similar past tasks and biases its plan from their outcomes.

## Motivation

Today the project ships:

- `src/memory/graph.c` — entity + relation graph (W1 makes it bitemporal)
- `src/memory/emotional_graph.c` — topic ↔ emotion graph
- `src/memory/contact_graph.c` — identity ↔ platform_handle graph
- `src/memory/episodic.c` — contact-scoped narrative episodes
- `src/memory/relational_episode.c` — relational episode capture

Each is reachable via its own API. There is no single "give me everything related to X across all four subgraphs" entry point. Cross-graph reasoning happens in the LLM, not in the graph. MAGMA's 0.7 LoCoMo score (vs MemoryOS 0.55) was attributed largely to dedicated cross-graph traversal for multi-hop temporal Qs.

Separately: `src/agent/outcomes.c`, `src/agent/episodic.c`, `src/agent/planner.c`, and `src/agent/mcts_planner.c` exist but the planner does not call episodic similarity by default. So the system has memory of past plans and outcomes but does not reuse it.

## Prior art

- MAGMA (arxiv 2601.03236) — three-subgraph topology (event ⊕ entity ⊕ concept) with cross-graph traversal; top LoCoMo score 0.7.
- Letta — case-based planning via episodic similarity.
- MemRL (arxiv early 2026) — RL-based memory write policies trained on planning outcomes.
- Existing plan: `docs/plans/2026-03-21-elastic-memory-episodic.md` (cognitive replay + pattern compression). W3 is the architectural prerequisite for that plan; if W3 lands, that plan can be revived.

## Design

### 1. Cross-edge layer

New module `src/memory/cross_graph.c` + `include/human/memory/cross_graph.h`.

A cross-edge connects an entity in one subgraph to an entity in another. Stored in a single SQLite table:

```sql
CREATE TABLE IF NOT EXISTS cross_edges (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contact_id TEXT NOT NULL DEFAULT '',
    src_graph TEXT NOT NULL,        /* "entity", "emotion", "contact", "episode" */
    src_id INTEGER NOT NULL,
    dst_graph TEXT NOT NULL,
    dst_id INTEGER NOT NULL,
    relation TEXT NOT NULL,         /* e.g. "FELT_DURING", "MENTIONED_IN", "ABOUT" */
    confidence REAL DEFAULT 1.0,
    event_start INTEGER DEFAULT 0,
    event_end INTEGER DEFAULT 0,
    weight REAL DEFAULT 1.0,
    UNIQUE(contact_id, src_graph, src_id, dst_graph, dst_id, relation)
);
CREATE INDEX IF NOT EXISTS idx_xedge_src ON cross_edges(src_graph, src_id);
CREATE INDEX IF NOT EXISTS idx_xedge_dst ON cross_edges(dst_graph, dst_id);
```

Public API:

```c
typedef struct hu_cross_edge {
    int64_t id;
    const char *src_graph;
    int64_t src_id;
    const char *dst_graph;
    int64_t dst_id;
    const char *relation;
    float confidence;
    int64_t event_start;
    int64_t event_end;
    float weight;
} hu_cross_edge_t;

hu_error_t hu_cross_edge_upsert(hu_graph_t *g, const hu_cross_edge_t *edge);

hu_error_t hu_cross_graph_traverse(
    hu_graph_t *g, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len,
    const char *start_graph, int64_t start_id,
    size_t max_hops,            /* default 2 */
    size_t max_results,
    int64_t event_window_start, /* 0 = unbounded */
    int64_t event_window_end,
    hu_cross_edge_t **out, size_t *out_count);
```

Traversal is bounded by `max_hops` AND `max_results` AND the event window — this prevents the blowup that the canvas flagged.

### 2. Population — where cross-edges come from

Cross-edges are written from existing call sites with no schema-level coupling to the source subgraphs:

- `src/memory/emotional_moments.c` already detects an emotional moment; on detection, write a cross-edge `(episode → emotion, "FELT_DURING")`.
- `src/memory/fact_extract.c` writes entity-graph relations; on extraction during a contact-scoped session, write `(entity → contact, "MENTIONED_BY")`.
- `src/memory/episodic.c` records episodes; on episode completion, write `(episode → entities-in-episode, "ABOUT")`.
- W2's AutoDream `phase_derive_facts` extends its rules to write cross-edges where applicable.

No subgraph implementation changes. The cross-edge layer is additive.

### 3. Retrieval integration

`src/memory/retrieval/engine.c` gets a new strategy: cross-graph hop expansion. When QMD classifies a query as multi-hop temporal:

1. Standard hybrid retrieval finds candidate entities.
2. For each candidate, run `hu_cross_graph_traverse` with `max_hops=2` bounded by the query's time window.
3. The expanded result set goes through reranker as usual.

This is a strategy added to `hu_retrieval_engine_t`'s existing strategy table, not a new vtable.

### 4. Case-based planning

New module `src/agent/case_based.c` + `include/human/agent/case_based.h`.

```c
typedef struct hu_case_match {
    int64_t episode_id;
    float similarity;          /* 0.0 - 1.0 */
    float outcome_quality;     /* 0.0 - 1.0 from outcomes.c */
    const char *plan_summary;  /* what the agent did */
    const char *outcome_text;  /* what happened */
    int64_t event_start;
} hu_case_match_t;

hu_error_t hu_case_based_recall(
    hu_allocator_t *alloc,
    hu_graph_t *graph,
    hu_memory_t *memory,
    const char *current_task, size_t current_task_len,
    size_t k,                  /* default 3 */
    hu_case_match_t **out, size_t *out_count);

hu_error_t hu_case_based_format_for_planner(
    hu_allocator_t *alloc,
    const hu_case_match_t *cases, size_t case_count,
    char **out_md, size_t *out_md_len);
```

`hu_case_based_recall` uses episodic similarity (existing embeddings via `src/memory/vector/embeddings.c`) plus `hu_cross_graph_traverse` to find episodes whose entities overlap. It then joins to `outcomes.c` for the outcome quality.

Wired into `src/agent/planner.c` and `src/agent/mcts_planner.c`: at plan start, the planner calls `hu_case_based_recall` and prepends the formatted markdown to its prompt context. Replay is **attributed** ("Last time you saw a similar task on 2026-04-12, you used X and the outcome was Y") — never presented as ground truth.

### 5. Provenance

Every cross-edge and every case-based recall surfaces an attribution string the agent must use when citing memory. This pre-populates W4's provenance receipts.

## File map

| File | Role |
|------|------|
| `include/human/memory/cross_graph.h` | New — public API |
| `src/memory/cross_graph.c` | New — table + traverse |
| `src/memory/graph.c` | Add cross_edges schema migration |
| `src/memory/retrieval/engine.c` | New strategy: cross-graph hop expansion |
| `src/memory/retrieval/qmd.c` | Classify multi-hop-temporal queries |
| `src/memory/emotional_moments.c` | Emit cross-edges on detection |
| `src/memory/fact_extract.c` | Emit cross-edges on extraction |
| `src/memory/episodic.c` | Emit cross-edges on episode close |
| `include/human/agent/case_based.h` | New — public API |
| `src/agent/case_based.c` | New — recall + format |
| `src/agent/planner.c` | Call `hu_case_based_recall` at plan start |
| `src/agent/mcts_planner.c` | Same — at MCTS root expansion |
| `tests/test_cross_graph.c` | New — schema, traverse, hop limits, event-window |
| `tests/test_case_based.c` | New — recall ranking, attribution format |
| `tests/test_planner_case_based.c` | New — planner integration with stub graph |
| `eval_suites/multi_hop_temporal.json` | New — 15 multi-hop temporal questions |

## Test strategy

- Cross-edge schema round-trip; uniqueness; event-window filter.
- Traversal: assert hop limit and result limit honored; cycles don't loop.
- Case-based recall: 5 fixture episodes, ask for top 3, assert ranking matches expected (similarity × outcome_quality).
- Planner integration: stub graph + episode store; assert planner prompt contains "Last time…" attribution.
- ASan clean.

## Success criteria

- All existing tests still pass.
- Multi-hop temporal eval: ≥ baseline + 5 points.
- Plan-quality eval on tasks resembling prior episodes: measurable lift.
- Binary size delta: < 50 KB.

## Risks

| Risk | Mitigation |
|------|------------|
| Cross-graph traversal blows up retrieval latency | `max_hops` and `max_results` bounded; event-window filter required for multi-hop |
| Case-based recall surfaces irrelevant past episodes as "similar" | Similarity threshold (default 0.6); attribution makes failure modes visible |
| Episode → outcome join misses for pre-W3 data | Backfill is a one-time AutoDream phase in W3's first-run path |
| Cross-edges drift out of sync with their source subgraphs | Deletion of source row triggers cross-edge cleanup via SQLite trigger |

## Open questions

1. Should cross-edges have their own bitemporal fields or inherit from the source row? Recommendation: own — supports cases like "Casey felt anxious about the move" where the emotion's window is narrower than the move's.
2. What's the right `k` for case-based recall — 3, 5, or learned? Recommendation: 3 in W3; W6's MemRL can learn it later.

## References

- MAGMA: arxiv 2601.03236
- Letta benchmarking blog
- Project prior work: `docs/plans/2026-03-21-elastic-memory-episodic.md`
