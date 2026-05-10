/* W12 — Goal-conditioned planner + HippoRAG-style PageRank.
 *
 * Tests run against an in-memory SQLite DB via hu_graph_open(NULL, 0).
 * All allocations free before return — ASan is the final arbiter. */

#include "human/agent/retrieval_planner.h"
#include "human/agent/world_model.h"
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"
#include "human/memory/pagerank.h"
#include "test_framework.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) { g_alloc = hu_system_allocator(); return &g_alloc; }

static void open_facade_(hu_graph_t **g, hu_memory_t **m) {
    HU_ASSERT_EQ(hu_graph_open(A(), NULL, 0, g), HU_OK);
    HU_ASSERT_NOT_NULL(*g);
    HU_ASSERT_EQ(hu_memory_open(A(), *g, m), HU_OK);
    HU_ASSERT_NOT_NULL(*m);
    hu_world_model_invalidate(NULL, 0);
}

static void close_facade_(hu_graph_t *g, hu_memory_t *m) {
    hu_world_model_invalidate(NULL, 0);
    hu_memory_close(m, A());
    hu_graph_close(g, A());
}

static int64_t add_entity(hu_graph_t *g, const char *cid, const char *name,
                          hu_entity_type_t t) {
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, cid, strlen(cid), name, strlen(name), t,
                                         NULL, &id),
                 HU_OK);
    return id;
}

static void add_relation(hu_graph_t *g, const char *cid, int64_t s, int64_t t,
                         hu_relation_type_t rt) {
    HU_ASSERT_EQ(hu_graph_upsert_relation(g, cid, strlen(cid), s, t, rt, 1.0f, NULL, 0),
                 HU_OK);
}

static hu_world_model_t *load_wm(hu_memory_t *m, const char *cid) {
    hu_world_model_t *wm = NULL;
    HU_ASSERT_EQ(hu_world_model_build(m, A(), cid, strlen(cid), 1735690000000LL, &wm),
                 HU_OK);
    HU_ASSERT_NOT_NULL(wm);
    return wm;
}

/* ── Heuristic planner ──────────────────────────────────────────────────── */

static void test_w12_heuristic_planner_emits_3_hop_plan_for_relationship_query(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);

    /* Two anchors so the "between" branch fires fully. */
    add_entity(g, "u1", "Alice", HU_ENTITY_PERSON);
    add_entity(g, "u1", "Bob",   HU_ENTITY_PERSON);
    hu_world_model_t *wm = load_wm(m, "u1");

    hu_planner_t p;
    HU_ASSERT_EQ(hu_planner_heuristic(&p), HU_OK);

    hu_retrieval_plan_t plan;
    const char *goal = "when did Alice and Bob last collaborate between funding rounds?";
    HU_ASSERT_EQ(hu_planner_plan(&p, goal, strlen(goal), wm, &plan), HU_OK);

    /* Three steps: anchor-1 neighbours, anchor-2 neighbours, time-window. */
    HU_ASSERT_EQ((int)plan.steps_count, 3);
    HU_ASSERT_LT(plan.total_budget_ms, HU_PLANNER_MAX_TOTAL_BUDGET_MS + 1);
    HU_ASSERT_GT(plan.total_budget_ms, 0);
    /* Verifier on every step (multi-hop is high-stakes). */
    HU_ASSERT(plan.steps[0].verify_after);
    HU_ASSERT(plan.steps[1].verify_after);
    HU_ASSERT(plan.steps[2].verify_after);
    /* Step 1/2 are entity-anchored neighbour expansions. */
    HU_ASSERT_EQ(plan.steps[0].kind, HU_MEM_ENTITY);
    HU_ASSERT_EQ(plan.steps[1].kind, HU_MEM_ENTITY);
    /* Step 3 is a relations window. */
    HU_ASSERT_EQ(plan.steps[2].kind, HU_MEM_RELATION);

    hu_planner_close(&p);
    hu_world_model_free(A(), wm);
    close_facade_(g, m);
}

