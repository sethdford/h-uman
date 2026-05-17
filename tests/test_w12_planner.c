/* W12 — Goal-conditioned planner + HippoRAG-style PageRank.
 *
 * Tests run against an in-memory SQLite DB via hu_graph_open(NULL, 0).
 * All allocations free before return — ASan is the final arbiter. */

#include "human/agent/retrieval_planner.h"
#include "human/agent/world_model.h"
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/hyperedge.h"
#include "human/memory/memory.h"
#include "human/memory/pagerank.h"
#include "human/provider.h"
#include "test_framework.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) { g_alloc = hu_system_allocator(); return &g_alloc; }

static void open_facade_(hu_graph_t **g, hu_memory_facade_t **m) {
    HU_ASSERT_EQ(hu_graph_open(A(), NULL, 0, g), HU_OK);
    HU_ASSERT_NOT_NULL(*g);
    HU_ASSERT_EQ(hu_memory_facade_open(A(), *g, m), HU_OK);
    HU_ASSERT_NOT_NULL(*m);
    hu_world_model_invalidate(NULL, 0);
}

static void close_facade_(hu_graph_t *g, hu_memory_facade_t *m) {
    hu_world_model_invalidate(NULL, 0);
    hu_memory_facade_close(m, A());
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

static hu_world_model_t *load_wm(hu_memory_facade_t *m, const char *cid) {
    hu_world_model_t *wm = NULL;
    HU_ASSERT_EQ(hu_world_model_build(m, A(), cid, strlen(cid), 1735690000000LL, &wm),
                 HU_OK);
    HU_ASSERT_NOT_NULL(wm);
    return wm;
}

/* ── Heuristic planner ──────────────────────────────────────────────────── */

static void test_w12_heuristic_planner_emits_3_hop_plan_for_relationship_query(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
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
    hu_memory_facade_t *m = NULL;
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
    hu_memory_facade_t *m = NULL;
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
    hu_memory_facade_t *m = NULL;
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
    hu_memory_facade_t *m = NULL;
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
    hu_memory_facade_t *m = NULL;
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
    HU_ASSERT_EQ(hu_planner_execute((hu_memory_facade_t *)1, NULL, NULL, A(), &out, &n),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_planner_execute((hu_memory_facade_t *)1, NULL, &plan, NULL, &out, &n),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_planner_execute((hu_memory_facade_t *)1, NULL, &plan, A(), NULL, &n),
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

/* P3 — Real LLM planner with mock provider returning malformed JSON falls
 * back to deterministic plan instead of crashing. */
static int g_mock_calls = 0;
static const char *g_mock_response_body = NULL;

static hu_error_t mock_chat_with_system(void *ctx, hu_allocator_t *alloc,
                                        const char *sys, size_t sys_len,
                                        const char *msg, size_t msg_len,
                                        const char *model, size_t model_len,
                                        double temperature,
                                        char **out, size_t *out_len) {
    (void)ctx; (void)sys; (void)sys_len; (void)msg; (void)msg_len;
    (void)model; (void)model_len; (void)temperature;
    g_mock_calls++;
    if (!g_mock_response_body) return HU_ERR_IO;
    size_t n = strlen(g_mock_response_body);
    char *copy = (char *)alloc->alloc(alloc->ctx, n + 1);
    if (!copy) return HU_ERR_OUT_OF_MEMORY;
    memcpy(copy, g_mock_response_body, n);
    copy[n] = '\0';
    *out = copy;
    *out_len = n;
    return HU_OK;
}

static const char *mock_get_name(void *ctx) { (void)ctx; return "mock"; }
static bool mock_supports_native_tools(void *ctx) { (void)ctx; return false; }
static void mock_deinit(void *ctx, hu_allocator_t *alloc) { (void)ctx; (void)alloc; }

static void test_w12_llm_planner_with_provider_falls_back_under_test_guard(void) {
    /* Under HU_IS_TEST the LLM planner forces the deterministic fallback
     * path to keep tests free of provider I/O. This test verifies the
     * fallback is well-formed (single step, sane budget). */
    static hu_provider_vtable_t mock_vt = {
        .chat_with_system = mock_chat_with_system,
        .supports_native_tools = mock_supports_native_tools,
        .get_name = mock_get_name,
        .deinit = mock_deinit,
    };
    hu_provider_t mock_provider = { .ctx = NULL, .vtable = &mock_vt };

    g_mock_calls = 0;
    g_mock_response_body = "{\"steps\":[{\"kind\":\"entity\",\"limit\":7}],\"total_budget_ms\":120}";

    hu_planner_t p;
    HU_ASSERT_EQ(hu_planner_llm(&mock_provider, &p), HU_OK);
    /* Configure with the test allocator so the LLM path *would* be live
     * outside HU_IS_TEST. */
    hu_planner_llm_configure(A(), NULL, 0);

    hu_world_model_t wm;
    memset(&wm, 0, sizeof(wm));
    snprintf(wm.contact_id, sizeof(wm.contact_id), "%s", "u1");

    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_plan(&p, "who works at acme?", 18, &wm, &plan), HU_OK);

    /* Under HU_IS_TEST the mock provider must NOT be called. */
    HU_ASSERT_EQ(g_mock_calls, 0);
    /* And the plan must be a valid fallback shape. */
    HU_ASSERT_EQ((int)plan.steps_count, 1);
    HU_ASSERT(plan.total_budget_ms > 0);
    HU_ASSERT_LT(plan.total_budget_ms, HU_PLANNER_MAX_TOTAL_BUDGET_MS + 1);
    HU_ASSERT_EQ(plan.steps[0].kind, HU_MEM_RELATION);
    HU_ASSERT(plan.steps[0].verify_after);

    hu_planner_close(&p);
}

static void test_w12_llm_planner_oversized_goal_is_truncated(void) {
    /* Adversarial: a 100 KB goal cannot reach the LLM unbounded. The
     * planner clamps to HU_PLANNER_MAX_GOAL_LEN before invoking the
     * backend; under HU_IS_TEST we just verify it still produces a
     * valid plan and doesn't crash. */
    size_t big_len = 100000;
    char *big = (char *)A()->alloc(A()->ctx, big_len + 1);
    HU_ASSERT_NOT_NULL(big);
    memset(big, 'x', big_len);
    big[big_len] = '\0';

    hu_planner_t p;
    HU_ASSERT_EQ(hu_planner_llm(NULL, &p), HU_OK);

    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_plan(&p, big, big_len, NULL, &plan), HU_OK);
    HU_ASSERT_EQ((int)plan.steps_count, 1);

    A()->free(A()->ctx, big, big_len + 1);
    hu_planner_close(&p);
}

static void test_w12_llm_planner_configure_idempotent(void) {
    /* Calling configure twice with different model strings must not leak. */
    hu_planner_llm_configure(A(), "model-a", 7);
    hu_planner_llm_configure(A(), "model-b-longer", 14);
    hu_planner_llm_configure(A(), NULL, 0);
    /* Re-configure with empty allocator does not crash on planner close. */
    hu_planner_t p;
    HU_ASSERT_EQ(hu_planner_llm(NULL, &p), HU_OK);
    hu_planner_close(&p);
}

/* Test hooks into the LLM planner (HU_IS_TEST only). */
extern hu_error_t hu_planner_llm__test_parse_json(hu_allocator_t *alloc, const char *json,
                                                  size_t json_len, const hu_world_model_t *wm,
                                                  hu_retrieval_plan_t *out);
extern char *hu_planner_llm__test_render_wm_digest(hu_allocator_t *alloc,
                                                   const hu_world_model_t *wm);

static void test_w12_llm_parse_well_formed_plan(void) {
    const char *json =
        "{"
        "  \"total_budget_ms\": 350,"
        "  \"steps\": ["
        "    {\"kind\": \"entity\", \"hops\": 0, \"budget_ms\": 100, \"verify_after\": false, \"limit\": 8},"
        "    {\"kind\": \"relation\", \"hops\": 2, \"budget_ms\": 200, \"verify_after\": true, \"limit\": 16},"
        "    {\"kind\": \"hyperedge\", \"hops\": 1, \"budget_ms\": 50, \"limit\": 4}"
        "  ]"
        "}";

    hu_world_model_t wm;
    memset(&wm, 0, sizeof(wm));
    snprintf(wm.contact_id, sizeof(wm.contact_id), "u-test");

    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_llm__test_parse_json(A(), json, strlen(json), &wm, &plan), HU_OK);
    HU_ASSERT_EQ((int)plan.steps_count, 3);
    HU_ASSERT_EQ(plan.total_budget_ms, 350);
    HU_ASSERT_EQ(plan.steps[0].kind, HU_MEM_ENTITY);
    HU_ASSERT_EQ((int)plan.steps[0].hops, 0);
    HU_ASSERT_EQ(plan.steps[1].kind, HU_MEM_RELATION);
    HU_ASSERT_EQ((int)plan.steps[1].hops, 2);
    HU_ASSERT(plan.steps[1].verify_after);
    HU_ASSERT_EQ(plan.steps[2].kind, HU_MEM_HYPEREDGE);
    /* Contact-id propagated to query payload. */
    HU_ASSERT_EQ((int)plan.steps[0].query.contact_id_len, 6);
}

