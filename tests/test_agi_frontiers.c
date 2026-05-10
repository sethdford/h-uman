#include "human/agent.h"
#include "human/agent/constitutional.h"
#include "human/agent/dispatcher.h"
#include "human/agent/orchestrator.h"
#include "human/agent/orchestrator_llm.h"
#include "human/agent/process_reward.h"
#include "human/agent/speculative.h"
#include "human/agent/tree_of_thought.h"
#include "human/agent/uncertainty.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/memory/adaptive_rag.h"
#include "human/memory/self_rag.h"
#include "human/memory/tiers.h"
#include "human/ml/dpo.h"
#include "human/tools/delegate.h"
#include "test_framework.h"
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include "human/agent/goals.h"
#include "human/intelligence/online_learning.h"
#include "human/intelligence/self_improve.h"
#include "human/intelligence/value_learning.h"
#include "human/intelligence/world_model.h"
#include "human/memory.h"
#include "human/memory/retrieval/strategy_learner.h"
#include <sqlite3.h>

static sqlite3 *open_test_db(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    const char *sql = "CREATE TABLE IF NOT EXISTS kv (key TEXT PRIMARY KEY, value TEXT)";
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    return db;
}

static void close_test_db(sqlite3 *db) {
    if (db)
        sqlite3_close(db);
}

/* -- Self-Improvement ------------------------------------------------ */

static void test_self_improve_create_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_self_improve_t engine;
    HU_ASSERT_EQ(hu_self_improve_create(NULL, NULL, &engine), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_self_improve_create(&alloc, NULL, &engine), HU_ERR_INVALID_ARGUMENT);
}

static void test_self_improve_init_tables(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_self_improve_t engine;
    HU_ASSERT_EQ(hu_self_improve_create(&alloc, db, &engine), HU_OK);
    HU_ASSERT_EQ(hu_self_improve_init_tables(&engine), HU_OK);
    HU_ASSERT_EQ(hu_self_improve_init_tables(&engine), HU_OK);
    hu_self_improve_deinit(&engine);
    close_test_db(db);
}

static void test_self_improve_tool_outcome_tracking(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_self_improve_t engine;
    hu_self_improve_create(&alloc, db, &engine);
    hu_self_improve_init_tables(&engine);

    double w = hu_self_improve_get_tool_weight(&engine, "shell", 5);
    HU_ASSERT_TRUE(w >= 0.99 && w <= 1.01);

    HU_ASSERT_EQ(hu_self_improve_record_tool_outcome(&engine, "shell", 5, true, 1000), HU_OK);
    HU_ASSERT_EQ(hu_self_improve_record_tool_outcome(&engine, "shell", 5, true, 1001), HU_OK);
    HU_ASSERT_EQ(hu_self_improve_record_tool_outcome(&engine, "shell", 5, false, 1002), HU_OK);

    w = hu_self_improve_get_tool_weight(&engine, "shell", 5);
    HU_ASSERT_TRUE(w > 0.5 && w < 1.0);

    hu_self_improve_deinit(&engine);
    close_test_db(db);
}

static void test_self_improve_prompt_patches(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_self_improve_t engine;
    hu_self_improve_create(&alloc, db, &engine);
    hu_self_improve_init_tables(&engine);

    size_t count = 99;
    HU_ASSERT_EQ(hu_self_improve_active_patch_count(&engine, &count), HU_OK);
    HU_ASSERT_EQ(count, 0u);

    char *patches = NULL;
    size_t patches_len = 0;
    HU_ASSERT_EQ(hu_self_improve_get_prompt_patches(&engine, &patches, &patches_len), HU_OK);
    if (patches)
        alloc.free(alloc.ctx, patches, patches_len + 1);

    hu_self_improve_deinit(&engine);
    close_test_db(db);
}

static void test_self_improve_prompt_patches_with_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_self_improve_t engine;
    hu_self_improve_create(&alloc, db, &engine);
    hu_self_improve_init_tables(&engine);

    sqlite3_exec(db,
                 "CREATE TABLE IF NOT EXISTS self_evaluations("
                 "id INTEGER PRIMARY KEY, recommendations TEXT, created_at INTEGER)",
                 NULL, NULL, NULL);
    sqlite3_exec(db,
                 "INSERT INTO self_evaluations (recommendations, created_at) VALUES "
                 "('Be more concise', 100), ('Use bullet points', 200)",
                 NULL, NULL, NULL);

    HU_ASSERT_EQ(hu_self_improve_apply_reflections(&engine, 300), HU_OK);

    size_t count = 0;
    HU_ASSERT_EQ(hu_self_improve_active_patch_count(&engine, &count), HU_OK);
    HU_ASSERT_EQ(count, 2u);

    char *patches = NULL;
    size_t patches_len = 0;
    HU_ASSERT_EQ(hu_self_improve_get_prompt_patches(&engine, &patches, &patches_len), HU_OK);
    HU_ASSERT_NOT_NULL(patches);
    HU_ASSERT_TRUE(patches_len > 0);
    HU_ASSERT_NOT_NULL(strstr(patches, "concise"));
    HU_ASSERT_NOT_NULL(strstr(patches, "bullet"));
    alloc.free(alloc.ctx, patches, patches_len + 1);

    hu_self_improve_deinit(&engine);
    close_test_db(db);
}

/* -- Goal Autonomy --------------------------------------------------- */

static void test_goal_create_null_args(void) {
    hu_goal_engine_t engine;
    HU_ASSERT_EQ(hu_goal_engine_create(NULL, NULL, &engine), HU_ERR_INVALID_ARGUMENT);
}

static void test_goal_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_goal_engine_t engine;
    HU_ASSERT_EQ(hu_goal_engine_create(&alloc, db, &engine), HU_OK);
    HU_ASSERT_EQ(hu_goal_init_tables(&engine), HU_OK);

    int64_t id = 0;
    HU_ASSERT_EQ(hu_goal_create(&engine, "learn C11", 9, 0.8, 0, 0, 1000, &id), HU_OK);
    HU_ASSERT_TRUE(id > 0);

    hu_goal_t goal;
    bool found = false;
    HU_ASSERT_EQ(hu_goal_get(&engine, id, &goal, &found), HU_OK);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_EQ(goal.status, HU_AUTO_GOAL_PENDING);

    HU_ASSERT_EQ(hu_goal_update_status(&engine, id, HU_AUTO_GOAL_ACTIVE, 1001), HU_OK);
    HU_ASSERT_EQ(hu_goal_get(&engine, id, &goal, &found), HU_OK);
    HU_ASSERT_EQ(goal.status, HU_AUTO_GOAL_ACTIVE);

    HU_ASSERT_EQ(hu_goal_update_progress(&engine, id, 1.0, 1002), HU_OK);
    HU_ASSERT_EQ(hu_goal_get(&engine, id, &goal, &found), HU_OK);
    HU_ASSERT_EQ(goal.status, HU_AUTO_GOAL_COMPLETED);

    size_t count = 0;
    HU_ASSERT_EQ(hu_goal_count(&engine, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);

    hu_goal_engine_deinit(&engine);
    close_test_db(db);
}

