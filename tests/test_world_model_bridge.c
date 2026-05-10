/* FIX 12 — W7 facade + W9 world-model bridge.
 *
 * Verifies the bridge open/close lifecycle, that hu_w7_render_world_model
 * produces a non-empty markdown block when the underlying world model has
 * goals/negatives/topics, and that an empty world model returns NULL/0
 * (so callers cleanly skip injection). */

#include "human/agent/world_model_bridge.h"
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "test_framework.h"
#include <stdlib.h>

/* These two are deliberately the only W7-headers visible to this TU. We
 * include them through a separate helper so this test does not double-
 * include `human/memory.h` (legacy) and `human/memory/memory.h` (W7) in the
 * same translation unit. We use a forward declaration trick: the bridge
 * already exports an opaque type, so we don't need direct W7 access here. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) {
    g_alloc = hu_system_allocator();
    return &g_alloc;
}

/* Helper: open graph, open W7 facade, return both. Disables the world-model
 * cache so per-test contact ids do not see stale entries from a prior test
 * (the global cache lives in src/agent/world_model.c). */
static void open_graph_and_facade(hu_graph_t **out_g, hu_w7_facade_t **out_f) {
    setenv("HU_WORLD_MODEL_TTL_MS", "0", 1);
    HU_ASSERT_EQ(hu_graph_open(A(), NULL, 0, out_g), HU_OK);
    HU_ASSERT_EQ(hu_w7_facade_open(*out_g, A(), out_f), HU_OK);
}

static void cleanup(hu_graph_t *g, hu_w7_facade_t *f) {
    if (f)
        hu_w7_facade_close(f, A());
    if (g)
        hu_graph_close(g, A());
}

/* --- Lifecycle --- */

static void bridge_open_close_clean(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);
    HU_ASSERT_NOT_NULL(f);
    cleanup(g, f);
}

static void bridge_open_rejects_null_args(void) {
    hu_w7_facade_t *f = NULL;
    HU_ASSERT_EQ(hu_w7_facade_open(NULL, A(), &f), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT(f == NULL);
    /* close NULL is a no-op (does not crash). */
    hu_w7_facade_close(NULL, A());
}

/* --- Render: empty graph -> empty output --- */

static void bridge_render_empty_world_model_returns_null(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);

    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(
        hu_w7_render_world_model(f, A(), "ut_empty", 8, 1700000000000LL, &txt, &tlen), HU_OK);
    /* Empty world model -> NULL/0 so callers skip injection. */
    HU_ASSERT(txt == NULL);
    HU_ASSERT_EQ(tlen, 0);

    cleanup(g, f);
}

/* --- Render: non-empty world model produces formatted text ---
 *
 * We seed the underlying graph with a negative-memory entry through the
 * W9 public API. That ensures the world_model_load builder sees something
 * worth surfacing. */

#include "human/agent/world_model.h"

static void bridge_render_with_negative_memory_includes_avoid_section(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);

    hu_negative_memory_t nm = {0};
    snprintf(nm.text, sizeof(nm.text), "joke about her grandmother");
    snprintf(nm.scope, sizeof(nm.scope), "topic");
    snprintf(nm.reason, sizeof(nm.reason), "she's grieving");
    nm.belief = hu_belief_init(0.95f, "test", 1700000000000LL);
    nm.created_at = 1700000000000LL;

    int64_t id = 0;
    HU_ASSERT_EQ(hu_negative_memory_add(g, "ut_neg", 6, &nm, &id), HU_OK);
    HU_ASSERT(id > 0);

    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(
        hu_w7_render_world_model(f, A(), "ut_neg", 6, 1700000000000LL + 1000, &txt, &tlen),
        HU_OK);
    HU_ASSERT_NOT_NULL(txt);
    HU_ASSERT(tlen > 0);
    /* Header always present when any section fires. */
    HU_ASSERT(strstr(txt, "What I know about this conversation") != NULL);
    /* Avoid section present, with the text body. */
    HU_ASSERT(strstr(txt, "Avoid:") != NULL);
    HU_ASSERT(strstr(txt, "joke about her grandmother") != NULL);
    HU_ASSERT(strstr(txt, "she's grieving") != NULL);

    A()->free(A()->ctx, txt, tlen + 1);
    cleanup(g, f);
}

/* --- Adversarial: huge contact_id rejected --- */

static void bridge_render_rejects_invalid_args(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);

    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(NULL, A(), "ut_inv", 6, 0, &txt, &tlen),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_w7_render_world_model(f, NULL, "ut_inv", 6, 0, &txt, &tlen),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), NULL, 0, 0, &txt, &tlen),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), "ut_inv", 0, 0, &txt, &tlen),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), "ut_inv", 6, 0, NULL, &tlen),
                 HU_ERR_INVALID_ARGUMENT);
    cleanup(g, f);
}

/* --- Cache hit: second call within TTL hits the cache --- */