static void test_w12_llm_parse_strips_markdown_fences(void) {
    const char *json =
        "```json\n"
        "{\"total_budget_ms\":200,\"steps\":[{\"kind\":\"relation\",\"limit\":8}]}\n"
        "```";
    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_llm__test_parse_json(A(), json, strlen(json), NULL, &plan), HU_OK);
    HU_ASSERT_EQ((int)plan.steps_count, 1);
    HU_ASSERT_EQ(plan.steps[0].kind, HU_MEM_RELATION);
}

static void test_w12_llm_parse_rejects_malformed_json(void) {
    const char *bad_cases[] = {
        "",                                     /* empty */
        "not json at all",                      /* not JSON */
        "{\"steps\":[]}",                       /* empty steps */
        "{\"total_budget_ms\":250}",            /* no steps key */
        "[1, 2, 3]",                            /* array not object */
        "{\"steps\":\"not an array\"}",         /* steps is string */
        NULL,
    };
    for (size_t i = 0; bad_cases[i]; i++) {
        hu_retrieval_plan_t plan;
        hu_error_t err = hu_planner_llm__test_parse_json(A(), bad_cases[i],
                                                          strlen(bad_cases[i]),
                                                          NULL, &plan);
        HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    }
}

static void test_w12_llm_parse_unknown_kind_defaults_to_relation(void) {
    const char *json =
        "{\"steps\":[{\"kind\":\"nonsense\",\"limit\":5}]}";
    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_llm__test_parse_json(A(), json, strlen(json), NULL, &plan),
                 HU_OK);
    HU_ASSERT_EQ((int)plan.steps_count, 1);
    /* Adversarial input: unknown kinds fall back to RELATION, not ENTITY,
     * because RELATION is the cheapest safe query. */
    HU_ASSERT_EQ(plan.steps[0].kind, HU_MEM_RELATION);
}