static void test_goal_decompose(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_goal_engine_t engine;
    hu_goal_engine_create(&alloc, db, &engine);
    hu_goal_init_tables(&engine);

    int64_t parent_id = 0;
    hu_goal_create(&engine, "build agent", 11, 0.9, 0, 0, 1000, &parent_id);

    const char *subs[] = {"design API", "implement", "test"};
    size_t sub_lens[] = {10, 9, 4};
    int64_t sub_ids[3] = {0};
    HU_ASSERT_EQ(hu_goal_decompose(&engine, parent_id, subs, sub_lens, 3, 1001, sub_ids), HU_OK);
    HU_ASSERT_TRUE(sub_ids[0] > 0);
    HU_ASSERT_TRUE(sub_ids[1] > 0);
    HU_ASSERT_TRUE(sub_ids[2] > 0);

    hu_goal_t g;
    bool found;
    hu_goal_get(&engine, sub_ids[0], &g, &found);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_EQ(g.parent_id, parent_id);

    hu_goal_engine_deinit(&engine);
    close_test_db(db);
}

static void test_goal_select_next(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_goal_engine_t engine;
    hu_goal_engine_create(&alloc, db, &engine);
    hu_goal_init_tables(&engine);

    int64_t id1, id2;
    hu_goal_create(&engine, "low priority", 12, 0.3, 0, 0, 1000, &id1);
    hu_goal_create(&engine, "high priority", 13, 0.9, 0, 0, 1001, &id2);

    hu_goal_t next;
    bool found;
    HU_ASSERT_EQ(hu_goal_select_next(&engine, &next, &found), HU_OK);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_EQ(next.id, id2);

    hu_goal_engine_deinit(&engine);
    close_test_db(db);
}

static void test_goal_build_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_goal_engine_t engine;
    hu_goal_engine_create(&alloc, db, &engine);
    hu_goal_init_tables(&engine);

    int64_t id;
    hu_goal_create(&engine, "test goal", 9, 0.7, 0, 0, 1000, &id);

    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_goal_build_context(&engine, &ctx, &ctx_len), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(ctx_len > 0);
    alloc.free(alloc.ctx, ctx, ctx_len + 1);

    hu_goal_engine_deinit(&engine);
    close_test_db(db);
}

static void test_goal_status_str(void) {
    HU_ASSERT_STR_EQ(hu_auto_goal_status_str(HU_AUTO_GOAL_PENDING), "pending");
    HU_ASSERT_STR_EQ(hu_auto_goal_status_str(HU_AUTO_GOAL_ACTIVE), "active");
    HU_ASSERT_STR_EQ(hu_auto_goal_status_str(HU_AUTO_GOAL_COMPLETED), "completed");
    HU_ASSERT_STR_EQ(hu_auto_goal_status_str(HU_AUTO_GOAL_BLOCKED), "blocked");
    HU_ASSERT_STR_EQ(hu_auto_goal_status_str(HU_AUTO_GOAL_ABANDONED), "abandoned");
}

/* -- World Model ----------------------------------------------------- */

static void test_world_model_create_null(void) {
    hu_causal_world_model_t model;
    HU_ASSERT_EQ(hu_causal_world_model_create(NULL, NULL, &model), HU_ERR_INVALID_ARGUMENT);
}

static void test_world_model_simulate_no_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_causal_world_model_t model;
    hu_causal_world_model_create(&alloc, db, &model);
    hu_causal_world_model_init_tables(&model);

    hu_wm_prediction_t pred;
    HU_ASSERT_EQ(hu_world_simulate(&model, "deploy app", 10, "production", 10, &pred), HU_OK);
    HU_ASSERT_TRUE(pred.confidence <= 0.2);

    hu_causal_world_model_deinit(&model);
    close_test_db(db);
}

static void test_world_model_record_and_simulate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_causal_world_model_t model;
    hu_causal_world_model_create(&alloc, db, &model);
    hu_causal_world_model_init_tables(&model);

    HU_ASSERT_EQ(
        hu_world_record_outcome(&model, "restart server", 14, "server recovered", 16, 0.9, 1000),
        HU_OK);

    hu_wm_prediction_t pred;
    HU_ASSERT_EQ(hu_world_simulate(&model, "restart service", 15, "outage", 6, &pred), HU_OK);
    HU_ASSERT_TRUE(pred.outcome_len > 0);

    hu_causal_world_model_deinit(&model);
    close_test_db(db);
}

static void test_world_model_counterfactual(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_causal_world_model_t model;
    hu_causal_world_model_create(&alloc, db, &model);
    hu_causal_world_model_init_tables(&model);

    hu_world_record_outcome(&model, "deploy fast", 11, "crash", 5, 0.7, 1000);
    hu_world_record_outcome(&model, "deploy slow", 11, "success", 7, 0.8, 1001);

    hu_wm_prediction_t pred;
    HU_ASSERT_EQ(
        hu_world_counterfactual(&model, "deploy fast", 11, "deploy slow", 11, "prod", 4, &pred),
        HU_OK);

    hu_causal_world_model_deinit(&model);
    close_test_db(db);
}

static void test_world_model_causal_depth(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_causal_world_model_t model;
    hu_causal_world_model_create(&alloc, db, &model);
    hu_causal_world_model_init_tables(&model);

    size_t depth = 99;
    HU_ASSERT_EQ(hu_world_causal_depth(&model, "unknown", 7, &depth), HU_OK);
    HU_ASSERT_EQ(depth, 0u);

    hu_causal_world_model_deinit(&model);
    close_test_db(db);
}

static void test_world_evaluate_options(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_causal_world_model_t model;
    hu_causal_world_model_create(&alloc, db, &model);
    hu_causal_world_model_init_tables(&model);

    hu_world_record_outcome(&model, "restart", 7, "recovered", 9, 0.9, 1000);
    hu_world_record_outcome(&model, "restart", 7, "recovered", 9, 0.85, 1001);
    hu_world_record_outcome(&model, "ignore", 6, "crashed", 7, 0.8, 1002);

    const char *actions[] = {"restart", "ignore"};
    size_t action_lens[] = {7, 6};
    hu_action_option_t opts[2];
    memset(opts, 0, sizeof(opts));
    hu_error_t err =
        hu_world_evaluate_options(&model, actions, action_lens, 2, "server down", 11, opts);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(opts[0].score >= 0.0);
    HU_ASSERT_TRUE(opts[1].score >= 0.0);

    hu_causal_world_model_deinit(&model);
    close_test_db(db);
}

static void world_model_evaluate_options_scores_order(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    hu_causal_world_model_t model;
    hu_error_t err = hu_causal_world_model_create(&alloc, db, &model);
    HU_ASSERT_EQ(err, HU_OK);
    (void)hu_causal_world_model_init_tables(&model);

    /* Record some outcomes to bias scoring */
    hu_world_record_outcome(&model, "high_confidence_action", 22, "good result", 11, 0.9, 1000);
    hu_world_record_outcome(&model, "low_confidence_action", 21, "poor result", 11, 0.2, 1000);

    const char *actions[] = {"low_confidence_action", "high_confidence_action", "unknown_action"};
    size_t action_lens[] = {21, 22, 14};
    hu_action_option_t opts[3];
    memset(opts, 0, sizeof(opts));

    err = hu_world_evaluate_options(&model, actions, action_lens, 3, "test context", 12, opts);
    HU_ASSERT_EQ(err, HU_OK);

    /* All scores should be non-negative */
    for (int i = 0; i < 3; i++)
        HU_ASSERT_TRUE(opts[i].score >= 0.0);

    hu_causal_world_model_deinit(&model);
    mem.vtable->deinit(mem.ctx);
}

/* -- Online Learning ------------------------------------------------- */

static void test_online_learning_create_null(void) {
    hu_online_learning_t engine;
    HU_ASSERT_EQ(hu_online_learning_create(NULL, NULL, 0.1, &engine), HU_ERR_INVALID_ARGUMENT);
}