static void test_w12_heuristic_planner_temporal_query_emits_window(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);
    add_entity(g, "u1", "Alice", HU_ENTITY_PERSON);
    hu_world_model_t *wm = load_wm(m, "u1");

    hu_planner_t p;
    HU_ASSERT_EQ(hu_planner_heuristic(&p), HU_OK);

    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_plan(&p, "when was the last meeting?", 25, wm, &plan), HU_OK);
    HU_ASSERT_EQ((int)plan.steps_count, 1);
    HU_ASSERT_EQ(plan.steps[0].kind, HU_MEM_RELATION);
    HU_ASSERT(plan.steps[0].verify_after);

    hu_planner_close(&p);
    hu_world_model_free(A(), wm);
    close_facade_(g, m);
}

static void test_w12_heuristic_planner_default_plan_for_bare_goal(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);
    hu_world_model_t *wm = load_wm(m, "u1");

    hu_planner_t p;
    HU_ASSERT_EQ(hu_planner_heuristic(&p), HU_OK);

    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_plan(&p, "tell me something", 17, wm, &plan), HU_OK);
    /* Catch-all path: 1-step relations list. */
    HU_ASSERT_EQ((int)plan.steps_count, 1);
    HU_ASSERT_EQ(plan.steps[0].kind, HU_MEM_RELATION);

    hu_planner_close(&p);
    hu_world_model_free(A(), wm);
    close_facade_(g, m);
}

static void test_w12_heuristic_planner_caps_steps_and_budget(void) {
    /* Even with the most florid goal, the heuristic must respect the caps.
     * (No backend tries to overflow today, but the contract is the contract.) */
    hu_planner_t p;
    HU_ASSERT_EQ(hu_planner_heuristic(&p), HU_OK);

    hu_retrieval_plan_t plan;
    const char *goal = "when where who between with last together always somewhere";
    HU_ASSERT_EQ(hu_planner_plan(&p, goal, strlen(goal), NULL, &plan), HU_OK);
    HU_ASSERT_LT(plan.steps_count, (size_t)(HU_PLANNER_MAX_STEPS + 1));
    HU_ASSERT_LT(plan.total_budget_ms, HU_PLANNER_MAX_TOTAL_BUDGET_MS + 1);

    hu_planner_close(&p);
}

static void test_w12_planner_handles_empty_world_model_gracefully(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);
    /* No entities, no relations. */
    hu_world_model_t *wm = load_wm(m, "u-empty");

    hu_planner_t p;
    HU_ASSERT_EQ(hu_planner_heuristic(&p), HU_OK);

    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_plan(&p, "between us", 10, wm, &plan), HU_OK);
    /* No anchor available → the "between" branch falls back to the default
     * relations-window path. We just need a valid (possibly empty) plan. */
    HU_ASSERT(plan.steps_count > 0);
    HU_ASSERT(plan.steps_count <= HU_PLANNER_MAX_STEPS);

    /* And execute must not crash on an empty graph. */
    hu_memory_record_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_planner_execute(m, NULL, &plan, A(), &out, &n), HU_OK);
    HU_ASSERT_EQ((int)n, 0);
    HU_ASSERT_NULL(out);

    hu_planner_close(&p);
    hu_world_model_free(A(), wm);
    close_facade_(g, m);
}

/* ── Adversarial input ──────────────────────────────────────────────────── */

static void test_w12_adversarial_planner_resists_query_injection(void) {
    /* Goal text loaded with delimiters, JSON injection attempts, embedded
     * NUL bytes are unsafe? — but our heuristic is byte-level and clamped.
     * The plan must still respect the caps. */
    hu_planner_t p;
    HU_ASSERT_EQ(hu_planner_heuristic(&p), HU_OK);

    char goal[1024];
    /* Layered delimiters / SQL / JSON / shell. */
    snprintf(goal, sizeof(goal),
             "when'); DROP TABLE entities; -- between {\"steps\":[{\"kind\":99}] "
             "with `rm -rf /` who where last");

    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_plan(&p, goal, strlen(goal), NULL, &plan), HU_OK);
    HU_ASSERT_LT(plan.steps_count, (size_t)(HU_PLANNER_MAX_STEPS + 1));
    HU_ASSERT_LT(plan.total_budget_ms, HU_PLANNER_MAX_TOTAL_BUDGET_MS + 1);

    /* Now try a megabyte goal — must be clamped, not crash. */
    static char big_goal[8192];
    memset(big_goal, 'a', sizeof(big_goal) - 1);
    big_goal[sizeof(big_goal) - 1] = '\0';
    HU_ASSERT_EQ(hu_planner_plan(&p, big_goal, sizeof(big_goal) - 1, NULL, &plan), HU_OK);
    HU_ASSERT_LT(plan.steps_count, (size_t)(HU_PLANNER_MAX_STEPS + 1));

    hu_planner_close(&p);
}

