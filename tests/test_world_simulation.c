#ifdef HU_ENABLE_SQLITE

#include "test_framework.h"
#include "human/core/allocator.h"
#include "human/intelligence/world_model.h"
#include <sqlite3.h>
#include <string.h>
#include <time.h>

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

static void simulation_predicts_with_observations(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_causal_world_model_t model;
    HU_ASSERT_EQ(hu_causal_world_model_create(&alloc, db, &model), HU_OK);
    HU_ASSERT_EQ(hu_causal_world_model_init_tables(&model), HU_OK);

    int64_t now = (int64_t)time(NULL);
    HU_ASSERT_EQ(hu_world_record_outcome(&model, "restart server", 14,
                                         "server recovered", 16, 0.9, now), HU_OK);

    hu_wm_prediction_t pred = {0};
    HU_ASSERT_EQ(hu_world_simulate(&model, "restart service", 15, NULL, 0, &pred), HU_OK);
    HU_ASSERT_TRUE(pred.outcome_len > 0);
    HU_ASSERT_TRUE(pred.confidence > 0.3);

    hu_causal_world_model_deinit(&model);
    close_test_db(db);
}

static void simulation_handles_no_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_causal_world_model_t model;
    HU_ASSERT_EQ(hu_causal_world_model_create(&alloc, db, &model), HU_OK);
    HU_ASSERT_EQ(hu_causal_world_model_init_tables(&model), HU_OK);

    hu_wm_prediction_t pred = {0};
    HU_ASSERT_EQ(hu_world_simulate(&model, "unknown action", 14, NULL, 0, &pred), HU_OK);
    HU_ASSERT_TRUE(pred.confidence <= 0.2);

    hu_causal_world_model_deinit(&model);
    close_test_db(db);
}

static void simulation_context_from_text_extracts_entities(void) {
    hu_world_context_t ctx = {0};
    const char *text = "Alice met Bob";
    HU_ASSERT_EQ(hu_world_context_from_text(text, 13, &ctx), HU_OK);
    HU_ASSERT_EQ(ctx.entity_count, 2u);
    HU_ASSERT_STR_EQ(ctx.entities[0], "Alice");
    HU_ASSERT_STR_EQ(ctx.entities[1], "Bob");

    hu_world_context_t empty = {0};
    HU_ASSERT_EQ(hu_world_context_from_text("", 0, &empty), HU_OK);
    HU_ASSERT_EQ(empty.entity_count, 0u);
}

static void simulation_context_affects_prediction(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_causal_world_model_t model;
    HU_ASSERT_EQ(hu_causal_world_model_create(&alloc, db, &model), HU_OK);
    HU_ASSERT_EQ(hu_causal_world_model_init_tables(&model), HU_OK);

    int64_t now = (int64_t)time(NULL);
    /* Record observation where action contains "Alice" */
    HU_ASSERT_EQ(hu_world_record_outcome(&model, "Alice called", 11,
                                         "connected", 9, 0.85, now), HU_OK);

    hu_world_context_t ctx_alice = {0};
    HU_ASSERT_EQ(hu_world_context_from_text("Alice met Bob", 13, &ctx_alice), HU_OK);

    hu_world_context_t ctx_charlie = {0};
    HU_ASSERT_EQ(hu_world_context_from_text("Charlie left", 12, &ctx_charlie), HU_OK);

    hu_wm_prediction_t pred_alice = {0};
    HU_ASSERT_EQ(hu_world_simulate_with_context(&model, "call", 4, &ctx_alice, &pred_alice), HU_OK);

    hu_wm_prediction_t pred_charlie = {0};
    HU_ASSERT_EQ(hu_world_simulate_with_context(&model, "call", 4, &ctx_charlie, &pred_charlie), HU_OK);

    /* Context with Alice should have higher confidence (matches "Alice called") */
    HU_ASSERT_TRUE(pred_alice.confidence >= pred_charlie.confidence);

    hu_causal_world_model_deinit(&model);
    close_test_db(db);
}