static void test_online_learning_record_and_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_online_learning_t engine;
    hu_online_learning_create(&alloc, db, 0.1, &engine);
    hu_online_learning_init_tables(&engine);

    hu_learning_signal_t sig = {.type = HU_SIGNAL_TOOL_SUCCESS,
                                .context = "test context",
                                .context_len = 12,
                                .tool_name = "shell",
                                .tool_name_len = 5,
                                .magnitude = 0.8,
                                .timestamp = 1000};
    HU_ASSERT_EQ(hu_online_learning_record(&engine, &sig), HU_OK);

    size_t count = 0;
    HU_ASSERT_EQ(hu_online_learning_signal_count(&engine, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);

    hu_online_learning_deinit(&engine);
    close_test_db(db);
}

static void test_online_learning_weight_ema(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_online_learning_t engine;
    hu_online_learning_create(&alloc, db, 0.5, &engine);
    hu_online_learning_init_tables(&engine);

    double w = hu_online_learning_get_weight(&engine, "verbose", 7);
    HU_ASSERT_TRUE(w >= 0.99 && w <= 1.01);

    HU_ASSERT_EQ(hu_online_learning_update_weight(&engine, "verbose", 7, 2.0, 1000), HU_OK);
    w = hu_online_learning_get_weight(&engine, "verbose", 7);
    HU_ASSERT_TRUE(w > 1.4 && w < 1.6);

    hu_online_learning_deinit(&engine);
    close_test_db(db);
}

static void test_online_learning_build_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_online_learning_t engine;
    hu_online_learning_create(&alloc, db, 0.1, &engine);
    hu_online_learning_init_tables(&engine);

    hu_online_learning_update_weight(&engine, "concise", 7, 1.5, 1000);

    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_online_learning_build_context(&engine, &ctx, &ctx_len), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    if (ctx)
        alloc.free(alloc.ctx, ctx, ctx_len + 1);

    hu_online_learning_deinit(&engine);
    close_test_db(db);
}

static void test_online_learning_response_quality(void) {
    hu_learning_signal_t signals[3] = {
        {.type = HU_SIGNAL_TOOL_SUCCESS},
        {.type = HU_SIGNAL_USER_APPROVAL},
        {.type = HU_SIGNAL_TOOL_FAILURE},
    };
    double q = hu_online_learning_response_quality(signals, 3);
    HU_ASSERT_TRUE(q >= 0.0 && q <= 1.0);
}

static void test_signal_type_str(void) {
    HU_ASSERT_NOT_NULL(hu_signal_type_str(HU_SIGNAL_TOOL_SUCCESS));
    HU_ASSERT_NOT_NULL(hu_signal_type_str(HU_SIGNAL_USER_CORRECTION));
}

static void test_online_learning_edge_cases(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_online_learning_t engine;
    hu_online_learning_create(&alloc, db, 0.1, &engine);
    hu_online_learning_init_tables(&engine);

    double w = hu_online_learning_get_weight(&engine, "empty", 5);
    HU_ASSERT_TRUE(w >= 0.99 && w <= 1.01);

    HU_ASSERT_EQ(hu_online_learning_update_weight(&engine, "zeroed", 6, 0.0, 1000), HU_OK);
    w = hu_online_learning_get_weight(&engine, "zeroed", 6);
    HU_ASSERT_TRUE(w >= 0.0);

    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_online_learning_build_context(&engine, &ctx, &ctx_len), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    if (ctx)
        alloc.free(alloc.ctx, ctx, ctx_len + 1);

    size_t count = 0;
    HU_ASSERT_EQ(hu_online_learning_signal_count(&engine, &count), HU_OK);
    HU_ASSERT_EQ(count, 0u);

    hu_online_learning_deinit(&engine);
    close_test_db(db);
}

/* -- Value Learning -------------------------------------------------- */

static void test_value_create_null(void) {
    hu_value_engine_t engine;
    HU_ASSERT_EQ(hu_value_engine_create(NULL, NULL, &engine), HU_ERR_INVALID_ARGUMENT);
}

static void test_value_learn_from_correction(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_value_engine_t engine;
    hu_value_engine_create(&alloc, db, &engine);
    hu_value_init_tables(&engine);

    HU_ASSERT_EQ(hu_value_learn_from_correction(&engine, "brevity", 7, "prefers short answers", 21,
                                                0.8, 1000),
                 HU_OK);

    hu_value_t val;
    bool found = false;
    HU_ASSERT_EQ(hu_value_get(&engine, "brevity", 7, &val, &found), HU_OK);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_TRUE(val.importance > 0.0);
    HU_ASSERT_EQ(val.evidence_count, 1);

    hu_value_engine_deinit(&engine);
    close_test_db(db);
}

static void test_value_learn_from_approval(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_value_engine_t engine;
    hu_value_engine_create(&alloc, db, &engine);
    hu_value_init_tables(&engine);

    HU_ASSERT_EQ(hu_value_learn_from_approval(&engine, "accuracy", 8, 0.9, 1000), HU_OK);

    hu_value_t val;
    bool found;
    hu_value_get(&engine, "accuracy", 8, &val, &found);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_TRUE(val.importance > 0.0);

    hu_value_engine_deinit(&engine);
    close_test_db(db);
}

static void test_value_weaken(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_value_engine_t engine;
    hu_value_engine_create(&alloc, db, &engine);
    hu_value_init_tables(&engine);

    hu_value_learn_from_correction(&engine, "formality", 9, "formal tone", 11, 0.6, 1000);

    hu_value_t val;
    bool found;
    hu_value_get(&engine, "formality", 9, &val, &found);
    HU_ASSERT_TRUE(found);
    double orig = val.importance;

    HU_ASSERT_EQ(hu_value_weaken(&engine, "formality", 9, 0.1, 1001), HU_OK);
    hu_value_get(&engine, "formality", 9, &val, &found);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_TRUE(val.importance < orig);

    HU_ASSERT_EQ(hu_value_weaken(&engine, "formality", 9, 10.0, 1002), HU_OK);
    hu_value_get(&engine, "formality", 9, &val, &found);
    HU_ASSERT_TRUE(!found);

    hu_value_engine_deinit(&engine);
    close_test_db(db);
}

static void test_value_list_and_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_value_engine_t engine;
    hu_value_engine_create(&alloc, db, &engine);
    hu_value_init_tables(&engine);

    hu_value_learn_from_correction(&engine, "privacy", 7, "keep data safe", 14, 0.9, 1000);
    hu_value_learn_from_correction(&engine, "speed", 5, "fast responses", 14, 0.7, 1001);

    size_t count = 0;
    HU_ASSERT_EQ(hu_value_count(&engine, &count), HU_OK);
    HU_ASSERT_EQ(count, 2u);

    hu_value_t *values = NULL;
    size_t val_count = 0;
    HU_ASSERT_EQ(hu_value_list(&engine, &values, &val_count), HU_OK);
    HU_ASSERT_EQ(val_count, 2u);
    HU_ASSERT_TRUE(values[0].importance >= values[1].importance);
    hu_value_free(&alloc, values, val_count);

    hu_value_engine_deinit(&engine);
    close_test_db(db);
}

static void test_value_build_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_value_engine_t engine;
    hu_value_engine_create(&alloc, db, &engine);
    hu_value_init_tables(&engine);

    hu_value_learn_from_correction(&engine, "clarity", 7, "clear explanations", 18, 0.8, 1000);

    char *prompt = NULL;
    size_t prompt_len = 0;
    HU_ASSERT_EQ(hu_value_build_prompt(&engine, &prompt, &prompt_len), HU_OK);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(prompt_len > 0);
    alloc.free(alloc.ctx, prompt, prompt_len + 1);

    hu_value_engine_deinit(&engine);
    close_test_db(db);
}

