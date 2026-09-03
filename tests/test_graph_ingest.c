/* Task 3 of docs/superpowers/plans/2026-09-01-sota-e2e.md — one superseding
 * ingest path for facts. A changed fact (lives_in KoP -> lives_in St Pete)
 * must CLOSE the prior edge so the grounding read sees one current truth.
 * The two live writers used the legacy non-superseding upsert; three of nine
 * human detections at n=40 were stale event-state facts. */
#ifdef HU_ENABLE_SQLITE
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/graph_ingest.h"
#include "test_framework.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static hu_graph_t *open_tmp_graph(hu_allocator_t *alloc, char *path, size_t cap) {
    snprintf(path, cap, "/tmp/hu_graph_ingest_%d.db", (int)getpid());
    unlink(path);
    hu_graph_t *g = NULL;
    if (hu_graph_open(alloc, path, strlen(path), &g) != HU_OK)
        return NULL;
    return g;
}

/* Count open LIVES_IN edges for "self" true at time `at`; report whether the
 * single hit points at the entity named `expect_target`. The window read does
 * not resolve names, so compare ids via hu_graph_find_entity. */
static size_t open_lives_in_at(hu_graph_t *g, hu_allocator_t *alloc, int64_t at,
                               const char *expect_target, bool *matches_out) {
    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    if (matches_out)
        *matches_out = false;
    if (hu_graph_relations_in_window(g, alloc, "self", 4, at, at, 32, &rels, &n) != HU_OK)
        return (size_t)-1;
    hu_graph_entity_t want;
    memset(&want, 0, sizeof(want));
    bool have_want = expect_target && hu_graph_find_entity(g, "self", 4, expect_target,
                                                           strlen(expect_target), &want) == HU_OK;
    size_t hits = 0;
    for (size_t i = 0; i < n; i++) {
        if (rels[i].type != HU_REL_LIVES_IN)
            continue;
        hits++;
        if (have_want && matches_out && rels[i].target_id == want.id)
            *matches_out = true;
    }
    hu_graph_relations_free(alloc, rels, n);
    return hits;
}

static void test_changed_fact_supersedes_prior_edge(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    hu_graph_t *g = open_tmp_graph(&alloc, path, sizeof(path));
    HU_ASSERT_NOT_NULL(g);
    HU_ASSERT_EQ(hu_graph_ingest_fact(g, "self", 4, "user", "lives_in", "king of prussia", 0.9f,
                                      100, "test:1"),
                 HU_OK);
    HU_ASSERT_EQ(
        hu_graph_ingest_fact(g, "self", 4, "user", "lives_in", "st pete", 0.9f, 200, "test:2"),
        HU_OK);
    bool is_new = false, is_old = false;
    /* Now: exactly one open LIVES_IN, and it is the newer place. */
    HU_ASSERT_EQ((long)open_lives_in_at(g, &alloc, 250, "st pete", &is_new), 1L);
    HU_ASSERT_TRUE(is_new);
    /* Then: at t=150 the old place was the truth. */
    HU_ASSERT_EQ((long)open_lives_in_at(g, &alloc, 150, "king of prussia", &is_old), 1L);
    HU_ASSERT_TRUE(is_old);
    hu_graph_close(g, &alloc);
    unlink(path);
}

static void test_same_fact_twice_is_one_edge(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    hu_graph_t *g = open_tmp_graph(&alloc, path, sizeof(path));
    HU_ASSERT_NOT_NULL(g);
    HU_ASSERT_EQ(hu_graph_ingest_fact(g, "self", 4, "user", "works_at", "acme", 0.8f, 100, "t"),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_ingest_fact(g, "self", 4, "user", "works_at", "acme", 0.8f, 300, "t"),
                 HU_OK);
    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_relations_in_window(g, &alloc, "self", 4, 400, 400, 32, &rels, &n),
                 HU_OK);
    size_t works = 0;
    for (size_t i = 0; i < n; i++)
        if (rels[i].type == HU_REL_WORKS_AT)
            works++;
    HU_ASSERT_EQ((long)works, 1L);
    hu_graph_relations_free(&alloc, rels, n);
    hu_graph_close(g, &alloc);
    unlink(path);
}

static void test_unknown_predicate_maps_to_related_to_not_dropped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    hu_graph_t *g = open_tmp_graph(&alloc, path, sizeof(path));
    HU_ASSERT_NOT_NULL(g);
    HU_ASSERT_EQ(hu_graph_ingest_fact(g, "self", 4, "user", "visiting", "tampa", 0.7f, 100, "t"),
                 HU_OK);
    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_relations_in_window(g, &alloc, "self", 4, 150, 150, 32, &rels, &n),
                 HU_OK);
    HU_ASSERT_EQ((long)n, 1L);
    HU_ASSERT_EQ((int)rels[0].type, (int)HU_REL_RELATED_TO);
    hu_graph_relations_free(&alloc, rels, n);
    hu_graph_close(g, &alloc);
    unlink(path);
}

static void test_rejects_empty_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    hu_graph_t *g = open_tmp_graph(&alloc, path, sizeof(path));
    HU_ASSERT_NOT_NULL(g);
    HU_ASSERT_EQ(hu_graph_ingest_fact(g, "self", 4, "user", "lives_in", "", 0.9f, 1, "t"),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_graph_ingest_fact(NULL, "self", 4, "user", "lives_in", "x", 0.9f, 1, "t"),
                 HU_ERR_INVALID_ARGUMENT);
    hu_graph_close(g, &alloc);
    unlink(path);
}

void run_graph_ingest_tests(void) {
    HU_TEST_SUITE("graph_ingest");
    HU_RUN_TEST(test_changed_fact_supersedes_prior_edge);
    HU_RUN_TEST(test_same_fact_twice_is_one_edge);
    HU_RUN_TEST(test_unknown_predicate_maps_to_related_to_not_dropped);
    HU_RUN_TEST(test_rejects_empty_fields);
}
#else
void run_graph_ingest_tests(void) {
    (void)0;
}
#endif
