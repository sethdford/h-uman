/*
 * W12 — Goal-conditioned retrieval planner (HippoRAG-style).
 *
 * Distinct from the existing tool-decomposition planner in
 * `human/agent/planner.h` (which produces a tool-DAG plan from an LLM JSON
 * response). This surface plans a memory-retrieval workflow: which kind of
 * memory to fetch, how many hops to traverse, what budget to spend.
 *
 * Two backends:
 *
 *   - heuristic: deterministic, pattern-matches goal verbs (when/where/who/
 *     last/between/with) and synthesizes 1-3 step plans against the W7
 *     facade (HU_MEM_ENTITY, HU_MEM_RELATION). Cheap, no I/O, no provider.
 *
 *   - llm: takes a `hu_provider_t *`, calls it with a JSON-schema-locked
 *     prompt, parses the response into a plan, and clamps to hard caps.
 *     Falls back to a deterministic single-step plan when the provider is
 *     NULL, the call fails, or the JSON is malformed.
 *
 * `hu_planner_execute` walks the plan, dispatches each step through the
 * memory facade, optionally runs the W11 self-RAG verifier between hops,
 * and aggregates a deduplicated record array for the caller.
 *
 * Hard caps (enforced in plan() and execute()):
 *   - steps_count    <= HU_PLANNER_MAX_STEPS              (8)
 *   - total_budget_ms <= HU_PLANNER_MAX_TOTAL_BUDGET_MS   (500)
 *
 * Naming note: the spec for W12 (docs/plans/2026-05-10-w12-goal-conditioned-
 * retrieval.md) places these declarations in `planner.h`. The existing
 * `human/agent/planner.h` is the tool-DAG planner used pervasively across
 * the agent loop; merging W12 into that header would force every agent
 * include site to pull in the world-model and memory-facade headers, which
 * collide with the parallel `human/intelligence/world_model.h` and
 * `human/memory.h` typedefs. Splitting into a sibling header keeps the
 * blast radius bounded. See AGENTS.md §10 (anti-patterns: cross-subsystem
 * coupling) — better to add a focused header than to retrofit a widely-
 * included one.
 *
 * Layer 4 of the v2 stack (see docs/plans/2026-05-10-memory-v2-roadmap-
 * overview.md). Reads layers 1-3 (facade, beliefs, world model); will
 * replace the ad-hoc retrieval calls in src/agent/context.c in a follow-up.
 */
#ifndef HU_AGENT_RETRIEVAL_PLANNER_H
#define HU_AGENT_RETRIEVAL_PLANNER_H