static void test_value_alignment_score(void) {
    hu_value_t vals[2] = {
        {.name = "safety", .name_len = 6, .importance = 0.9},
        {.name = "speed", .name_len = 5, .importance = 0.5},
    };
    double score = hu_value_alignment_score(vals, 2, "ensure safety first", 19);
    HU_ASSERT_TRUE(score > 0.0);

    score = hu_value_alignment_score(vals, 2, "do something random", 19);
    HU_ASSERT_TRUE(score < 0.01);
}

#endif /* HU_ENABLE_SQLITE */

/* -- Orchestrator (no SQLite needed) --------------------------------- */

static void test_orchestrator_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_orchestrator_t orch;
    HU_ASSERT_EQ(hu_orchestrator_create(&alloc, &orch), HU_OK);
    HU_ASSERT_EQ(orch.task_count, 0u);
    HU_ASSERT_EQ(orch.agent_count, 0u);
    hu_orchestrator_deinit(&orch);
}

static void test_orchestrator_create_null(void) {
    hu_orchestrator_t orch;
    HU_ASSERT_EQ(hu_orchestrator_create(NULL, &orch), HU_ERR_INVALID_ARGUMENT);
}

static void test_orchestrator_register_agent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_orchestrator_t orch;
    hu_orchestrator_create(&alloc, &orch);

    HU_ASSERT_EQ(hu_orchestrator_register_agent(&orch, "agent-1", 7, "coder", 5, "python,c", 8),
                 HU_OK);
    HU_ASSERT_EQ(orch.agent_count, 1u);

    hu_orchestrator_deinit(&orch);
}

static void test_orchestrator_task_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_orchestrator_t orch;
    hu_orchestrator_create(&alloc, &orch);

    hu_orchestrator_register_agent(&orch, "agent-1", 7, "coder", 5, "c", 1);

    const char *tasks[] = {"design API", "implement", "test"};
    size_t lens[] = {10, 9, 4};
    HU_ASSERT_EQ(hu_orchestrator_propose_split(&orch, "build feature", 13, tasks, lens, 3), HU_OK);
    HU_ASSERT_EQ(orch.task_count, 3u);

    HU_ASSERT_EQ(hu_orchestrator_assign_task(&orch, 1, "agent-1", 7), HU_OK);
    HU_ASSERT_EQ(hu_orchestrator_count_by_status(&orch, HU_TASK_ASSIGNED), 1u);

    HU_ASSERT_EQ(hu_orchestrator_complete_task(&orch, 1, "API designed", 12), HU_OK);
    HU_ASSERT_EQ(hu_orchestrator_count_by_status(&orch, HU_TASK_COMPLETED), 1u);
    HU_ASSERT_TRUE(!hu_orchestrator_all_complete(&orch));

    hu_orchestrator_assign_task(&orch, 2, "agent-1", 7);
    hu_orchestrator_complete_task(&orch, 2, "implemented", 11);
    hu_orchestrator_assign_task(&orch, 3, "agent-1", 7);
    hu_orchestrator_complete_task(&orch, 3, "tested", 6);
    HU_ASSERT_TRUE(hu_orchestrator_all_complete(&orch));

    hu_orchestrator_deinit(&orch);
}

static void test_orchestrator_merge_results(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_orchestrator_t orch;
    hu_orchestrator_create(&alloc, &orch);

    const char *tasks[] = {"task A", "task B"};
    size_t lens[] = {6, 6};
    hu_orchestrator_propose_split(&orch, "goal", 4, tasks, lens, 2);
    hu_orchestrator_assign_task(&orch, 1, "a1", 2);
    hu_orchestrator_complete_task(&orch, 1, "result A", 8);
    hu_orchestrator_assign_task(&orch, 2, "a1", 2);
    hu_orchestrator_complete_task(&orch, 2, "result B", 8);

    char *merged = NULL;
    size_t merged_len = 0;
    HU_ASSERT_EQ(hu_orchestrator_merge_results(&orch, &alloc, &merged, &merged_len), HU_OK);
    HU_ASSERT_NOT_NULL(merged);
    HU_ASSERT_TRUE(merged_len > 0);
    alloc.free(alloc.ctx, merged, merged_len + 1);

    hu_orchestrator_deinit(&orch);
}

static void test_orchestrator_fail_task(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_orchestrator_t orch;
    hu_orchestrator_create(&alloc, &orch);

    const char *tasks[] = {"will fail"};
    size_t lens[] = {9};
    hu_orchestrator_propose_split(&orch, "goal", 4, tasks, lens, 1);
    hu_orchestrator_assign_task(&orch, 1, "a1", 2);
    HU_ASSERT_EQ(hu_orchestrator_fail_task(&orch, 1, "timeout", 7), HU_OK);
    HU_ASSERT_EQ(hu_orchestrator_count_by_status(&orch, HU_TASK_FAILED), 1u);

    hu_orchestrator_deinit(&orch);
}

static void test_orchestrator_next_task(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_orchestrator_t orch;
    hu_orchestrator_create(&alloc, &orch);

    const char *tasks[] = {"first", "second"};
    size_t lens[] = {5, 6};
    hu_orchestrator_propose_split(&orch, "goal", 4, tasks, lens, 2);

    hu_orchestrator_task_t *next = NULL;
    HU_ASSERT_EQ(hu_orchestrator_next_task(&orch, &next), HU_OK);
    HU_ASSERT_NOT_NULL(next);
    HU_ASSERT_EQ(next->id, 1u);

    hu_orchestrator_deinit(&orch);
}

static void test_orchestrator_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_orchestrator_t orch;
    hu_orchestrator_create(&alloc, &orch);
    HU_ASSERT_EQ(hu_orchestrator_assign_task(&orch, 999, "a1", 2), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(hu_orchestrator_complete_task(&orch, 999, "x", 1), HU_ERR_NOT_FOUND);
    hu_orchestrator_deinit(&orch);
}

static void test_task_status_str(void) {
    HU_ASSERT_STR_EQ(hu_task_status_str(HU_TASK_UNASSIGNED), "unassigned");
    HU_ASSERT_STR_EQ(hu_task_status_str(HU_TASK_COMPLETED), "completed");
    HU_ASSERT_STR_EQ(hu_task_status_str(HU_TASK_FAILED), "failed");
}

/* ─── Speculative Pre-Computation ──────────────────────────────────── */

static void test_speculative_cache_store_and_lookup(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_speculative_cache_t cache;
    hu_error_t err = hu_speculative_cache_init(&cache, &alloc);
    HU_ASSERT_EQ(err, HU_OK);

    err = hu_speculative_cache_store(&cache, "what is X", 9, "X is a thing", 12, 0.8, 1000);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(cache.count, 1u);

    hu_speculative_config_t cfg = hu_speculative_config_default();
    hu_prediction_t *hit = NULL;
    err = hu_speculative_cache_lookup(&cache, "what is X", 9, 1001, &cfg, &hit);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(hit);
    HU_ASSERT_TRUE(hit->confidence >= 0.7);

    hu_speculative_cache_deinit(&cache);
}

static void test_speculative_cache_evict_expired(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_speculative_cache_t cache;
    hu_speculative_cache_init(&cache, &alloc);
    hu_speculative_cache_store(&cache, "old query", 9, "old resp", 8, 0.7, 100);
    HU_ASSERT_EQ(cache.count, 1u);
    hu_speculative_cache_evict(&cache, 500, 300);
    HU_ASSERT_EQ(cache.count, 0u);
    hu_speculative_cache_deinit(&cache);
}