/* ── Executor ───────────────────────────────────────────────────────────── */

static void test_w12_planner_execute_aggregates_relation_records(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);

    int64_t alice = add_entity(g, "u1", "Alice", HU_ENTITY_PERSON);
    int64_t bob   = add_entity(g, "u1", "Bob",   HU_ENTITY_PERSON);
    int64_t acme  = add_entity(g, "u1", "Acme",  HU_ENTITY_ORGANIZATION);
    add_relation(g, "u1", alice, acme, HU_REL_WORKS_AT);
    add_relation(g, "u1", bob,   acme, HU_REL_WORKS_AT);

    hu_world_model_t *wm = load_wm(m, "u1");
    hu_planner_t p;
    HU_ASSERT_EQ(hu_planner_heuristic(&p), HU_OK);

    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_plan(&p, "when did Alice work at Acme?", 28, wm, &plan), HU_OK);

    hu_memory_record_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_planner_execute(m, NULL, &plan, A(), &out, &n), HU_OK);
    HU_ASSERT_GT((int)n, 0);
    /* All returned records are summary-only (payload stripped). */
    for (size_t i = 0; i < n; i++) {
        HU_ASSERT_NULL(out[i].payload);
        HU_ASSERT_EQ((int)out[i].payload_len, 0);
    }
    hu_planner_records_free(A(), out, n);
    hu_planner_close(&p);
    hu_world_model_free(A(), wm);
    close_facade_(g, m);
}

static void test_w12_planner_execute_respects_total_budget(void) {
    /* Construct a max plan and run it with budget = 0 (unlimited): must
     * complete without rejection. */
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);
    int64_t a = add_entity(g, "u1", "Alice", HU_ENTITY_PERSON);
    int64_t b = add_entity(g, "u1", "Acme",  HU_ENTITY_ORGANIZATION);
    add_relation(g, "u1", a, b, HU_REL_WORKS_AT);

    hu_retrieval_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    plan.steps_count = HU_PLANNER_MAX_STEPS; /* exactly the cap */
    plan.total_budget_ms = 0;                 /* unlimited */
    for (size_t i = 0; i < plan.steps_count; i++) {
        plan.steps[i].kind = HU_MEM_RELATION;
        plan.steps[i].query.kind = HU_MEM_RELATION;
        plan.steps[i].query.contact_id = "u1";
        plan.steps[i].query.contact_id_len = 2;
        plan.steps[i].budget_ms = 50;
    }
    hu_memory_record_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_planner_execute(m, NULL, &plan, A(), &out, &n), HU_OK);
    /* Dedup means count is at most the real distinct relation count (1). */
    HU_ASSERT_LT((int)n, 2);
    if (out) hu_planner_records_free(A(), out, n);

    /* Now reject malformed plans. */
    plan.steps_count = HU_PLANNER_MAX_STEPS + 1;
    HU_ASSERT_EQ(hu_planner_execute(m, NULL, &plan, A(), &out, &n), HU_ERR_INVALID_ARGUMENT);

    plan.steps_count = 1;
    plan.total_budget_ms = -5;
    HU_ASSERT_EQ(hu_planner_execute(m, NULL, &plan, A(), &out, &n), HU_ERR_INVALID_ARGUMENT);

    plan.total_budget_ms = HU_PLANNER_MAX_TOTAL_BUDGET_MS + 1;
    HU_ASSERT_EQ(hu_planner_execute(m, NULL, &plan, A(), &out, &n), HU_ERR_INVALID_ARGUMENT);

    close_facade_(g, m);
}

static void test_w12_planner_execute_invalid_args_rejected(void) {
    hu_retrieval_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    hu_memory_record_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_planner_execute(NULL, NULL, &plan, A(), &out, &n),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_planner_execute((hu_memory_t *)1, NULL, NULL, A(), &out, &n),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_planner_execute((hu_memory_t *)1, NULL, &plan, NULL, &out, &n),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_planner_execute((hu_memory_t *)1, NULL, &plan, A(), NULL, &n),
                 HU_ERR_INVALID_ARGUMENT);
}