#include "human/agent/world_model.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/memory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* W11 self-RAG verifier is now in-tree at all times (`include/human/agent/
 * self_rag.h`). The planner accepts a verifier handle for callers that want
 * inline per-step verification but, until the inline integration ships,
 * simply records the pointer and lets W11 run post-response from
 * `world_model_bridge.c`. Pass NULL to opt out entirely. */
#include "human/agent/self_rag.h"

/* Forward-declare hu_provider_t to keep this header lean (provider.h pulls
 * in chat / tool / streaming surface area not needed here). */
typedef struct hu_provider hu_provider_t;

#define HU_PLANNER_MAX_STEPS               8
#define HU_PLANNER_MAX_TOTAL_BUDGET_MS     500
#define HU_PLANNER_MAX_GOAL_LEN            4096   /* adversarial-input ceiling */

/* One step of a retrieval plan. */
typedef struct hu_retrieval_step {
    hu_memory_kind_t   kind;          /* which backend to dispatch to */
    hu_memory_query_t  query;         /* fully-formed query payload */
    size_t             hops;          /* 0 for direct; 1-3 for traversal */
    int                budget_ms;     /* per-step latency hint */
    bool               verify_after;  /* run W11 self-RAG on the result */
} hu_retrieval_step_t;

/* A complete plan. Steps run in array order. */
typedef struct hu_retrieval_plan {
    hu_retrieval_step_t steps[HU_PLANNER_MAX_STEPS];
    size_t              steps_count;
    int                 total_budget_ms; /* 0 = unlimited (within step caps) */
} hu_retrieval_plan_t;

/* Backend vtable. */
typedef struct hu_planner_vtable {
    const char *name;
    hu_error_t (*plan)(void *ctx, const char *goal, size_t goal_len,
                       const hu_world_model_t *wm, hu_retrieval_plan_t *out_plan);
    void       (*deinit)(void *ctx);
} hu_planner_vtable_t;

typedef struct hu_planner {
    hu_planner_vtable_t *vt;
    void                *ctx;
} hu_planner_t;

/* Heuristic backend: fast, deterministic, no I/O. Always succeeds. */
hu_error_t hu_planner_heuristic(hu_planner_t *out);

/* LLM backend. Calls the provider with a JSON-schema-locked prompt, parses
 * and clamps the response, and falls back to a deterministic single-step
 * plan when the provider is NULL, the call fails, or the JSON is malformed.
 * Tests always take the fallback path (HU_IS_TEST) so they stay
 * deterministic and free of provider I/O. */
hu_error_t hu_planner_llm(hu_provider_t *p, hu_planner_t *out);

/* Configure the LLM backend's allocator and optional model override. The
 * allocator is required for the LLM round-trip; without it, `plan()` takes
 * the deterministic fallback. The model string is copied. Safe to call
 * multiple times; previous model copy is freed. */
void hu_planner_llm_configure(hu_allocator_t *alloc, const char *model, size_t model_len);

/* Convenience: invoke the backend's plan() with cap enforcement.
 * Equivalent to vt->plan(...) followed by post-clamp; preferable because
 * it guarantees the caps even if a backend forgets to clamp. */
hu_error_t hu_planner_plan(hu_planner_t *p, const char *goal, size_t goal_len,
                           const hu_world_model_t *wm, hu_retrieval_plan_t *out_plan);

/* Release backend resources. Safe with NULL. */
void hu_planner_close(hu_planner_t *p);

/* Execute a plan. Walks each step, dispatches through the facade, runs the
 * W11 verifier when available (no-op today if `self_rag` is NULL), and
 * aggregates summary records into `*out`. Records carry id, kind,
 * confidence, event_start/end. Payloads are NOT carried through; callers
 * needing payload re-fetch via hu_memory_facade_read.
 *
 * The output array is allocated via `alloc` and MUST be freed with
 * hu_planner_records_free. Returns HU_ERR_INVALID_ARGUMENT if the plan
 * exceeds the hard caps. `self_rag` may be NULL. */
hu_error_t hu_planner_execute(hu_memory_facade_t *m, hu_self_rag_t *self_rag,
                              const hu_retrieval_plan_t *plan, hu_allocator_t *alloc,
                              hu_memory_record_t **out, size_t *out_count);

/* Free the records array returned by hu_planner_execute. Safe with NULL/0. */
void hu_planner_records_free(hu_allocator_t *alloc, hu_memory_record_t *records,
                             size_t count);

/* ── Goal-conditioned (PageRank) backend ──────────────────────────────── */

/* W12: HippoRAG-style goal-conditioned planner. Runs personalized PageRank
 * over the contact's entity graph, seeded by world-model entities, to
 * prioritize which neighbors to expand. Falls back to the heuristic backend
 * when the graph is empty or PageRank returns no results.
 *
 * `m` must remain valid for the lifetime of the planner (borrowed, not
 * owned). `alloc` is used for scratch allocations during plan(). */
hu_error_t hu_planner_goal_conditioned(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                       hu_planner_t *out);

/* ── Multi-hop traversal ──────────────────────────────────────────────── */

#define HU_PLANNER_MULTI_HOP_DEFAULT    3
#define HU_PLANNER_MULTI_HOP_MAX        5

/* W12: iterative multi-hop retrieval with PageRank scoring.
 *
 * 1. Execute `initial_query` against the memory facade.
 * 2. For each hop (up to `max_hops`, clamped to HU_PLANNER_MULTI_HOP_MAX):
 *    extract entity IDs from current results, query neighbors, score with
 *    personalized PageRank, keep top-K.
 * 3. Deduplicate across all hops.
 *
 * `max_hops == 0` uses HU_PLANNER_MULTI_HOP_DEFAULT.
 * Caller frees `*out` via hu_planner_records_free. */
hu_error_t hu_planner_multi_hop(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                const hu_memory_query_t *initial_query,
                                size_t max_hops,
                                hu_memory_record_t **out, size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_RETRIEVAL_PLANNER_H */