static void simulation_compare_actions_ranks(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_causal_world_model_t model;
    HU_ASSERT_EQ(hu_causal_world_model_create(&alloc, db, &model), HU_OK);
    HU_ASSERT_EQ(hu_causal_world_model_init_tables(&model), HU_OK);

    int64_t now = (int64_t)time(NULL);
    HU_ASSERT_EQ(hu_world_record_outcome(&model, "restart", 7, "recovered", 9, 0.9, now), HU_OK);
    HU_ASSERT_EQ(hu_world_record_outcome(&model, "restart", 7, "recovered", 9, 0.85, now + 1), HU_OK);
    HU_ASSERT_EQ(hu_world_record_outcome(&model, "ignore", 6, "crashed", 7, 0.3, now + 2), HU_OK);

    hu_world_context_t ctx = {0};
    HU_ASSERT_EQ(hu_world_context_from_text("server down", 11, &ctx), HU_OK);

    const char *actions[] = {"restart", "ignore", "wait"};
    size_t lens[] = {7, 6, 4};
    hu_action_option_t opts[3];
    memset(opts, 0, sizeof(opts));
    HU_ASSERT_EQ(hu_world_compare_actions(&model, actions, lens, 3, &ctx, opts), HU_OK);

    /* restart should rank first (has observations), ignore second, wait last */
    HU_ASSERT_TRUE(opts[0].prediction.confidence >= opts[1].prediction.confidence);
    HU_ASSERT_TRUE(opts[1].prediction.confidence >= opts[2].prediction.confidence);

    hu_causal_world_model_deinit(&model);
    close_test_db(db);
}

static void simulation_what_if_generates_scenarios(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_causal_world_model_t model;
    HU_ASSERT_EQ(hu_causal_world_model_create(&alloc, db, &model), HU_OK);
    HU_ASSERT_EQ(hu_causal_world_model_init_tables(&model), HU_OK);

    int64_t now = (int64_t)time(NULL);
    HU_ASSERT_EQ(hu_world_record_outcome(&model, "deploy", 6, "success", 7, 0.8, now), HU_OK);

    hu_world_context_t ctx = {0};
    HU_ASSERT_EQ(hu_world_context_from_text("production", 10, &ctx), HU_OK);

    hu_wm_prediction_t scenarios[4];
    size_t count = 0;
    HU_ASSERT_EQ(hu_world_what_if(&model, "deploy", 6, &ctx, scenarios, 4, &count), HU_OK);
    HU_ASSERT_EQ(count, 3u);

    /* Best case should have highest confidence, worst lowest */
    HU_ASSERT_TRUE(scenarios[0].confidence >= scenarios[1].confidence);
    HU_ASSERT_TRUE(scenarios[1].confidence >= scenarios[2].confidence);

    hu_causal_world_model_deinit(&model);
    close_test_db(db);
}

static void simulation_null_args_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_causal_world_model_t model;
    HU_ASSERT_EQ(hu_causal_world_model_create(&alloc, db, &model), HU_OK);
    HU_ASSERT_EQ(hu_causal_world_model_init_tables(&model), HU_OK);

    hu_wm_prediction_t pred = {0};
    hu_world_context_t ctx = {0};

    HU_ASSERT_EQ(hu_world_simulate(NULL, "action", 6, NULL, 0, &pred), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_world_simulate_with_context(NULL, "action", 6, &ctx, &pred),
                 HU_ERR_INVALID_ARGUMENT);

    hu_action_option_t opts[1];
    const char *actions[] = {"action"};
    size_t lens[] = {6};
    HU_ASSERT_EQ(hu_world_compare_actions(NULL, actions, lens, 1, &ctx, opts),
                 HU_ERR_INVALID_ARGUMENT);

    hu_wm_prediction_t scenarios[3];
    size_t count = 0;
    HU_ASSERT_EQ(hu_world_what_if(NULL, "action", 6, &ctx, scenarios, 3, &count),
                 HU_ERR_INVALID_ARGUMENT);

    hu_causal_world_model_deinit(&model);
    close_test_db(db);
}

void run_world_simulation_tests(void) {
    HU_TEST_SUITE("world_sim");
    HU_RUN_TEST(simulation_predicts_with_observations);
    HU_RUN_TEST(simulation_handles_no_data);
    HU_RUN_TEST(simulation_context_from_text_extracts_entities);
    HU_RUN_TEST(simulation_context_affects_prediction);
    HU_RUN_TEST(simulation_compare_actions_ranks);
    HU_RUN_TEST(simulation_what_if_generates_scenarios);
    HU_RUN_TEST(simulation_null_args_returns_error);
}

#else

void run_world_simulation_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