/* ── LLM stub ───────────────────────────────────────────────────────────── */

static void test_w12_llm_stub_returns_single_step_plan(void) {
    hu_planner_t p;
    HU_ASSERT_EQ(hu_planner_llm(NULL, &p), HU_OK);

    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_plan(&p, "hello", 5, NULL, &plan), HU_OK);
    HU_ASSERT_EQ((int)plan.steps_count, 1);
    HU_ASSERT_EQ(plan.steps[0].kind, HU_MEM_RELATION);
    HU_ASSERT(plan.total_budget_ms > 0);
    HU_ASSERT_LT(plan.total_budget_ms, HU_PLANNER_MAX_TOTAL_BUDGET_MS + 1);

    hu_planner_close(&p);
}

/* ── PageRank ───────────────────────────────────────────────────────────── */

/* Small ground-truth graph (6 nodes):
 *
 *   A — B — C — D
 *   |       |
 *   E       F
 *
 * Seed: {A}.  Expected: A scores highest (own teleport mass).
 *  - B gets PR via A (A's only walkable neighbours are B, E).
 *  - E also gets early PR.
 *  - C, D, F decay with hop distance.
 *
 * We don't pin exact float values (they depend on iteration count), but we
 * assert the rank ordering and the structural properties. */
static void seed_pagerank_graph(hu_graph_t *g, int64_t out_ids[6]) {
    out_ids[0] = add_entity(g, "u1", "A", HU_ENTITY_PERSON);
    out_ids[1] = add_entity(g, "u1", "B", HU_ENTITY_PERSON);
    out_ids[2] = add_entity(g, "u1", "C", HU_ENTITY_PERSON);
    out_ids[3] = add_entity(g, "u1", "D", HU_ENTITY_PERSON);
    out_ids[4] = add_entity(g, "u1", "E", HU_ENTITY_PERSON);
    out_ids[5] = add_entity(g, "u1", "F", HU_ENTITY_PERSON);
    add_relation(g, "u1", out_ids[0], out_ids[1], HU_REL_KNOWS); /* A-B */
    add_relation(g, "u1", out_ids[1], out_ids[2], HU_REL_KNOWS); /* B-C */
    add_relation(g, "u1", out_ids[2], out_ids[3], HU_REL_KNOWS); /* C-D */
    add_relation(g, "u1", out_ids[0], out_ids[4], HU_REL_KNOWS); /* A-E */
    add_relation(g, "u1", out_ids[2], out_ids[5], HU_REL_KNOWS); /* C-F */
}

static float score_for(int64_t id, const int64_t *ids, const float *sc, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (ids[i] == id) return sc[i];
    }
    return -1.0f;
}

static void test_w12_pagerank_top_k_matches_expected_subgraph(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);

    int64_t ids[6];
    seed_pagerank_graph(g, ids);

    int64_t seeds[1] = { ids[0] }; /* A */
    int64_t *out_ids = NULL;
    float   *out_sc  = NULL;
    size_t   out_n   = 0;
    HU_ASSERT_EQ(hu_memory_pagerank_seeds(m, A(), "u1", 2, seeds, 1, 0.85f, 20,
                                            &out_ids, &out_sc, &out_n),
                 HU_OK);
    HU_ASSERT_EQ((int)out_n, 6);
    HU_ASSERT_NOT_NULL(out_ids);
    HU_ASSERT_NOT_NULL(out_sc);

    /* Top-1 must be A. */
    HU_ASSERT_EQ(out_ids[0], ids[0]);
    /* All scores in [0, 1] approximately. */
    for (size_t i = 0; i < out_n; i++) {
        HU_ASSERT(out_sc[i] >= 0.0f);
        HU_ASSERT(out_sc[i] <= 1.0f + 1e-3f);
    }
    /* Sorted descending. */
    for (size_t i = 1; i < out_n; i++) {
        HU_ASSERT(out_sc[i - 1] + 1e-6f >= out_sc[i]);
    }
    /* B and E (1-hop from A) score above D (3 hops). */
    float sB = score_for(ids[1], out_ids, out_sc, out_n);
    float sE = score_for(ids[4], out_ids, out_sc, out_n);
    float sD = score_for(ids[3], out_ids, out_sc, out_n);
    HU_ASSERT(sB > sD);
    HU_ASSERT(sE > sD);

    A()->free(A()->ctx, out_ids, out_n * sizeof(*out_ids));
    A()->free(A()->ctx, out_sc,  out_n * sizeof(*out_sc));
    close_facade_(g, m);
}