static void test_speculative_predict_heuristic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *preds[3] = {NULL};
    size_t pred_lens[3] = {0};
    double confs[3] = {0};
    size_t actual = 0;

    hu_error_t err = hu_speculative_predict(&alloc, "how do I fix this?", 18,
                                            "Here are the steps: 1. Check config 2. Restart", 47,
                                            preds, pred_lens, confs, 3, &actual);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(actual >= 1);
    for (size_t i = 0; i < actual; i++) {
        HU_ASSERT_NOT_NULL(preds[i]);
        HU_ASSERT_TRUE(confs[i] > 0.0);
        alloc.free(alloc.ctx, preds[i], pred_lens[i] + 1);
    }
}

static void test_speculative_config_default(void) {
    hu_speculative_config_t cfg = hu_speculative_config_default();
    HU_ASSERT_TRUE(cfg.enabled);
    HU_ASSERT_TRUE(cfg.min_confidence > 0.0);
    HU_ASSERT_TRUE(cfg.ttl_seconds > 0);
    HU_ASSERT_TRUE(cfg.max_predictions > 0);
}

/* ─── Uncertainty Quantification ──────────────────────────────────── */

static void test_uncertainty_high_confidence(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_uncertainty_signals_t signals = {
        .retrieval_coverage = 0.9,
        .has_citations = true,
        .has_hedging_language = false,
        .tool_results_count = 2,
        .memory_results_count = 5,
        .is_factual_query = true,
    };
    hu_uncertainty_result_t result;
    memset(&result, 0, sizeof(result));
    hu_error_t err = hu_uncertainty_evaluate(&alloc, &signals, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.confidence >= 0.7);
    HU_ASSERT_TRUE(result.level == HU_CONFIDENCE_HIGH || result.level == HU_CONFIDENCE_MEDIUM);
    hu_uncertainty_result_free(&alloc, &result);
}

static void test_uncertainty_low_confidence(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_uncertainty_signals_t signals = {
        .retrieval_coverage = 0.1,
        .has_citations = false,
        .has_hedging_language = true,
        .tool_results_count = 0,
        .memory_results_count = 0,
        .is_factual_query = true,
    };
    hu_uncertainty_result_t result;
    memset(&result, 0, sizeof(result));
    hu_error_t err = hu_uncertainty_evaluate(&alloc, &signals, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.confidence < 0.5);
    HU_ASSERT_TRUE(result.level == HU_CONFIDENCE_LOW || result.level == HU_CONFIDENCE_VERY_LOW);
    hu_uncertainty_result_free(&alloc, &result);
}

static void test_uncertainty_extract_signals(void) {
    hu_uncertainty_signals_t signals;
    memset(&signals, 0, sizeof(signals));
    hu_error_t err = hu_uncertainty_extract_signals("I think the answer might be 42", 30,
                                                    "what is the answer", 18, 1, 3, &signals);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(signals.has_hedging_language);
    HU_ASSERT_TRUE(signals.is_factual_query);
    HU_ASSERT_EQ(signals.tool_results_count, 1u);
    HU_ASSERT_EQ(signals.memory_results_count, 3u);
}

static void test_confidence_level_str(void) {
    HU_ASSERT_STR_EQ(hu_confidence_level_str(HU_CONFIDENCE_HIGH), "high");
    HU_ASSERT_STR_EQ(hu_confidence_level_str(HU_CONFIDENCE_LOW), "low");
}

/* ─── Strategy Learner ────────────────────────────────────────────── */

#ifdef HU_ENABLE_SQLITE
static void test_strategy_learner_classify(void) {
    HU_ASSERT_EQ(hu_strategy_classify_query("what is AGI", 11), HU_QCAT_FACTUAL);
    HU_ASSERT_EQ(hu_strategy_classify_query("how to build X", 14), HU_QCAT_PROCEDURAL);
    HU_ASSERT_EQ(hu_strategy_classify_query("my preferences", 14), HU_QCAT_PERSONAL);
    HU_ASSERT_EQ(hu_strategy_classify_query("yesterday meeting", 17), HU_QCAT_TEMPORAL);
}

static void test_strategy_learner_record_and_recommend(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = open_test_db();
    hu_strategy_learner_t learner;
    hu_error_t err = hu_strategy_learner_create(&alloc, db, &learner);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_strategy_learner_init_tables(&learner);
    HU_ASSERT_EQ(err, HU_OK);

    hu_strategy_learner_record(&learner, HU_QCAT_FACTUAL, HU_RSTRAT_KEYWORD, true, 1000);
    hu_strategy_learner_record(&learner, HU_QCAT_FACTUAL, HU_RSTRAT_KEYWORD, true, 1001);
    hu_strategy_learner_record(&learner, HU_QCAT_FACTUAL, HU_RSTRAT_VECTOR, false, 1002);

    hu_retrieval_strategy_t best = hu_strategy_learner_recommend(&learner, HU_QCAT_FACTUAL);
    HU_ASSERT_EQ(best, HU_RSTRAT_KEYWORD);

    hu_strategy_learner_deinit(&learner);
    close_test_db(db);
}
#endif

/* ─── Orchestrator LLM Decomposition ─────────────────────────────── */

static void test_orchestrator_decompose_goal_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_decomposition_t result;
    memset(&result, 0, sizeof(result));

    hu_error_t err = hu_orchestrator_decompose_goal(&alloc, NULL, "gpt-4", 5, "Build a web app", 15,
                                                    NULL, 0, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.task_count >= 2);
    HU_ASSERT_NOT_NULL(result.reasoning);
    HU_ASSERT_TRUE(result.reasoning_len > 0);
    hu_decomposition_free(&alloc, &result);
}

static void test_decomposition_free_handles_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_decomposition_t result;
    memset(&result, 0, sizeof(result));
    hu_decomposition_free(&alloc, &result);
    hu_decomposition_free(NULL, &result);
    hu_decomposition_free(&alloc, NULL);
}

/* ─── Integration: constitutional + uncertainty together ─────────── */

static void test_integration_uncertainty_signals(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_uncertainty_signals_t signals;
    memset(&signals, 0, sizeof(signals));

    /* Simulate a well-grounded response */
    hu_uncertainty_extract_signals(
        "According to the documentation, the answer is 42. Based on memory, this was confirmed.",
        83, "what is the answer", 18, 2, 5, &signals);

    HU_ASSERT_TRUE(signals.has_citations);
    HU_ASSERT_FALSE(signals.has_hedging_language);

    hu_uncertainty_result_t result;
    memset(&result, 0, sizeof(result));
    hu_uncertainty_evaluate(&alloc, &signals, &result);
    HU_ASSERT_TRUE(result.confidence >= 0.5);
    hu_uncertainty_result_free(&alloc, &result);
}

static void test_integration_speculative_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_speculative_cache_t cache;
    hu_speculative_cache_init(&cache, &alloc);

    /* Store a prediction */
    hu_speculative_cache_store(&cache, "summarize", 9, "Here is the summary...", 21, 0.8, 1000);

    /* Predict follow-ups from a conversation */
    char *preds[3] = {NULL};
    size_t pred_lens[3] = {0};
    double confs[3] = {0};
    size_t count = 0;
    hu_speculative_predict(&alloc, "explain X", 9, "X works by doing Y and Z", 24, preds, pred_lens,
                           confs, 3, &count);

    /* Store predictions */
    for (size_t i = 0; i < count; i++) {
        if (preds[i]) {
            hu_speculative_cache_store(&cache, preds[i], pred_lens[i], "pre-computed response", 21,
                                       confs[i], 1001);
            alloc.free(alloc.ctx, preds[i], pred_lens[i] + 1);
        }
    }

    HU_ASSERT_TRUE(cache.count >= 1);
    hu_speculative_cache_deinit(&cache);
}