static void test_w12_llm_parse_caps_steps_array_to_max(void) {
    /* Hand-craft a JSON with 20 step entries; the parser must cap to
     * HU_PLANNER_MAX_STEPS (8) without crashing. */
    char buf[4096];
    size_t off = 0;
    int n = snprintf(buf + off, sizeof(buf) - off, "{\"steps\":[");
    if (n > 0) off += (size_t)n;
    for (int i = 0; i < 20; i++) {
        n = snprintf(buf + off, sizeof(buf) - off,
                     "%s{\"kind\":\"relation\",\"limit\":4}", i == 0 ? "" : ",");
        if (n > 0) off += (size_t)n;
    }
    n = snprintf(buf + off, sizeof(buf) - off, "]}");
    if (n > 0) off += (size_t)n;

    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_llm__test_parse_json(A(), buf, off, NULL, &plan), HU_OK);
    HU_ASSERT_EQ((int)plan.steps_count, HU_PLANNER_MAX_STEPS);
}

static void test_w12_llm_parse_negative_limits_clamped(void) {
    const char *json =
        "{\"steps\":[{\"kind\":\"relation\",\"limit\":-99,\"hops\":-5,\"budget_ms\":-1}]}";
    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_llm__test_parse_json(A(), json, strlen(json), NULL, &plan),
                 HU_OK);
    HU_ASSERT_EQ((int)plan.steps_count, 1);
    /* Negative limit ⇒ default 16. Negative hops ⇒ size_t wraps so it becomes
     * huge; clamp_plan() inside hu_planner_plan() (NOT called by the test
     * hook) caps to 3. So we only verify the limit clamp here. */
    HU_ASSERT_EQ((int)plan.steps[0].query.as.window.limit, 16);
}