static void test_w12_pagerank_deterministic_repeated_calls(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);
    int64_t ids[6];
    seed_pagerank_graph(g, ids);
    int64_t seeds[1] = { ids[0] };

    int64_t *ids_a = NULL; float *sc_a = NULL; size_t na = 0;
    int64_t *ids_b = NULL; float *sc_b = NULL; size_t nb = 0;
    HU_ASSERT_EQ(hu_memory_pagerank_seeds(m, A(), "u1", 2, seeds, 1, 0.85f, 20,
                                            &ids_a, &sc_a, &na), HU_OK);
    HU_ASSERT_EQ(hu_memory_pagerank_seeds(m, A(), "u1", 2, seeds, 1, 0.85f, 20,
                                            &ids_b, &sc_b, &nb), HU_OK);
    HU_ASSERT_EQ((int)na, (int)nb);
    for (size_t i = 0; i < na; i++) {
        HU_ASSERT_EQ(ids_a[i], ids_b[i]);
        HU_ASSERT_FLOAT_EQ(sc_a[i], sc_b[i], 1e-6);
    }
    A()->free(A()->ctx, ids_a, na * sizeof(*ids_a));
    A()->free(A()->ctx, sc_a,  na * sizeof(*sc_a));
    A()->free(A()->ctx, ids_b, nb * sizeof(*ids_b));
    A()->free(A()->ctx, sc_b,  nb * sizeof(*sc_b));
    close_facade_(g, m);
}

static void test_w12_pagerank_zero_seeds_returns_empty(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);
    int64_t ids[6];
    seed_pagerank_graph(g, ids);

    int64_t *out_ids = NULL; float *out_sc = NULL; size_t n = 0;
    HU_ASSERT_EQ(hu_memory_pagerank_seeds(m, A(), "u1", 2, NULL, 0, 0.85f, 20,
                                            &out_ids, &out_sc, &n),
                 HU_OK);
    HU_ASSERT_EQ((int)n, 0);
    HU_ASSERT_NULL(out_ids);
    HU_ASSERT_NULL(out_sc);
    close_facade_(g, m);
}

static void test_w12_pagerank_unknown_seed_yields_empty(void) {
    /* Caller passes seed ids that don't exist in this contact's graph: the
     * function returns success with empty output (no usable teleport mass). */
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);
    int64_t ids[6];
    seed_pagerank_graph(g, ids);

    int64_t bogus[2] = { 999999, 888888 };
    int64_t *out_ids = NULL; float *out_sc = NULL; size_t n = 0;
    HU_ASSERT_EQ(hu_memory_pagerank_seeds(m, A(), "u1", 2, bogus, 2, 0.85f, 20,
                                            &out_ids, &out_sc, &n),
                 HU_OK);
    HU_ASSERT_EQ((int)n, 0);
    HU_ASSERT_NULL(out_ids);
    HU_ASSERT_NULL(out_sc);
    close_facade_(g, m);
}

static void test_w12_pagerank_invalid_args_rejected(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);
    int64_t seeds[1] = { 1 };
    int64_t *out_ids = NULL; float *out_sc = NULL; size_t n = 0;

    HU_ASSERT_EQ(hu_memory_pagerank_seeds(NULL, A(), "u1", 2, seeds, 1, 0.85f, 20,
                                           &out_ids, &out_sc, &n),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_memory_pagerank_seeds(m, NULL, "u1", 2, seeds, 1, 0.85f, 20,
                                           &out_ids, &out_sc, &n),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_memory_pagerank_seeds(m, A(), NULL, 0, seeds, 1, 0.85f, 20,
                                           &out_ids, &out_sc, &n),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_memory_pagerank_seeds(m, A(), "u1", 2, NULL, 1, 0.85f, 20,
                                           &out_ids, &out_sc, &n),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_memory_pagerank_seeds(m, A(), "u1", 2, seeds, 1, 0.85f, 20,
                                           NULL, &out_sc, &n),
                 HU_ERR_INVALID_ARGUMENT);
    close_facade_(g, m);
}