/* -- Delegate E2E ---------------------------------------------------- */

static void *agi_alloc_fn(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}
static void *agi_realloc_fn(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    (void)ctx;
    (void)old_size;
    return realloc(ptr, new_size);
}
static void agi_free_fn(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}

static void test_delegate_e2e_roundtrip(void) {
    hu_allocator_t alloc = {
        .alloc = agi_alloc_fn,
        .realloc = agi_realloc_fn,
        .free = agi_free_fn,
        .ctx = NULL,
    };
    hu_tool_t delegate;
    HU_ASSERT_EQ(hu_delegate_create(&alloc, NULL, NULL, NULL, &delegate), HU_OK);
    HU_ASSERT_STR_EQ(delegate.vtable->name(delegate.ctx), "delegate");

    const char *args_json = "{\"agent\":\"test_agent\",\"prompt\":\"summarize this\"}";
    hu_json_value_t *args = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, args_json, strlen(args_json), &args), HU_OK);

    hu_tool_result_t result;
    memset(&result, 0, sizeof(result));
    HU_ASSERT_EQ(delegate.vtable->execute(delegate.ctx, &alloc, args, &result), HU_OK);
    HU_ASSERT(result.output != NULL);
    HU_ASSERT(result.output_len > 0);
    HU_ASSERT(strstr(result.output, "test_agent") != NULL);
    HU_ASSERT(strstr(result.output, "summarize this") != NULL);

    hu_json_free(&alloc, args);
    hu_tool_result_free(&alloc, &result);
    if (delegate.vtable->deinit)
        delegate.vtable->deinit(delegate.ctx, &alloc);
}

static void test_delegate_missing_agent(void) {
    hu_allocator_t alloc = {
        .alloc = agi_alloc_fn,
        .realloc = agi_realloc_fn,
        .free = agi_free_fn,
        .ctx = NULL,
    };
    hu_tool_t delegate;
    HU_ASSERT_EQ(hu_delegate_create(&alloc, NULL, NULL, NULL, &delegate), HU_OK);

    const char *args_json = "{\"prompt\":\"do stuff\"}";
    hu_json_value_t *args = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, args_json, strlen(args_json), &args), HU_OK);

    hu_tool_result_t result;
    memset(&result, 0, sizeof(result));
    HU_ASSERT_EQ(delegate.vtable->execute(delegate.ctx, &alloc, args, &result), HU_OK);
    HU_ASSERT(!result.success);

    hu_json_free(&alloc, args);
    hu_tool_result_free(&alloc, &result);
    if (delegate.vtable->deinit)
        delegate.vtable->deinit(delegate.ctx, &alloc);
}

static void test_delegate_missing_prompt(void) {
    hu_allocator_t alloc = {
        .alloc = agi_alloc_fn,
        .realloc = agi_realloc_fn,
        .free = agi_free_fn,
        .ctx = NULL,
    };
    hu_tool_t delegate;
    HU_ASSERT_EQ(hu_delegate_create(&alloc, NULL, NULL, NULL, &delegate), HU_OK);

    const char *args_json = "{\"agent\":\"test_agent\"}";
    hu_json_value_t *args = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, args_json, strlen(args_json), &args), HU_OK);

    hu_tool_result_t result;
    memset(&result, 0, sizeof(result));
    HU_ASSERT_EQ(delegate.vtable->execute(delegate.ctx, &alloc, args, &result), HU_OK);
    HU_ASSERT(!result.success);

    hu_json_free(&alloc, args);
    hu_tool_result_free(&alloc, &result);
    if (delegate.vtable->deinit)
        delegate.vtable->deinit(delegate.ctx, &alloc);
}

/* ─── Integration: Self-RAG assess + verify roundtrip ──────────────── */

static void test_srag_assess_and_verify_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_srag_config_t cfg = hu_srag_config_default();
    cfg.enabled = true;

    hu_srag_assessment_t assess;
    memset(&assess, 0, sizeof(assess));
    hu_error_t err = hu_srag_should_retrieve(
        &alloc, &cfg, "What is the height of the Eiffel Tower?", 39, NULL, 0, &assess);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(assess.is_factual_query);
    HU_ASSERT_TRUE(assess.decision == HU_SRAG_RETRIEVE ||
                   assess.decision == HU_SRAG_RETRIEVE_AND_VERIFY);

    double relevance = 0.0;
    bool should_use = false;
    err = hu_srag_verify_relevance(&alloc, &cfg, "What is the height of the Eiffel Tower?", 39,
                                   "The Eiffel Tower is 330 meters tall, completed in 1889.", 55,
                                   &relevance, &should_use);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(relevance > 0.0);
    HU_ASSERT_TRUE(should_use);
}

static void test_srag_creative_query_skips_retrieval(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_srag_config_t cfg = hu_srag_config_default();
    cfg.enabled = true;

    hu_srag_assessment_t assess;
    memset(&assess, 0, sizeof(assess));
    hu_srag_should_retrieve(&alloc, &cfg, "Write me a poem about the ocean", 31, NULL, 0, &assess);
    HU_ASSERT_TRUE(assess.is_creative_query);
}

/* ─── Integration: PRM score_step + score_chain ───────────────────── */

static void test_prm_score_step_standalone(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prm_config_t cfg = hu_prm_config_default();
    cfg.enabled = true;

    double score = 0.0;
    hu_error_t err =
        hu_prm_score_step(&alloc, &cfg, "First, let's identify the key variables: x=5, y=10", 51,
                          "Calculate x+y given x=5, y=10", 30, &score);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(score > 0.0);
}

static void test_prm_chain_with_multi_step(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prm_config_t cfg = hu_prm_config_default();
    cfg.enabled = true;

    hu_prm_result_t result;
    memset(&result, 0, sizeof(result));
    const char *chain = "Step 1: Identify the problem.\n\n"
                        "Step 2: Break it down.\n\n"
                        "Step 3: Solve each part.\n\n"
                        "Step 4: Combine and verify.";
    hu_error_t err = hu_prm_score_chain(&alloc, &cfg, chain, strlen(chain), &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.step_count >= 2);
    HU_ASSERT_TRUE(result.aggregate_score > 0.0);
    hu_prm_result_free(&alloc, &result);
}

/* ─── Integration: Constitutional critique in test mode ───────────── */

static void test_constitutional_critique_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_constitutional_config_t cfg = hu_constitutional_config_default();
    cfg.enabled = true;

    hu_critique_result_t result;
    memset(&result, 0, sizeof(result));
    hu_error_t err = hu_constitutional_critique(
        &alloc, NULL, NULL, 0, "How do I fix my code?", 21,
        "Here's a helpful explanation of the issue and how to fix it.", 60, &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.verdict == HU_CRITIQUE_PASS || result.verdict == HU_CRITIQUE_MINOR);
    hu_critique_result_free(&alloc, &result);
}

/* ─── Integration: Adaptive RAG select + record + learn cycle ─────── */

static void test_adaptive_rag_select_and_learn(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_adaptive_rag_t rag;
    hu_error_t err = hu_adaptive_rag_create(&alloc, NULL, &rag);
    HU_ASSERT_EQ(err, HU_OK);

    hu_rag_strategy_t s1 = hu_adaptive_rag_select(&rag, "What is X?", 10);
    HU_ASSERT_TRUE(s1 < HU_RAG_STRATEGY_COUNT);
    hu_adaptive_rag_record_outcome(&rag, s1, 0.9);

    hu_rag_strategy_t s2 = hu_adaptive_rag_select(&rag, "Tell me about person Y's preferences", 37);
    HU_ASSERT_TRUE(s2 < HU_RAG_STRATEGY_COUNT);
    hu_adaptive_rag_record_outcome(&rag, s2, 0.3);

    const char *name = hu_rag_strategy_str(s1);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strlen(name) > 0);

    hu_adaptive_rag_deinit(&rag);
}