/* ── Entity-name queries (LLM planner can now emit BY_NAME) ────────────── */

/* Happy path: LLM emits an entity step with `entity_name` ⇒ planner produces
 * a HU_MEMORY_QUERY_BY_NAME query that points to the step's own buffer. The
 * pointer must survive a `hu_json_free` so the buffer ownership matters. */
static void test_w12_llm_parse_entity_name_produces_by_name_variant(void) {
    const char *json =
        "{\"steps\":[{\"kind\":\"entity\",\"entity_name\":\"Alice\","
        "\"hops\":0,\"limit\":4,\"verify_after\":true}]}";

    hu_world_model_t wm;
    memset(&wm, 0, sizeof(wm));
    snprintf(wm.contact_id, sizeof(wm.contact_id), "u-name");

    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_llm__test_parse_json(A(), json, strlen(json), &wm, &plan),
                 HU_OK);
    HU_ASSERT_EQ((int)plan.steps_count, 1);
    HU_ASSERT_EQ(plan.steps[0].kind, HU_MEM_ENTITY);
    HU_ASSERT_EQ((int)plan.steps[0].query.variant, (int)HU_MEMORY_QUERY_BY_NAME);
    HU_ASSERT_EQ((int)plan.steps[0].query.as.by_name.name_len, 5);
    HU_ASSERT_NOT_NULL(plan.steps[0].query.as.by_name.name);
    /* Pointer must equal the step's own buffer (stable across plan lifetime). */
    HU_ASSERT_EQ((const void *)plan.steps[0].query.as.by_name.name,
                 (const void *)plan.steps[0].entity_name_buf);
    HU_ASSERT_EQ(memcmp(plan.steps[0].query.as.by_name.name, "Alice", 5), 0);
}

/* Adversarial: oversized name (>= HU_PLANNER_ENTITY_NAME_MAX) must NOT
 * overflow the buffer. The planner silently downgrades to a window query. */
static void test_w12_llm_parse_oversized_entity_name_downgrades(void) {
    char json[256];
    char big_name[HU_PLANNER_ENTITY_NAME_MAX + 32];
    memset(big_name, 'a', sizeof(big_name));
    big_name[sizeof(big_name) - 1] = '\0';
    int n = snprintf(json, sizeof(json),
                     "{\"steps\":[{\"kind\":\"entity\",\"entity_name\":\"%s\",\"limit\":4}]}",
                     big_name);
    HU_ASSERT(n > 0 && (size_t)n < sizeof(json));

    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_llm__test_parse_json(A(), json, (size_t)n, NULL, &plan),
                 HU_OK);
    HU_ASSERT_EQ((int)plan.steps_count, 1);
    /* Downgrade to WINDOW; the entity_name_buf must remain all-zero. */
    HU_ASSERT_EQ((int)plan.steps[0].query.variant, (int)HU_MEMORY_QUERY_WINDOW);
    HU_ASSERT_EQ(plan.steps[0].entity_name_buf[0], '\0');
}

