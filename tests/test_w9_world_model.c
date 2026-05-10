/* W9 — World model: build, cache hit/miss, invalidation, negative memory.
 * All tests run on in-memory SQLite. */

#include "human/agent/world_model.h"
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"
#include "test_framework.h"

#include <stdint.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) { g_alloc = hu_system_allocator(); return &g_alloc; }

static void open_facade_(hu_graph_t **g, hu_memory_t **m) {
    HU_ASSERT_EQ(hu_graph_open(A(), NULL, 0, g), HU_OK);
    HU_ASSERT_EQ(hu_memory_open(A(), *g, m), HU_OK);
    /* Ensure no stale cache entries leak across tests. */
    hu_world_model_invalidate(NULL, 0);
}

static void close_facade_(hu_graph_t *g, hu_memory_t *m) {
    hu_world_model_invalidate(NULL, 0);
    hu_memory_close(m, A());
    hu_graph_close(g, A());
}

static void seed_one_relation_(hu_graph_t *g, const char *cid) {
    int64_t alice = 0, acme = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, cid, strlen(cid), "Alice", 5,
                                          HU_ENTITY_PERSON, NULL, &alice),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, cid, strlen(cid), "Acme", 4,
                                          HU_ENTITY_ORGANIZATION, NULL, &acme),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation(g, cid, strlen(cid), alice, acme,
                                            HU_REL_WORKS_AT, 1.0f, NULL, 0),
                 HU_OK);
}

/* --- build --- */

static void test_w9_build_returns_entities_and_relations(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);
    seed_one_relation_(g, "u1");

    hu_world_model_t *wm = NULL;
    HU_ASSERT_EQ(hu_world_model_build(m, A(), "u1", 2, 1735690000000LL, &wm), HU_OK);
    HU_ASSERT_NOT_NULL(wm);
    HU_ASSERT_EQ(strcmp(wm->contact_id, "u1"), 0);
    HU_ASSERT_GT(wm->built_at, 0);
    HU_ASSERT_GT(wm->valid_until, wm->built_at);
    HU_ASSERT_EQ(wm->entities_count, 2u);
    HU_ASSERT_EQ(wm->relations_count, 1u);
    /* ToM stub fields are present and zero-initialized cleanly. */
    HU_ASSERT_EQ(strcmp(wm->dominant_emotion, "neutral"), 0);

    hu_world_model_free(A(), wm);
    close_facade_(g, m);
}

static void test_w9_build_with_no_data_returns_empty_snapshot(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);

    hu_world_model_t *wm = NULL;
    HU_ASSERT_EQ(hu_world_model_build(m, A(), "u-empty", 7, 1735690000000LL, &wm), HU_OK);
    HU_ASSERT_NOT_NULL(wm);
    HU_ASSERT_EQ(wm->entities_count, 0u);
    HU_ASSERT_EQ(wm->relations_count, 0u);
    HU_ASSERT_EQ(wm->negatives_count, 0u);

    hu_world_model_free(A(), wm);
    close_facade_(g, m);
}

/* --- cache --- */

static void test_w9_load_cache_hit_within_ttl(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);
    seed_one_relation_(g, "u-cached");

    hu_world_model_t *wm1 = NULL;
    HU_ASSERT_EQ(hu_world_model_load(m, A(), "u-cached", 8, 1000LL, &wm1), HU_OK);
    HU_ASSERT_EQ(wm1->entities_count, 2u);

    /* Insert a NEW entity. Without invalidation, the cache hit should
     * still return the original 2-entity snapshot. */
    int64_t carol = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u-cached", 8, "Carol", 5, HU_ENTITY_PERSON,
                                          NULL, &carol),
                 HU_OK);

    hu_world_model_t *wm2 = NULL;
    /* Same now_ms; entry not yet expired. */
    HU_ASSERT_EQ(hu_world_model_load(m, A(), "u-cached", 8, 2000LL, &wm2), HU_OK);
    HU_ASSERT_EQ(wm2->entities_count, 2u);

    hu_world_model_free(A(), wm1);
    hu_world_model_free(A(), wm2);
    close_facade_(g, m);
}

static void test_w9_load_cache_miss_after_invalidation(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);
    seed_one_relation_(g, "u-inv");

    hu_world_model_t *wm1 = NULL;
    HU_ASSERT_EQ(hu_world_model_load(m, A(), "u-inv", 5, 1000LL, &wm1), HU_OK);
    HU_ASSERT_EQ(wm1->entities_count, 2u);

    int64_t carol = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u-inv", 5, "Carol", 5, HU_ENTITY_PERSON,
                                          NULL, &carol),
                 HU_OK);

    /* Invalidate; next load should rebuild and see Carol. */
    hu_world_model_invalidate("u-inv", 5);

    hu_world_model_t *wm2 = NULL;
    HU_ASSERT_EQ(hu_world_model_load(m, A(), "u-inv", 5, 2000LL, &wm2), HU_OK);
    HU_ASSERT_EQ(wm2->entities_count, 3u);

    hu_world_model_free(A(), wm1);
    hu_world_model_free(A(), wm2);
    close_facade_(g, m);
}

