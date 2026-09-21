/* W3 — Multi-graph cross-edges + case-based recall.
 * All tests use :memory: via the test-build path of graph.c. */

#include "human/core/allocator.h"
#include "human/memory/cross_graph.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) {
    g_alloc = hu_system_allocator();
    return &g_alloc;
}

static void open_graph(hu_graph_t **g) {
    HU_ASSERT_EQ(hu_graph_open(A(), NULL, 0, g), HU_OK);
}

/* --- Cross-edge upsert + idempotency --- */
static void test_w3_cross_edge_upsert_is_idempotent(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);

    HU_ASSERT_EQ(hu_cross_edge_upsert(g, "u1", 2, "entity", 1, "emotion", 2, "FELT_DURING", 0.9f,
                                      1735689600000LL, 0, 1.0f),
                 HU_OK);
    HU_ASSERT_EQ(hu_cross_edge_upsert(g, "u1", 2, "entity", 1, "emotion", 2, "FELT_DURING", 0.9f,
                                      1735689600000LL, 0, 1.0f),
                 HU_OK);

    hu_cross_edge_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_cross_graph_traverse(g, A(), "u1", 2, "entity", 1, 1, 32, 0, 0, &out, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_STR_EQ(out[0].dst_graph, "emotion");
    HU_ASSERT_STR_EQ(out[0].relation, "FELT_DURING");
    hu_cross_edges_free(A(), out, n);

    hu_graph_close(g, A());
}

/* --- Cross-graph traversal: bounded results, sorted by weight --- */
static void test_w3_cross_graph_traverse_returns_top_weighted(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);

    hu_cross_edge_upsert(g, "u1", 2, "entity", 1, "episode", 10, "ABOUT", 1.0f, 1735689600000LL, 0,
                         0.3f);
    hu_cross_edge_upsert(g, "u1", 2, "entity", 1, "episode", 11, "ABOUT", 1.0f, 1735689600000LL, 0,
                         0.9f);
    hu_cross_edge_upsert(g, "u1", 2, "entity", 1, "episode", 12, "ABOUT", 1.0f, 1735689600000LL, 0,
                         0.7f);

    hu_cross_edge_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_cross_graph_traverse(g, A(), "u1", 2, "entity", 1, 1, 2, 0, 0, &out, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, 2);
    HU_ASSERT_EQ(out[0].dst_id, 11); /* heaviest first */
    HU_ASSERT_EQ(out[1].dst_id, 12);
    hu_cross_edges_free(A(), out, n);

    hu_graph_close(g, A());
}

/* --- Cross-graph traversal: window predicate --- */
static void test_w3_cross_graph_traverse_filters_by_window(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);

    /* 2024 edge */
    hu_cross_edge_upsert(g, "u1", 2, "entity", 1, "episode", 10, "ABOUT", 1.0f,
                         1704067200000LL /* 2024-01-01 */, 1735689600000LL /* 2025-01-01 */, 1.0f);
    /* 2025 edge */
    hu_cross_edge_upsert(g, "u1", 2, "entity", 1, "episode", 11, "ABOUT", 1.0f,
                         1735689600000LL /* 2025-01-01 */, 0, 1.0f);

    hu_cross_edge_t *out = NULL;
    size_t n = 0;
    /* Window = "during 2025": expect only the 2025 edge. */
    HU_ASSERT_EQ(hu_cross_graph_traverse(g, A(), "u1", 2, "entity", 1, 1, 32,
                                         1735689600000LL /* 2025-01-01 */,
                                         1767139200000LL /* 2025-12-30 */, &out, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_EQ(out[0].dst_id, 11);
    hu_cross_edges_free(A(), out, n);

    hu_graph_close(g, A());
}

/* --- Case-based recall: empty store returns no rows --- */
/* --- Case-based recall: matching anchors score higher --- */
/* --- Case-based recall: top-K trims --- */
#endif /* HU_ENABLE_SQLITE */

void run_w3_multigraph_tests(void) {
    HU_TEST_SUITE("W3 multi-graph + case-based");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_w3_cross_edge_upsert_is_idempotent);
    HU_RUN_TEST(test_w3_cross_graph_traverse_returns_top_weighted);
    HU_RUN_TEST(test_w3_cross_graph_traverse_filters_by_window);
#endif
}