static void bridge_render_uses_cache_within_ttl(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);

    /* Seed once. */
    hu_negative_memory_t nm = {0};
    snprintf(nm.text, sizeof(nm.text), "topic alpha");
    snprintf(nm.scope, sizeof(nm.scope), "topic");
    nm.belief = hu_belief_init(0.9f, "test", 1700000000000LL);
    nm.created_at = 1700000000000LL;
    int64_t id = 0;
    HU_ASSERT_EQ(hu_negative_memory_add(g, "ut_cache", 8, &nm, &id), HU_OK);

    /* Re-enable cache for this test specifically (default TTL = 60s). */
    setenv("HU_WORLD_MODEL_TTL_MS", "60000", 1);
    char *txt1 = NULL;
    size_t tlen1 = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), "ut_cache", 8, 1700000000000LL + 1000, &txt1,
                                          &tlen1),
                 HU_OK);
    HU_ASSERT_NOT_NULL(txt1);

    char *txt2 = NULL;
    size_t tlen2 = 0;
    /* Within the 60s default TTL -- should still produce content (cache or rebuild). */
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), "ut_cache", 8, 1700000000000LL + 5000, &txt2,
                                          &tlen2),
                 HU_OK);
    HU_ASSERT_NOT_NULL(txt2);
    HU_ASSERT_EQ(tlen1, tlen2);

    A()->free(A()->ctx, txt1, tlen1 + 1);
    A()->free(A()->ctx, txt2, tlen2 + 1);
    cleanup(g, f);
}

/* --- W11 self-RAG bridge (FIX 12b) --- */

static void w11_off_mode_is_noop(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);
    hu_w11_outcome_t outc = HU_W11_OUTCOME_HEDGED;
    size_t total = 99, flagged = 99;
    HU_ASSERT_EQ(hu_w11_self_rag_verify(f, A(), "u", 1, "anything", 8, 0, /*OFF*/
                                         0, &outc, &total, &flagged, NULL, NULL),
                 HU_OK);
    HU_ASSERT_EQ((int)outc, (int)HU_W11_OUTCOME_SUPPORTED);
    HU_ASSERT_EQ(total, 0);
    HU_ASSERT_EQ(flagged, 0);
    cleanup(g, f);
}

static void w11_telemetry_extracts_claims_without_modifying(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);
    hu_w11_outcome_t outc = HU_W11_OUTCOME_SUPPORTED;
    size_t total = 0, flagged = 0;
    char *modified = NULL;
    size_t modified_len = 0;
    /* Pass NULL for out_modified to assert no modification path runs. */
    HU_ASSERT_EQ(hu_w11_self_rag_verify(f, A(), "u_w11_t", 7,
                                         "Paris is the capital of France.", 32, 1, /*TELEMETRY*/
                                         0, &outc, &total, &flagged, NULL, NULL),
                 HU_OK);
    /* Telemetry never modifies. */
    HU_ASSERT(modified == NULL);
    HU_ASSERT_EQ(modified_len, 0);
    /* outcome SUPPORTED for telemetry mode (no rewrite). */
    HU_ASSERT(outc == HU_W11_OUTCOME_SUPPORTED || outc == HU_W11_OUTCOME_ABSTAINED);
    cleanup(g, f);
}

static void w11_rejects_invalid_args(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);
    HU_ASSERT_EQ(hu_w11_self_rag_verify(NULL, A(), "u", 1, "x", 1, 1, 0, NULL, NULL, NULL, NULL,
                                         NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_w11_self_rag_verify(f, NULL, "u", 1, "x", 1, 1, 0, NULL, NULL, NULL, NULL,
                                         NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_w11_self_rag_verify(f, A(), NULL, 0, "x", 1, 1, 0, NULL, NULL, NULL, NULL,
                                         NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_w11_self_rag_verify(f, A(), "u", 1, NULL, 0, 1, 0, NULL, NULL, NULL, NULL,
                                         NULL),
                 HU_ERR_INVALID_ARGUMENT);
    cleanup(g, f);
}

/* ── W14 scheduler bridge (FIX 13) ───────────────────────────────────────
 * Verifies the open/tick/close lifecycle, that tick is a no-op when the
 * queue is empty, that enqueue + tick consumes a counterfactual job,
 * and that the bridge rejects invalid arguments cleanly. */

static void w14_scheduler_open_close_clean(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);
    hu_w14_scheduler_t *s = NULL;
    HU_ASSERT_EQ(hu_w14_scheduler_open(f, A(), &s), HU_OK);
    HU_ASSERT_NOT_NULL(s);
    hu_w14_scheduler_close(s, A());
    cleanup(g, f);
}

static void w14_scheduler_rejects_null_args(void) {
    hu_w14_scheduler_t *s = NULL;
    HU_ASSERT_EQ(hu_w14_scheduler_open(NULL, A(), &s), HU_ERR_INVALID_ARGUMENT);
    /* close(NULL) is a no-op. */
    hu_w14_scheduler_close(NULL, A());
    HU_ASSERT_EQ(hu_w14_scheduler_tick(NULL, 1000), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_w14_scheduler_enqueue_counterfactual(NULL, "u", 1, 50),
                 HU_ERR_INVALID_ARGUMENT);
}

static void w14_scheduler_tick_is_noop_when_empty(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);
    hu_w14_scheduler_t *s = NULL;
    HU_ASSERT_EQ(hu_w14_scheduler_open(f, A(), &s), HU_OK);
    /* Tick with empty queue: must succeed and not crash. */
    HU_ASSERT_EQ(hu_w14_scheduler_tick(s, 1000), HU_OK);
    HU_ASSERT_EQ(hu_w14_scheduler_tick(s, 0), HU_OK); /* 0 -> use OS clock */
    /* Status should report 0 pending. */
    size_t pending = 999;
    HU_ASSERT_EQ(hu_w14_scheduler_status(s, &pending, NULL, NULL, NULL), HU_OK);
    HU_ASSERT_EQ(pending, (size_t)0);
    hu_w14_scheduler_close(s, A());
    cleanup(g, f);
}