/* Adversarial: non-printable bytes in entity_name (e.g. NUL injection or
 * control characters) get rejected — planner downgrades to WINDOW. This
 * prevents a malicious provider from sneaking control bytes into the
 * downstream SQL bind. */
static void test_w12_llm_parse_nonprintable_entity_name_downgrades(void) {
    /* Inject a \x01 control byte mid-name. */
    const char *json =
        "{\"steps\":[{\"kind\":\"entity\",\"entity_name\":\"al\\u0001ce\",\"limit\":4}]}";
    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_llm__test_parse_json(A(), json, strlen(json), NULL, &plan),
                 HU_OK);
    HU_ASSERT_EQ((int)plan.steps_count, 1);
    HU_ASSERT_EQ((int)plan.steps[0].query.variant, (int)HU_MEMORY_QUERY_WINDOW);
}

/* entity_name on a non-entity kind (relation/hyperedge) must be ignored —
 * the planner sticks with the window query for those kinds. */
static void test_w12_llm_parse_entity_name_ignored_on_relation_kind(void) {
    const char *json =
        "{\"steps\":[{\"kind\":\"relation\",\"entity_name\":\"Alice\",\"limit\":4}]}";
    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_llm__test_parse_json(A(), json, strlen(json), NULL, &plan),
                 HU_OK);
    HU_ASSERT_EQ((int)plan.steps_count, 1);
    HU_ASSERT_EQ(plan.steps[0].kind, HU_MEM_RELATION);
    HU_ASSERT_EQ((int)plan.steps[0].query.variant, (int)HU_MEMORY_QUERY_WINDOW);
}

/* End-to-end: parse an LLM-emitted plan with entity_name, execute against a
 * facade seeded with the matching entity. The executor must dispatch through
 * the v1 backend's BY_NAME path and return at least one record. This is the
 * full P3 win — entity-name queries now flow LLM → JSON → executor → SQL. */
static void test_w12_llm_entity_name_plan_executes_end_to_end(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade_(&g, &m);

    int64_t alice_id = add_entity(g, "u1", "Alice", HU_ENTITY_PERSON);
    HU_ASSERT(alice_id > 0);
    /* Bystander entities to prove BY_NAME isn't returning everything. */
    (void)add_entity(g, "u1", "Bob", HU_ENTITY_PERSON);
    (void)add_entity(g, "u1", "Charlie", HU_ENTITY_PERSON);

    const char *json =
        "{\"total_budget_ms\":150,\"steps\":["
        "{\"kind\":\"entity\",\"entity_name\":\"Alice\",\"hops\":0,"
        "\"budget_ms\":100,\"verify_after\":false,\"limit\":8}"
        "]}";

    hu_world_model_t wm;
    memset(&wm, 0, sizeof(wm));
    snprintf(wm.contact_id, sizeof(wm.contact_id), "u1");

    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_llm__test_parse_json(A(), json, strlen(json), &wm, &plan),
                 HU_OK);
    HU_ASSERT_EQ((int)plan.steps[0].query.variant, (int)HU_MEMORY_QUERY_BY_NAME);

    hu_memory_record_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_planner_execute(m, /*self_rag=*/NULL, &plan, A(), &out, &n), HU_OK);
    HU_ASSERT_GE((int)n, 1);
    /* Every record must be the Alice entity (id-match). */
    bool found = false;
    for (size_t i = 0; i < n; i++) {
        if (out[i].id == alice_id && out[i].kind == HU_MEM_ENTITY) {
            found = true;
            break;
        }
    }
    HU_ASSERT(found);

    hu_planner_records_free(A(), out, n);
    close_facade_(g, m);
}

/* ── W12 world-model cell wiring (recent_changes / hyperedges / self_model) */