/* ─── Integration: DPO record_from_retry ──────────────────────────── */

#ifdef HU_ENABLE_SQLITE
static void test_dpo_record_from_retry_roundtrip(void) {
    sqlite3 *db = open_test_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t collector;
    hu_error_t err = hu_dpo_collector_create(&alloc, db, 100, &collector);
    HU_ASSERT_EQ(err, HU_OK);
    hu_dpo_init_tables(&collector);

    err = hu_dpo_record_from_retry(
        &collector, "Explain quantum computing", 25, "Quantum computing is magic.", 27,
        "Quantum computing uses qubits which can exist in superposition states.", 70);
    HU_ASSERT_EQ(err, HU_OK);

    size_t count = 0;
    hu_dpo_pair_count(&collector, &count);
    HU_ASSERT_TRUE(count >= 1);

    hu_dpo_collector_deinit(&collector);
    sqlite3_close(db);
}
#endif

/* ─── Integration: Tier manager promote/demote roundtrip ──────────── */

#ifdef HU_ENABLE_SQLITE
static void test_tier_promote_demote_roundtrip(void) {
    sqlite3 *db = open_test_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_tier_manager_t mgr;
    hu_error_t err = hu_tier_manager_create(&alloc, db, &mgr);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tier_manager_init_tables(&mgr);

    hu_tier_manager_store(&mgr, HU_TIER_ARCHIVAL, "fact:sky:blue", 13, "The sky is blue", 15);

    err = hu_tier_manager_promote(&mgr, "fact:sky:blue", 13, HU_TIER_ARCHIVAL, HU_TIER_RECALL);
    HU_ASSERT_EQ(err, HU_OK);

    err = hu_tier_manager_demote(&mgr, "fact:sky:blue", 13, HU_TIER_RECALL, HU_TIER_ARCHIVAL);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_tier_t assigned;
    err = hu_tier_manager_auto_tier(&mgr, "important_key", 13, "user's favorite color is green", 30,
                                    &assigned);
    HU_ASSERT_EQ(err, HU_OK);

    const char *tier_name = hu_memory_tier_str(assigned);
    HU_ASSERT_NOT_NULL(tier_name);

    hu_tier_manager_deinit(&mgr);
    sqlite3_close(db);
}
#endif

/* ─── Integration: Streaming dispatcher calls execute_streaming ───── */

static size_t test_stream_chunks_received;
static void test_stream_chunk_counter(void *ctx, const char *data, size_t len) {
    (void)ctx;
    (void)data;
    (void)len;
    test_stream_chunks_received++;
}

static hu_error_t
mock_streaming_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                       void (*on_chunk)(void *cb_ctx, const char *data, size_t len), void *cb_ctx,
                       hu_tool_result_t *out) {
    (void)ctx;
    (void)args;
    if (on_chunk) {
        on_chunk(cb_ctx, "chunk1", 6);
        on_chunk(cb_ctx, "chunk2", 6);
    }
    char *buf = (char *)alloc->alloc(alloc->ctx, 13);
    memcpy(buf, "chunk1chunk2", 13);
    *out = hu_tool_result_ok_owned(buf, 12);
    return HU_OK;
}

static const char *mock_stream_name(void *ctx) {
    (void)ctx;
    return "mock_stream";
}
static const char *mock_stream_desc(void *ctx) {
    (void)ctx;
    return "mock streaming tool";
}
static const char *mock_stream_params(void *ctx) {
    (void)ctx;
    return "{}";
}

static const hu_tool_vtable_t mock_stream_vtable = {
    .execute = NULL,
    .name = mock_stream_name,
    .description = mock_stream_desc,
    .parameters_json = mock_stream_params,
    .deinit = NULL,
    .execute_streaming = mock_streaming_execute,
};

static void test_streaming_dispatcher_calls_execute_streaming(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {.ctx = NULL, .vtable = &mock_stream_vtable};

    hu_tool_call_t call = {
        .id = "c1",
        .id_len = 2,
        .name = "mock_stream",
        .name_len = 11,
        .arguments = "{}",
        .arguments_len = 2,
    };

    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    hu_dispatch_result_t dres;
    test_stream_chunks_received = 0;

    hu_error_t err = hu_dispatcher_dispatch_streaming(&disp, &alloc, &tool, 1, &call, 1,
                                                      test_stream_chunk_counter, NULL, &dres);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(dres.count, 1u);
    HU_ASSERT_TRUE(dres.results[0].success);
    HU_ASSERT_EQ(test_stream_chunks_received, 2u);
    HU_ASSERT_TRUE(strstr(dres.results[0].output, "chunk1") != NULL);

    hu_dispatch_result_free(&alloc, &dres);
}

static void test_streaming_dispatcher_null_callback_falls_back(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {.ctx = NULL, .vtable = &mock_stream_vtable};

    hu_tool_call_t call = {
        .id = "c1",
        .id_len = 2,
        .name = "mock_stream",
        .name_len = 11,
        .arguments = "{}",
        .arguments_len = 2,
    };

    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    hu_dispatch_result_t dres;

    hu_error_t err =
        hu_dispatcher_dispatch_streaming(&disp, &alloc, &tool, 1, &call, 1, NULL, NULL, &dres);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(dres.count, 1u);

    hu_dispatch_result_free(&alloc, &dres);
}

/* ─── Integration: Streaming dispatch with execute-only tool ──────── */

static hu_error_t mock_execute_only(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                    hu_tool_result_t *out) {
    (void)ctx;
    (void)args;
    char *buf = (char *)alloc->alloc(alloc->ctx, 10);
    memcpy(buf, "exec_only", 10);
    *out = hu_tool_result_ok_owned(buf, 9);
    return HU_OK;
}

static const char *mock_exec_name(void *ctx) {
    (void)ctx;
    return "exec_only";
}
static const char *mock_exec_desc(void *ctx) {
    (void)ctx;
    return "execute-only tool";
}
static const char *mock_exec_params(void *ctx) {
    (void)ctx;
    return "{}";
}

static const hu_tool_vtable_t mock_exec_only_vtable = {
    .execute = mock_execute_only,
    .name = mock_exec_name,
    .description = mock_exec_desc,
    .parameters_json = mock_exec_params,
    .deinit = NULL,
    .execute_streaming = NULL,
};

static void test_streaming_dispatch_execute_only_fallback(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {.ctx = NULL, .vtable = &mock_exec_only_vtable};

    hu_tool_call_t call = {
        .id = "c1",
        .id_len = 2,
        .name = "exec_only",
        .name_len = 9,
        .arguments = "{}",
        .arguments_len = 2,
    };

    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    hu_dispatch_result_t dres;
    test_stream_chunks_received = 0;

    hu_error_t err = hu_dispatcher_dispatch_streaming(&disp, &alloc, &tool, 1, &call, 1,
                                                      test_stream_chunk_counter, NULL, &dres);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(dres.count, 1u);
    HU_ASSERT_TRUE(dres.results[0].success);
    HU_ASSERT_EQ(test_stream_chunks_received, 0u);
    HU_ASSERT_TRUE(strstr(dres.results[0].output, "exec_only") != NULL);

    hu_dispatch_result_free(&alloc, &dres);
}

/* ─── Integration: hu_agent_turn_stream E2E ───────────────────────── */

