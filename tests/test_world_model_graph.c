#ifdef HU_ENABLE_SQLITE

#include "test_framework.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/intelligence/world_model.h"
#include <sqlite3.h>
#include <string.h>

static sqlite3 *open_test_db(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    return db;
}

static void close_test_db(sqlite3 *db) {
    if (db)
        sqlite3_close(db);
}

static void world_graph_add_node_and_retrieve(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_causal_world_model_t model;
    HU_ASSERT_EQ(hu_causal_world_model_create(&alloc, db, &model), HU_OK);
    HU_ASSERT_EQ(hu_causal_world_model_init_tables(&model), HU_OK);

    int64_t id = 0;
    HU_ASSERT_EQ(hu_world_add_node(&model, "rain", 4, "event", 5, &id), HU_OK);
    HU_ASSERT_GT(id, 0);

    hu_causal_world_model_deinit(&model);
    close_test_db(db);
}

static void world_graph_add_edge_creates_link(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_causal_world_model_t model;
    HU_ASSERT_EQ(hu_causal_world_model_create(&alloc, db, &model), HU_OK);
    HU_ASSERT_EQ(hu_causal_world_model_init_tables(&model), HU_OK);

    int64_t a = 0, b = 0;
    HU_ASSERT_EQ(hu_world_add_node(&model, "A", 1, "entity", 6, &a), HU_OK);
    HU_ASSERT_EQ(hu_world_add_node(&model, "B", 1, "entity", 6, &b), HU_OK);
    HU_ASSERT_EQ(hu_world_add_edge(&model, a, b, HU_EDGE_CAUSES, 0.9, 1000), HU_OK);

    hu_causal_edge_t edges[8];
    size_t count = 0;
    HU_ASSERT_EQ(hu_world_get_neighbors(&model, a, edges, 8, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_EQ(edges[0].target_id, b);
    HU_ASSERT_EQ(edges[0].type, HU_EDGE_CAUSES);

    hu_causal_world_model_deinit(&model);
    close_test_db(db);
}

static void world_graph_edge_confidence_updates(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_causal_world_model_t model;
    HU_ASSERT_EQ(hu_causal_world_model_create(&alloc, db, &model), HU_OK);
    HU_ASSERT_EQ(hu_causal_world_model_init_tables(&model), HU_OK);

    int64_t a = 0, b = 0;
    HU_ASSERT_EQ(hu_world_add_node(&model, "X", 1, "action", 6, &a), HU_OK);
    HU_ASSERT_EQ(hu_world_add_node(&model, "Y", 1, "outcome", 7, &b), HU_OK);
    HU_ASSERT_EQ(hu_world_add_edge(&model, a, b, HU_EDGE_CAUSES, 0.7, 1000), HU_OK);
    HU_ASSERT_EQ(hu_world_add_edge(&model, a, b, HU_EDGE_CAUSES, 0.85, 1001), HU_OK);

    hu_causal_edge_t edges[8];
    size_t count = 0;
    HU_ASSERT_EQ(hu_world_get_neighbors(&model, a, edges, 8, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_EQ(edges[0].evidence_count, 2);
    HU_ASSERT_TRUE(edges[0].confidence >= 0.8 && edges[0].confidence <= 0.9);

    hu_causal_world_model_deinit(&model);
    close_test_db(db);
}

static void world_graph_trace_chain_depth_1(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_causal_world_model_t model;
    HU_ASSERT_EQ(hu_causal_world_model_create(&alloc, db, &model), HU_OK);
    HU_ASSERT_EQ(hu_causal_world_model_init_tables(&model), HU_OK);

    int64_t a = 0, b = 0;
    HU_ASSERT_EQ(hu_world_add_node(&model, "A", 1, "entity", 6, &a), HU_OK);
    HU_ASSERT_EQ(hu_world_add_node(&model, "B", 1, "entity", 6, &b), HU_OK);
    HU_ASSERT_EQ(hu_world_add_edge(&model, a, b, HU_EDGE_CAUSES, 0.9, 1000), HU_OK);

    hu_causal_node_t path[8];
    size_t len = 0;
    HU_ASSERT_EQ(hu_world_trace_causal_chain(&model, a, 1, path, 8, &len), HU_OK);
    HU_ASSERT_EQ(len, 2u);
    HU_ASSERT_EQ(path[0].id, a);
    HU_ASSERT_STR_EQ(path[0].label, "A");
    HU_ASSERT_EQ(path[1].id, b);
    HU_ASSERT_STR_EQ(path[1].label, "B");

    hu_causal_world_model_deinit(&model);
    close_test_db(db);
}

static void world_graph_trace_chain_depth_3(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_causal_world_model_t model;
    HU_ASSERT_EQ(hu_causal_world_model_create(&alloc, db, &model), HU_OK);
    HU_ASSERT_EQ(hu_causal_world_model_init_tables(&model), HU_OK);

    int64_t ids[4] = {0};
    HU_ASSERT_EQ(hu_world_add_node(&model, "A", 1, "entity", 6, &ids[0]), HU_OK);
    HU_ASSERT_EQ(hu_world_add_node(&model, "B", 1, "entity", 6, &ids[1]), HU_OK);
    HU_ASSERT_EQ(hu_world_add_node(&model, "C", 1, "entity", 6, &ids[2]), HU_OK);
    HU_ASSERT_EQ(hu_world_add_node(&model, "D", 1, "entity", 6, &ids[3]), HU_OK);
    HU_ASSERT_EQ(hu_world_add_edge(&model, ids[0], ids[1], HU_EDGE_CAUSES, 0.9, 1000), HU_OK);
    HU_ASSERT_EQ(hu_world_add_edge(&model, ids[1], ids[2], HU_EDGE_CAUSES, 0.9, 1000), HU_OK);
    HU_ASSERT_EQ(hu_world_add_edge(&model, ids[2], ids[3], HU_EDGE_CAUSES, 0.9, 1000), HU_OK);

    hu_causal_node_t path[8];
    size_t len = 0;
    HU_ASSERT_EQ(hu_world_trace_causal_chain(&model, ids[0], 3, path, 8, &len), HU_OK);
    HU_ASSERT_EQ(len, 4u);
    HU_ASSERT_STR_EQ(path[0].label, "A");
    HU_ASSERT_STR_EQ(path[1].label, "B");
    HU_ASSERT_STR_EQ(path[2].label, "C");
    HU_ASSERT_STR_EQ(path[3].label, "D");

    hu_causal_world_model_deinit(&model);
    close_test_db(db);
}

static void world_graph_find_paths_simple(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_causal_world_model_t model;
    HU_ASSERT_EQ(hu_causal_world_model_create(&alloc, db, &model), HU_OK);
    HU_ASSERT_EQ(hu_causal_world_model_init_tables(&model), HU_OK);

    int64_t a = 0, b = 0, c = 0;
    HU_ASSERT_EQ(hu_world_add_node(&model, "A", 1, "entity", 6, &a), HU_OK);
    HU_ASSERT_EQ(hu_world_add_node(&model, "B", 1, "entity", 6, &b), HU_OK);
    HU_ASSERT_EQ(hu_world_add_node(&model, "C", 1, "entity", 6, &c), HU_OK);
    HU_ASSERT_EQ(hu_world_add_edge(&model, a, b, HU_EDGE_CAUSES, 0.9, 1000), HU_OK);
    HU_ASSERT_EQ(hu_world_add_edge(&model, b, c, HU_EDGE_CAUSES, 0.9, 1000), HU_OK);

    hu_causal_node_t path[8];
    size_t len = 0;
    HU_ASSERT_EQ(hu_world_find_paths(&model, a, c, 10, path, 8, &len), HU_OK);
    HU_ASSERT_EQ(len, 3u);
    HU_ASSERT_STR_EQ(path[0].label, "A");
    HU_ASSERT_STR_EQ(path[1].label, "B");
    HU_ASSERT_STR_EQ(path[2].label, "C");

    hu_causal_world_model_deinit(&model);
    close_test_db(db);
}

static void world_graph_null_model_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_causal_world_model_t model;
    HU_ASSERT_EQ(hu_causal_world_model_create(&alloc, db, &model), HU_OK);
    HU_ASSERT_EQ(hu_causal_world_model_init_tables(&model), HU_OK);

    int64_t id = 0;
    hu_causal_edge_t edges[4];
    size_t count = 0;
    hu_causal_node_t path[4];
    size_t len = 0;

    HU_ASSERT_EQ(hu_world_add_node(NULL, "x", 1, "entity", 6, &id), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_world_add_edge(NULL, 1, 2, HU_EDGE_CAUSES, 0.5, 1000), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_world_get_neighbors(NULL, 1, edges, 4, &count), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_world_trace_causal_chain(NULL, 1, 3, path, 4, &len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_world_find_paths(NULL, 1, 2, 5, path, 4, &len), HU_ERR_INVALID_ARGUMENT);

    hu_causal_world_model_deinit(&model);
    close_test_db(db);
}

void run_world_model_graph_tests(void) {
    HU_TEST_SUITE("world_model_graph");

    HU_RUN_TEST(world_graph_add_node_and_retrieve);
    HU_RUN_TEST(world_graph_add_edge_creates_link);
    HU_RUN_TEST(world_graph_edge_confidence_updates);
    HU_RUN_TEST(world_graph_trace_chain_depth_1);
    HU_RUN_TEST(world_graph_trace_chain_depth_3);
    HU_RUN_TEST(world_graph_find_paths_simple);
    HU_RUN_TEST(world_graph_null_model_returns_error);
}

#else

void run_world_model_graph_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
