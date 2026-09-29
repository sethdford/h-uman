#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/intelligence/world_model.h"
#include "test_framework.h"
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
    HU_ASSERT_EQ(
        hu_world_record_outcome(&model, "restart server", 14, "server recovered", 16, 0.9, now),
        HU_OK);

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
void run_world_simulation_tests(void) {
    HU_TEST_SUITE("world_sim");
    HU_RUN_TEST(simulation_predicts_with_observations);
    HU_RUN_TEST(simulation_handles_no_data);
}

#else

void run_world_simulation_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