static void test_w9_load_cache_expires_after_ttl(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);
    seed_one_relation_(g, "u-ttl");

    hu_world_model_t *wm1 = NULL;
    HU_ASSERT_EQ(hu_world_model_load(m, A(), "u-ttl", 5, 1000LL, &wm1), HU_OK);

    int64_t carol = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u-ttl", 5, "Carol", 5, HU_ENTITY_PERSON,
                                          NULL, &carol),
                 HU_OK);

    /* TTL is 60s by default; jump 120s ahead. */
    hu_world_model_t *wm2 = NULL;
    HU_ASSERT_EQ(hu_world_model_load(m, A(), "u-ttl", 5,
                                       1000LL + 120 * 1000, &wm2),
                 HU_OK);
    HU_ASSERT_EQ(wm2->entities_count, 3u);

    hu_world_model_free(A(), wm1);
    hu_world_model_free(A(), wm2);
    close_facade_(g, m);
}

/* --- negative memory --- */

static void test_w9_negative_memory_round_trip(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);

    hu_negative_memory_t nm;
    memset(&nm, 0, sizeof(nm));
    strcpy(nm.text, "do not bring up the project failure");
    strcpy(nm.scope, "topic");
    strcpy(nm.reason, "user said it was painful");
    nm.belief = hu_belief_init(0.95f, "user-explicit", 1735690000000LL);
    nm.created_at = 1735690000000LL;

    int64_t id = 0;
    HU_ASSERT_EQ(hu_negative_memory_add(g, "u-neg", 5, &nm, &id), HU_OK);
    HU_ASSERT_GT(id, 0);

    hu_negative_memory_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_negative_memory_list(g, A(), "u-neg", 5, 32, &out, &n), HU_OK);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ(out[0].id, id);
    HU_ASSERT_EQ(strcmp(out[0].text, "do not bring up the project failure"), 0);
    HU_ASSERT_EQ(strcmp(out[0].scope, "topic"), 0);
    HU_ASSERT_FLOAT_EQ(out[0].belief.mean, 0.95f, 1e-3);

    hu_negative_memory_free(A(), out, n);
    close_facade_(g, m);
}

static void test_w9_negative_memory_appears_in_world_model(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);
    seed_one_relation_(g, "u-neg2");

    hu_negative_memory_t nm;
    memset(&nm, 0, sizeof(nm));
    strcpy(nm.text, "no jokes about Acme's CEO");
    strcpy(nm.scope, "topic");
    nm.belief = hu_belief_init(0.99f, "user-explicit", 1735690000000LL);
    nm.created_at = 1735690000000LL;
    int64_t id = 0;
    HU_ASSERT_EQ(hu_negative_memory_add(g, "u-neg2", 6, &nm, &id), HU_OK);

    hu_world_model_t *wm = NULL;
    HU_ASSERT_EQ(hu_world_model_build(m, A(), "u-neg2", 6, 1735690000000LL, &wm), HU_OK);
    HU_ASSERT_NOT_NULL(wm);
    HU_ASSERT_EQ(wm->negatives_count, 1u);
    HU_ASSERT_EQ(strcmp(wm->negatives[0].text, "no jokes about Acme's CEO"), 0);

    hu_world_model_free(A(), wm);
    close_facade_(g, m);
}

/* --- adversarial --- */

static void test_w9_invalid_args_rejected(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);

    hu_world_model_t *wm = NULL;
    HU_ASSERT_EQ(hu_world_model_build(NULL, A(), "u", 1, 0, &wm), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_world_model_build(m, NULL, "u", 1, 0, &wm), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_world_model_build(m, A(), NULL, 0, 0, &wm), HU_ERR_INVALID_ARGUMENT);

    hu_negative_memory_t nm;
    memset(&nm, 0, sizeof(nm));
    /* empty text rejected */
    HU_ASSERT_EQ(hu_negative_memory_add(g, "u", 1, &nm, NULL), HU_ERR_INVALID_ARGUMENT);

    /* contact_id longer than 64 bytes is rejected at load. */
    char long_cid[80];
    memset(long_cid, 'x', 79);
    long_cid[79] = '\0';
    HU_ASSERT_EQ(hu_world_model_load(m, A(), long_cid, 79, 0, &wm),
                 HU_ERR_INVALID_ARGUMENT);

    close_facade_(g, m);
}

void run_w9_world_model_tests(void) {
    HU_TEST_SUITE("W9 world model - per-contact unified snapshot");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_w9_build_returns_entities_and_relations);
    HU_RUN_TEST(test_w9_build_with_no_data_returns_empty_snapshot);
    HU_RUN_TEST(test_w9_load_cache_hit_within_ttl);
    HU_RUN_TEST(test_w9_load_cache_miss_after_invalidation);
    HU_RUN_TEST(test_w9_load_cache_expires_after_ttl);
    HU_RUN_TEST(test_w9_negative_memory_round_trip);
    HU_RUN_TEST(test_w9_negative_memory_appears_in_world_model);
    HU_RUN_TEST(test_w9_invalid_args_rejected);
#endif
}

#else /* !HU_ENABLE_SQLITE */

void run_w9_world_model_tests(void) {}

#endif