static void test_w12_pagerank_handles_empty_graph(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);

    int64_t seeds[1] = { 1 };
    int64_t *out_ids = NULL; float *out_sc = NULL; size_t n = 0;
    HU_ASSERT_EQ(hu_memory_pagerank_seeds(m, A(), "u-empty", 7, seeds, 1, 0.85f, 20,
                                            &out_ids, &out_sc, &n),
                 HU_OK);
    HU_ASSERT_EQ((int)n, 0);
    HU_ASSERT_NULL(out_ids);
    HU_ASSERT_NULL(out_sc);
    close_facade_(g, m);
}

static void test_w12_pagerank_default_damping_when_out_of_range(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);
    int64_t ids[6];
    seed_pagerank_graph(g, ids);
    int64_t seeds[1] = { ids[0] };

    /* Damping = -1 (invalid) → default. */
    int64_t *ids_neg = NULL; float *sc_neg = NULL; size_t n_neg = 0;
    HU_ASSERT_EQ(hu_memory_pagerank_seeds(m, A(), "u1", 2, seeds, 1, -1.0f, 0,
                                            &ids_neg, &sc_neg, &n_neg), HU_OK);
    /* Damping = 0.85 explicitly. */
    int64_t *ids_def = NULL; float *sc_def = NULL; size_t n_def = 0;
    HU_ASSERT_EQ(hu_memory_pagerank_seeds(m, A(), "u1", 2, seeds, 1, 0.85f, 20,
                                            &ids_def, &sc_def, &n_def), HU_OK);
    HU_ASSERT_EQ((int)n_neg, (int)n_def);
    for (size_t i = 0; i < n_neg; i++) {
        HU_ASSERT_EQ(ids_neg[i], ids_def[i]);
        HU_ASSERT_FLOAT_EQ(sc_neg[i], sc_def[i], 1e-6);
    }
    A()->free(A()->ctx, ids_neg, n_neg * sizeof(*ids_neg));
    A()->free(A()->ctx, sc_neg,  n_neg * sizeof(*sc_neg));
    A()->free(A()->ctx, ids_def, n_def * sizeof(*ids_def));
    A()->free(A()->ctx, sc_def,  n_def * sizeof(*sc_def));
    close_facade_(g, m);
}

#endif /* HU_ENABLE_SQLITE */

/* ── Test runner ────────────────────────────────────────────────────────── */

void run_w12_planner_tests(void) {
    HU_TEST_SUITE("W12 planner - goal-conditioned retrieval + HippoRAG PageRank");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_w12_heuristic_planner_emits_3_hop_plan_for_relationship_query);
    HU_RUN_TEST(test_w12_heuristic_planner_temporal_query_emits_window);
    HU_RUN_TEST(test_w12_heuristic_planner_default_plan_for_bare_goal);
    HU_RUN_TEST(test_w12_heuristic_planner_caps_steps_and_budget);
    HU_RUN_TEST(test_w12_planner_handles_empty_world_model_gracefully);
    HU_RUN_TEST(test_w12_adversarial_planner_resists_query_injection);
    HU_RUN_TEST(test_w12_planner_execute_aggregates_relation_records);
    HU_RUN_TEST(test_w12_planner_execute_respects_total_budget);
    HU_RUN_TEST(test_w12_planner_execute_invalid_args_rejected);
    HU_RUN_TEST(test_w12_llm_stub_returns_single_step_plan);
    HU_RUN_TEST(test_w12_pagerank_top_k_matches_expected_subgraph);
    HU_RUN_TEST(test_w12_pagerank_deterministic_repeated_calls);
    HU_RUN_TEST(test_w12_pagerank_zero_seeds_returns_empty);
    HU_RUN_TEST(test_w12_pagerank_unknown_seed_yields_empty);
    HU_RUN_TEST(test_w12_pagerank_invalid_args_rejected);
    HU_RUN_TEST(test_w12_pagerank_handles_empty_graph);
    HU_RUN_TEST(test_w12_pagerank_default_damping_when_out_of_range);
#endif
}