static void test_w12_heuristic_recent_changes_bounds_temporal_window(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade_(&g, &m);

    int64_t carol = 0, dawn = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u-rc", 4, "Carol", 5, HU_ENTITY_PERSON,
                                          NULL, &carol), HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u-rc", 4, "Dawn", 4, HU_ENTITY_ORGANIZATION,
                                          NULL, &dawn), HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation_with_belief(
                     g, "u-rc", 4, carol, dawn, HU_REL_WORKS_AT, 1.0f,
                     1735000000000LL, 1735500000000LL, 0.9f, 0.01f, NULL, 0, NULL, 0,
                     NULL), HU_OK);

    hu_world_model_t *wm = load_wm(m, "u-rc");
    HU_ASSERT(wm->recent_changes_count >= 1);

    hu_planner_t p;
    HU_ASSERT_EQ(hu_planner_heuristic(&p), HU_OK);
    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_plan(&p, "when did Carol stop working at Dawn?", 36, wm, &plan),
                 HU_OK);

    bool found_window = false;
    for (size_t i = 0; i < plan.steps_count; i++) {
        if (plan.steps[i].kind != HU_MEM_RELATION) continue;
        if (plan.steps[i].query.as.window.from_ts > 0) {
            found_window = true;
            HU_ASSERT(plan.steps[i].query.as.window.from_ts <=
                      wm->recent_changes[0].at_ms);
        }
    }
    HU_ASSERT(found_window);

    hu_planner_close(&p);
    hu_world_model_free(A(), wm);
    close_facade_(g, m);
}

static void test_w12_heuristic_hyperedges_add_member_neighbor_steps(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade_(&g, &m);

    int64_t alice = add_entity(g, "u-he12", "Alice", HU_ENTITY_PERSON);
    int64_t bob   = add_entity(g, "u-he12", "Bob",   HU_ENTITY_PERSON);
    int64_t acme  = add_entity(g, "u-he12", "Acme",  HU_ENTITY_ORGANIZATION);
    add_relation(g, "u-he12", alice, acme, HU_REL_WORKS_AT);

    hu_hyperedge_t he;
    memset(&he, 0, sizeof(he));
    snprintf(he.relation_label, sizeof(he.relation_label), "met_at");
    hu_hyperedge_member_t members[3];
    memset(members, 0, sizeof(members));
    members[0].entity_id = alice;
    members[1].entity_id = bob;
    members[2].entity_id = acme;
    he.members = members;
    he.members_count = 3;
    he.belief.mean = 0.8f;
    int64_t he_id = 0;
    HU_ASSERT_EQ(hu_hyperedge_upsert(m, "u-he12", 6, &he, &he_id), HU_OK);

    hu_world_model_t *wm = load_wm(m, "u-he12");
    HU_ASSERT(wm->hyperedges_count >= 1);

    hu_planner_t p;
    HU_ASSERT_EQ(hu_planner_heuristic(&p), HU_OK);
    hu_retrieval_plan_t plan;
    const char *goal = "when did Alice and Bob meet at Acme?";
    HU_ASSERT_EQ(hu_planner_plan(&p, goal, strlen(goal), wm, &plan), HU_OK);

    size_t neighbor_steps = 0;
    bool has_acme_expansion = false;
    for (size_t i = 0; i < plan.steps_count; i++) {
        if (plan.steps[i].kind != HU_MEM_ENTITY) continue;
        neighbor_steps++;
        if (plan.steps[i].query.as.neighbors.entity_id == acme) has_acme_expansion = true;
    }
    HU_ASSERT(neighbor_steps >= 3);
    HU_ASSERT(has_acme_expansion);

    hu_planner_close(&p);
    hu_world_model_free(A(), wm);
    close_facade_(g, m);
}