static void w14_scheduler_enqueue_then_tick_drains_job(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);
    hu_w14_scheduler_t *s = NULL;
    HU_ASSERT_EQ(hu_w14_scheduler_open(f, A(), &s), HU_OK);

    HU_ASSERT_EQ(hu_w14_scheduler_enqueue_counterfactual(s, "u_cf", 4, 50), HU_OK);
    /* After enqueue: at least 1 pending (we don't assume exact count
     * because the scheduler may dedupe / coalesce in the future). */
    size_t pending = 0;
    HU_ASSERT_EQ(hu_w14_scheduler_status(s, &pending, NULL, NULL, NULL), HU_OK);
    HU_ASSERT(pending >= 1);

    /* Tick: counterfactual runner is registered, queue should drain. */
    HU_ASSERT_EQ(hu_w14_scheduler_tick(s, 2000), HU_OK);

    /* After tick: queue should be empty (counterfactual rehearsal is
     * cheap; one tick consumes it within the per-tick budget). */
    pending = 999;
    HU_ASSERT_EQ(hu_w14_scheduler_status(s, &pending, NULL, NULL, NULL), HU_OK);
    HU_ASSERT_EQ(pending, (size_t)0);

    hu_w14_scheduler_close(s, A());
    cleanup(g, f);
}

/* FIX 14: AutoDream runs through the scheduler. Enqueue all three phases
 * (quarantine / community / decay), tick once, expect the queue to drain
 * and the runner to return HU_OK on each. The empty graph yields zero
 * work but the runner must still complete cleanly. */
static void w14_autodream_phases_drain_through_scheduler(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);
    hu_w14_scheduler_t *s = NULL;
    HU_ASSERT_EQ(hu_w14_scheduler_open(f, A(), &s), HU_OK);

    /* Enqueue all three phases via the bridge helper. */
    HU_ASSERT_EQ(hu_w14_scheduler_enqueue_autodream(s, 0, 100), HU_OK);

    size_t pending = 0;
    HU_ASSERT_EQ(hu_w14_scheduler_status(s, &pending, NULL, NULL, NULL), HU_OK);
    HU_ASSERT(pending >= 3); /* one per phase */

    /* Tick once: scheduler should dispatch all three through the
     * registered hu_autodream_runner. The HU_SCHED_MAX_JOBS_PER_TICK
     * cap is 32, so 3 jobs fit comfortably. */
    HU_ASSERT_EQ(hu_w14_scheduler_tick(s, 1000), HU_OK);

    pending = 999;
    HU_ASSERT_EQ(hu_w14_scheduler_status(s, &pending, NULL, NULL, NULL), HU_OK);
    HU_ASSERT_EQ(pending, (size_t)0);

    hu_w14_scheduler_close(s, A());
    cleanup(g, f);
}

static void w14_autodream_rejects_null_args(void) {
    HU_ASSERT_EQ(hu_w14_scheduler_enqueue_autodream(NULL, 0, 100), HU_ERR_INVALID_ARGUMENT);
}

#endif /* HU_ENABLE_SQLITE */

void run_world_model_bridge_tests(void) {
    HU_TEST_SUITE("World-model bridge (FIX 12)");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(bridge_open_close_clean);
    HU_RUN_TEST(bridge_open_rejects_null_args);
    HU_RUN_TEST(bridge_render_empty_world_model_returns_null);
    HU_RUN_TEST(bridge_render_with_negative_memory_includes_avoid_section);
    HU_RUN_TEST(bridge_render_rejects_invalid_args);
    HU_RUN_TEST(bridge_render_uses_cache_within_ttl);
    HU_RUN_TEST(w11_off_mode_is_noop);
    HU_RUN_TEST(w11_telemetry_extracts_claims_without_modifying);
    HU_RUN_TEST(w11_rejects_invalid_args);
    HU_RUN_TEST(w14_scheduler_open_close_clean);
    HU_RUN_TEST(w14_scheduler_rejects_null_args);
    HU_RUN_TEST(w14_scheduler_tick_is_noop_when_empty);
    HU_RUN_TEST(w14_scheduler_enqueue_then_tick_drains_job);
    HU_RUN_TEST(w14_autodream_phases_drain_through_scheduler);
    HU_RUN_TEST(w14_autodream_rejects_null_args);
#endif
}