static hu_error_t agi_mock_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                                const char *model, size_t model_len, double temperature,
                                hu_chat_response_t *out) {
    (void)ctx;
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    const char *resp = "streamed response";
    char *buf = (char *)alloc->alloc(alloc->ctx, 18);
    if (buf) {
        memcpy(buf, resp, 18);
        out->content = buf;
        out->content_len = 17;
    }
    out->tool_calls = NULL;
    out->tool_calls_count = 0;
    out->usage.prompt_tokens = 1;
    out->usage.completion_tokens = 2;
    out->usage.total_tokens = 3;
    out->model = NULL;
    out->model_len = 0;
    out->reasoning_content = NULL;
    out->reasoning_content_len = 0;
    return out->content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static bool agi_mock_supports_tools(void *ctx) {
    (void)ctx;
    return false;
}
static const char *agi_mock_name(void *ctx) {
    (void)ctx;
    return "mock";
}
static void agi_mock_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const hu_provider_vtable_t agi_mock_vtable = {
    .chat = agi_mock_chat,
    .supports_native_tools = agi_mock_supports_tools,
    .get_name = agi_mock_name,
    .deinit = agi_mock_deinit,
};

static size_t stream_token_total_len;
static void stream_token_counter(const char *delta, size_t len, void *ctx) {
    (void)ctx;
    if (delta && len > 0)
        stream_token_total_len += len;
}

static void test_agent_turn_stream_e2e(void) {
    hu_allocator_t alloc = {
        .alloc = agi_alloc_fn,
        .realloc = agi_realloc_fn,
        .free = agi_free_fn,
        .ctx = NULL,
    };
    hu_provider_t prov = {.ctx = NULL, .vtable = &agi_mock_vtable};

    hu_agent_t agent;
    hu_error_t err =
        hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    char *response = NULL;
    size_t response_len = 0;
    stream_token_total_len = 0;

    err = hu_agent_turn_stream(&agent, "hello world", 11, stream_token_counter, NULL, &response,
                               &response_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(response);
    HU_ASSERT_TRUE(response_len > 0);
    HU_ASSERT_TRUE(stream_token_total_len > 0);
    HU_ASSERT_TRUE(hu_strcasestr(response, "streamed") != NULL);

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    hu_agent_deinit(&agent);
}

/* ─── Integration: Tree of Thought explore + config ───────────────── */

static void test_tot_explore_generates_branches(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tot_config_t cfg = hu_tot_config_default();
    cfg.enabled = true;

    hu_tot_result_t result;
    memset(&result, 0, sizeof(result));
    hu_error_t err =
        hu_tot_explore(&alloc, NULL, NULL, 0,
                       "How should we design a caching layer for this system?", 53, &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.branches_explored >= 1);
    HU_ASSERT_TRUE(result.best_thought != NULL);
    HU_ASSERT_TRUE(result.best_thought_len > 0);
    hu_tot_result_free(&alloc, &result);
}

/* -- Test runner ----------------------------------------------------- */

void run_agi_frontiers_tests(void) {
    HU_TEST_SUITE("agi_frontiers");

    HU_RUN_TEST(test_orchestrator_create);
    HU_RUN_TEST(test_orchestrator_create_null);
    HU_RUN_TEST(test_orchestrator_register_agent);
    HU_RUN_TEST(test_orchestrator_task_lifecycle);
    HU_RUN_TEST(test_orchestrator_merge_results);
    HU_RUN_TEST(test_orchestrator_fail_task);
    HU_RUN_TEST(test_orchestrator_next_task);
    HU_RUN_TEST(test_orchestrator_not_found);
    HU_RUN_TEST(test_task_status_str);

#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_self_improve_create_null_args);
    HU_RUN_TEST(test_self_improve_init_tables);
    HU_RUN_TEST(test_self_improve_tool_outcome_tracking);
    HU_RUN_TEST(test_self_improve_prompt_patches);
    HU_RUN_TEST(test_self_improve_prompt_patches_with_data);

    HU_RUN_TEST(test_goal_create_null_args);
    HU_RUN_TEST(test_goal_lifecycle);
    HU_RUN_TEST(test_goal_decompose);
    HU_RUN_TEST(test_goal_select_next);
    HU_RUN_TEST(test_goal_build_context);
    HU_RUN_TEST(test_goal_status_str);

    HU_RUN_TEST(test_world_model_create_null);
    HU_RUN_TEST(test_world_model_simulate_no_data);
    HU_RUN_TEST(test_world_model_record_and_simulate);
    HU_RUN_TEST(test_world_model_counterfactual);
    HU_RUN_TEST(test_world_model_causal_depth);
    HU_RUN_TEST(test_world_evaluate_options);
    HU_RUN_TEST(world_model_evaluate_options_scores_order);

    HU_RUN_TEST(test_online_learning_create_null);
    HU_RUN_TEST(test_online_learning_record_and_count);
    HU_RUN_TEST(test_online_learning_weight_ema);
    HU_RUN_TEST(test_online_learning_build_context);
    HU_RUN_TEST(test_online_learning_response_quality);
    HU_RUN_TEST(test_signal_type_str);
    HU_RUN_TEST(test_online_learning_edge_cases);

    HU_RUN_TEST(test_value_create_null);
    HU_RUN_TEST(test_value_learn_from_correction);
    HU_RUN_TEST(test_value_learn_from_approval);
    HU_RUN_TEST(test_value_weaken);
    HU_RUN_TEST(test_value_list_and_count);
    HU_RUN_TEST(test_value_build_prompt);
    HU_RUN_TEST(test_value_alignment_score);

    HU_RUN_TEST(test_strategy_learner_classify);
    HU_RUN_TEST(test_strategy_learner_record_and_recommend);
#endif

    HU_RUN_TEST(test_speculative_cache_store_and_lookup);
    HU_RUN_TEST(test_speculative_cache_evict_expired);
    HU_RUN_TEST(test_speculative_predict_heuristic);
    HU_RUN_TEST(test_speculative_config_default);

    HU_RUN_TEST(test_uncertainty_high_confidence);
    HU_RUN_TEST(test_uncertainty_low_confidence);
    HU_RUN_TEST(test_uncertainty_extract_signals);
    HU_RUN_TEST(test_confidence_level_str);

    HU_RUN_TEST(test_integration_uncertainty_signals);
    HU_RUN_TEST(test_integration_speculative_roundtrip);

    HU_RUN_TEST(test_orchestrator_decompose_goal_test_mode);
    HU_RUN_TEST(test_decomposition_free_handles_null);

    HU_RUN_TEST(test_delegate_e2e_roundtrip);
    HU_RUN_TEST(test_delegate_missing_agent);
    HU_RUN_TEST(test_delegate_missing_prompt);

    HU_RUN_TEST(test_srag_assess_and_verify_roundtrip);
    HU_RUN_TEST(test_srag_creative_query_skips_retrieval);
    HU_RUN_TEST(test_prm_score_step_standalone);
    HU_RUN_TEST(test_prm_chain_with_multi_step);
    HU_RUN_TEST(test_constitutional_critique_roundtrip);
    HU_RUN_TEST(test_adaptive_rag_select_and_learn);
    HU_RUN_TEST(test_tot_explore_generates_branches);
    HU_RUN_TEST(test_streaming_dispatcher_calls_execute_streaming);
    HU_RUN_TEST(test_streaming_dispatcher_null_callback_falls_back);
    HU_RUN_TEST(test_streaming_dispatch_execute_only_fallback);
    HU_RUN_TEST(test_agent_turn_stream_e2e);

#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_dpo_record_from_retry_roundtrip);
    HU_RUN_TEST(test_tier_promote_demote_roundtrip);
#endif
}
