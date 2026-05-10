---
title: "W12 — Goal-Conditioned Retrieval + Multi-Hop Reasoning"
created: 2026-05-10
status: proposed
parent: 2026-05-10-memory-v2-roadmap-overview.md
risk: medium
scope: include/human/agent/, src/agent/, src/memory/
---

# W12 — Goal-Conditioned Retrieval

## Goal

Replace ad-hoc memory queries scattered across `src/agent/context.c` with one planner: `hu_planner_t`. The planner takes a goal + world model and emits a retrieval plan (which memory to fetch, how many hops, what budget). Implements **HippoRAG-style PageRank** for soft retrieval and **multi-hop traversal** with verifier loops.

## Motivation

Today, retrieval is keyword + recency. Complex queries — "when did Alice and Bob last collaborate on funding?" — require 3-hop reasoning: find Alice, find collaboration relations, intersect with Bob, time-filter on funding topic. v1 cannot do this in one round-trip; the agent has to fall back to LLM stitching, which is the path to hallucination.

## Prior art

- HippoRAG (arxiv 2405.14831) — PageRank over entity graph for memory retrieval.
- GraphRAG-Local (Microsoft) — community-aware retrieval.
- LangChain `MultiQueryRetriever` — goal-conditioned plan.
- v1 W3 case-based recall — exists, but only for plan retrieval, not facts.

## Design

### Vtable

```c
/* include/human/agent/planner.h */

typedef struct hu_retrieval_step {
    hu_memory_kind_t kind;
    hu_memory_query_t query;
    size_t hops;            /* 0 for direct lookup, 1-3 for traversal */
    int budget_ms;          /* per-step latency budget */
    bool verify_after;      /* run W11 self-RAG on results */
} hu_retrieval_step_t;

typedef struct hu_retrieval_plan {
    hu_retrieval_step_t steps[8];
    size_t steps_count;
    int total_budget_ms;
} hu_retrieval_plan_t;

typedef struct hu_planner_vtable {
    const char *name;
    hu_error_t (*plan)(void *ctx, const char *goal, size_t goal_len,
                       const hu_world_model_t *wm, hu_retrieval_plan_t *out_plan);
    void (*deinit)(void *ctx);
} hu_planner_vtable_t;

typedef struct hu_planner {
    hu_planner_vtable_t *vt;
    void *ctx;
} hu_planner_t;

hu_error_t hu_planner_heuristic(hu_planner_t *out);    /* fast, deterministic */
hu_error_t hu_planner_llm(hu_provider_t *p, hu_planner_t *out);  /* model-driven */

/* Execute the plan. Returns aggregated records from all steps, scored, top-K. */
hu_error_t hu_planner_execute(hu_memory_t *m, hu_self_rag_t *self_rag,
                               const hu_retrieval_plan_t *plan, hu_allocator_t *alloc,
                               hu_memory_record_t **out, size_t *out_count);
```

### HippoRAG PageRank (soft retrieval)

```c
/* include/human/memory/pagerank.h */

hu_error_t hu_memory_pagerank_seeds(hu_memory_t *m, hu_allocator_t *alloc,
                                     const char *contact_id, size_t cid_len,
                                     const int64_t *seed_entity_ids, size_t seeds_count,
                                     float damping,    /* 0.85 default */
                                     size_t iterations, /* 20 default */
                                     int64_t **out_ids, float **out_scores,
                                     size_t *out_count);
```

Pure CPU implementation: power-iteration over the adjacency. For graphs ≤ 10K entities (the realistic per-contact cap), 20 iterations runs in under 5 ms.

### Multi-hop traversal with verifier loop

```
plan
 │
 ├── step 1 (1-hop): fetch Alice's relations
 │     │
 │     └── verify (W11) — drop unsupported / quarantined edges
 │
 ├── step 2 (1-hop from result): expand to Bob's relations from intersection
 │     │
 │     └── verify
 │
 └── step 3: time-filter to recent 12 months, score by topic match
```

Each step has its own budget; the planner enforces the total budget by truncating steps if needed.

## Phases

1. Author `planner.h` + heuristic backend (template-based plans for common goal verbs).
2. Author HippoRAG PageRank in `src/memory/pagerank.c`.
3. Author `hu_planner_execute` + integration with W11 verifier.
4. Migrate `src/agent/context.c` retrieval calls to use the planner.
5. Author LLM backend (model emits a JSON plan).
6. Adversarial tests.

## Test plan

- `test_w12_heuristic_planner_emits_3_hop_plan_for_relationship_query`.
- `test_w12_pagerank_top_k_matches_expected_subgraph`: known small graph, hand-verified.
- `test_w12_planner_execute_respects_total_budget`.
- `test_w12_planner_verifier_loop_drops_unsupported_facts`.
- `test_w12_planner_handles_empty_world_model_gracefully`.
- `test_w12_adversarial_planner_resists_query_injection`: malicious goal text → still produces a bounded plan.
- `test_w12_locomo_multihop_subset`: passes ≥80% on a sample.

## Success metric

- LoCoMo multi-hop subset: +10 pts vs v1 baseline.
- Multi-hop query p99 ≤ 100 ms on 10K-entity graph.
- Planner-emitted plans deterministic on repeated calls (heuristic backend).
- Binary size delta ≤ +60 KB.

## Risks

| Risk | Mitigation |
|------|------------|
| PageRank is O(N) per iteration; large graphs blow the budget | Cap entity count per call; W14 precomputes hot subgraphs |
| LLM-driven planner produces unbounded plans | Plan validator caps `steps_count ≤ 8` and total budget ≤ 500 ms |
| Multi-hop expansion explodes | Per-hop result cap; verifier-loop drops irrelevant before next hop |

## Out of scope

- Reinforcement-learned plan selection. (Heuristic + LLM only.)
- Plan caching across turns. (Each turn is a fresh plan.)

## Binary size budget: +60 KB.