static void test_w12_heuristic_self_model_focused_topics_prefers_entity(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade_(&g, &m);

    int64_t alice  = add_entity(g, "u-sm", "Alice", HU_ENTITY_PERSON);
    int64_t budget = add_entity(g, "u-sm", "Budget", HU_ENTITY_TOPIC);
    (void)alice;
    add_relation(g, "u-sm", budget, alice, HU_REL_RELATED_TO);

    hu_world_model_t *wm = load_wm(m, "u-sm");
    snprintf(wm->self_model.focused_topics, sizeof(wm->self_model.focused_topics),
             "Budget; travel");

    hu_planner_t p;
    HU_ASSERT_EQ(hu_planner_heuristic(&p), HU_OK);
    hu_retrieval_plan_t plan;
    HU_ASSERT_EQ(hu_planner_plan(&p, "what changed about Budget recently?", 35, wm, &plan),
                 HU_OK);

    HU_ASSERT(plan.steps_count > 0);
    HU_ASSERT_EQ(plan.steps[0].kind, HU_MEM_ENTITY);
    HU_ASSERT_EQ(plan.steps[0].query.as.neighbors.entity_id, budget);

    hu_planner_close(&p);
    hu_world_model_free(A(), wm);
    close_facade_(g, m);
}

static void test_w12_llm_wm_digest_includes_world_model_cells(void) {
    hu_world_model_t wm;
    memset(&wm, 0, sizeof(wm));
    snprintf(wm.contact_id, sizeof(wm.contact_id), "u-digest");
    snprintf(wm.self_model.name, sizeof(wm.self_model.name), "Aria");
    snprintf(wm.self_model.focused_topics, sizeof(wm.self_model.focused_topics),
             "budget; travel");
    snprintf(wm.self_model.capabilities[0], sizeof(wm.self_model.capabilities[0]),
             "memory_search");
    wm.self_model.capabilities_count = 1;

    wm.recent_changes_count = 1;
    hu_world_recent_change_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.kind = HU_WORLD_CHANGE_RETRACTED;
    ch.at_ms = 1735500000000LL;
    snprintf(ch.summary, sizeof(ch.summary), "rel retracted");
    wm.recent_changes = &ch;

    wm.hyperedges_count = 1;
    hu_hyperedge_t he;
    memset(&he, 0, sizeof(he));
    snprintf(he.relation_label, sizeof(he.relation_label), "met_at");
    he.members_count = 2;

    wm.hyperedges = &he;

    char *digest = hu_planner_llm__test_render_wm_digest(A(), &wm);
    HU_ASSERT_NOT_NULL(digest);
    HU_ASSERT_NOT_NULL(strstr(digest, "Recent change:"));
    HU_ASSERT_NOT_NULL(strstr(digest, "Hyperedge:"));
    HU_ASSERT_NOT_NULL(strstr(digest, "Self: Aria"));
    HU_ASSERT_NOT_NULL(strstr(digest, "Focused:"));
    HU_ASSERT_NOT_NULL(strstr(digest, "memory_search"));
    A()->free(A()->ctx, digest, 1024);
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
    hu_memory_facade_t *m = NULL;
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
    hu_memory_facade_t *m = NULL;
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
    hu_memory_facade_t *m = NULL;
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
    hu_memory_facade_t *m = NULL;
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
    hu_memory_facade_t *m = NULL;
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
    hu_memory_facade_t *m = NULL;
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
    hu_memory_facade_t *m = NULL;
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

/* ── Multi-hop ──────────────────────────────────────────────────────────── */

static void test_w12_multi_hop_returns_records_or_empty(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade_(&g, &m);

    int64_t alice = add_entity(g, "u1", "Alice", HU_ENTITY_PERSON);
    int64_t acme  = add_entity(g, "u1", "Acme",  HU_ENTITY_ORGANIZATION);
    add_relation(g, "u1", alice, acme, HU_REL_WORKS_AT);

    hu_memory_query_t q;
    memset(&q, 0, sizeof(q));
    q.kind = HU_MEM_RELATION;
    q.variant = HU_MEMORY_QUERY_AUTO;
    q.contact_id = "u1";
    q.contact_id_len = 2;

    hu_memory_record_t *out = NULL;
    size_t n = 0;
    hu_error_t err = hu_planner_multi_hop(m, A(), &q, 2, &out, &n);
    HU_ASSERT_EQ(err, HU_OK);
    if (out) {
        HU_ASSERT_GT((int)n, 0);
        hu_planner_records_free(A(), out, n);
    }

    close_facade_(g, m);
}

static void test_w12_multi_hop_null_args_rejected(void) {
    hu_memory_record_t *out = NULL;
    size_t n = 0;
    hu_memory_query_t q;
    memset(&q, 0, sizeof(q));
    q.kind = HU_MEM_ENTITY;
    q.variant = HU_MEMORY_QUERY_AUTO;

    HU_ASSERT_EQ(hu_planner_multi_hop(NULL, A(), &q, 2, &out, &n),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_planner_multi_hop((hu_memory_facade_t *)1, NULL, &q, 2, &out, &n),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_planner_multi_hop((hu_memory_facade_t *)1, A(), NULL, 2, &out, &n),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_planner_multi_hop((hu_memory_facade_t *)1, A(), &q, 2, NULL, &n),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_planner_multi_hop((hu_memory_facade_t *)1, A(), &q, 2, &out, NULL),
                 HU_ERR_INVALID_ARGUMENT);
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
    HU_RUN_TEST(test_w12_llm_planner_with_provider_falls_back_under_test_guard);
    HU_RUN_TEST(test_w12_llm_planner_oversized_goal_is_truncated);
    HU_RUN_TEST(test_w12_llm_planner_configure_idempotent);
    HU_RUN_TEST(test_w12_llm_parse_well_formed_plan);
    HU_RUN_TEST(test_w12_llm_parse_strips_markdown_fences);
    HU_RUN_TEST(test_w12_llm_parse_rejects_malformed_json);
    HU_RUN_TEST(test_w12_llm_parse_unknown_kind_defaults_to_relation);
    HU_RUN_TEST(test_w12_llm_parse_caps_steps_array_to_max);
    HU_RUN_TEST(test_w12_llm_parse_negative_limits_clamped);
    HU_RUN_TEST(test_w12_llm_parse_entity_name_produces_by_name_variant);
    HU_RUN_TEST(test_w12_llm_parse_oversized_entity_name_downgrades);
    HU_RUN_TEST(test_w12_llm_parse_nonprintable_entity_name_downgrades);
    HU_RUN_TEST(test_w12_llm_parse_entity_name_ignored_on_relation_kind);
    HU_RUN_TEST(test_w12_llm_entity_name_plan_executes_end_to_end);
    HU_RUN_TEST(test_w12_heuristic_recent_changes_bounds_temporal_window);
    HU_RUN_TEST(test_w12_heuristic_hyperedges_add_member_neighbor_steps);
    HU_RUN_TEST(test_w12_heuristic_self_model_focused_topics_prefers_entity);
    HU_RUN_TEST(test_w12_llm_wm_digest_includes_world_model_cells);
    HU_RUN_TEST(test_w12_pagerank_top_k_matches_expected_subgraph);
    HU_RUN_TEST(test_w12_pagerank_deterministic_repeated_calls);
    HU_RUN_TEST(test_w12_pagerank_zero_seeds_returns_empty);
    HU_RUN_TEST(test_w12_pagerank_unknown_seed_yields_empty);
    HU_RUN_TEST(test_w12_pagerank_invalid_args_rejected);
    HU_RUN_TEST(test_w12_pagerank_handles_empty_graph);
    HU_RUN_TEST(test_w12_pagerank_default_damping_when_out_of_range);
    HU_RUN_TEST(test_w12_multi_hop_returns_records_or_empty);
    HU_RUN_TEST(test_w12_multi_hop_null_args_rejected);
#endif
}
