#include "human/agent/autonomy.h"
#include "test_framework.h"
#include <string.h>
#ifdef HU_ENABLE_SQLITE
#include "human/agent/goals.h"
#include "human/core/allocator.h"
#include <sqlite3.h>
#endif

static void test_autonomy_init_defaults(void) {
    hu_autonomy_state_t state;
    hu_error_t err = hu_autonomy_init(&state, 8192);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(state.context_budget, 8192u);
    HU_ASSERT_EQ(state.goal_count, 0u);
}

static void test_autonomy_add_goal(void) {
    hu_autonomy_state_t state;
    hu_autonomy_init(&state, 8192);
    hu_error_t err = hu_autonomy_add_goal(&state, "Complete report", 15, 0.8);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(state.goal_count, 1u);
    HU_ASSERT_STR_EQ(state.goals[0].description, "Complete report");
    HU_ASSERT_EQ(state.goals[0].priority, 0.8);
    HU_ASSERT_FALSE(state.goals[0].completed);
}

static void test_autonomy_get_next_goal(void) {
    hu_autonomy_state_t state;
    hu_autonomy_init(&state, 8192);
    hu_autonomy_add_goal(&state, "Low priority", 12, 0.2);
    hu_autonomy_add_goal(&state, "High priority", 13, 0.9);

    hu_autonomy_goal_t out;
    hu_error_t err = hu_autonomy_get_next_goal(&state, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(out.description, "High priority");
    HU_ASSERT_EQ(out.priority, 0.9);
}

static void test_autonomy_mark_complete(void) {
    hu_autonomy_state_t state;
    hu_autonomy_init(&state, 8192);
    hu_autonomy_add_goal(&state, "Task", 4, 0.5);
    hu_error_t err = hu_autonomy_mark_complete(&state, 0);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(state.goals[0].completed);
}

static void test_autonomy_needs_consolidation_over_budget(void) {
    hu_autonomy_state_t state;
    hu_autonomy_init(&state, 100);
    state.context_tokens_used = 85; /* > 80% of 100 */
    bool need = hu_autonomy_needs_consolidation(&state, state.session_start + 1000);
    HU_ASSERT_TRUE(need);
}

static void test_autonomy_consolidate_resets(void) {
    hu_autonomy_state_t state;
    hu_autonomy_init(&state, 8192);
    hu_autonomy_add_goal(&state, "Done task", 9, 0.5);
    hu_autonomy_mark_complete(&state, 0);
    state.context_tokens_used = 1000;

    hu_error_t err = hu_autonomy_consolidate(&state);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(state.context_tokens_used, 0u);
    HU_ASSERT_EQ(state.goal_count, 0u);
}

static void test_autonomy_null_args_returns_error(void) {
    HU_ASSERT_EQ(hu_autonomy_init(NULL, 8192), HU_ERR_INVALID_ARGUMENT);
    hu_autonomy_state_t state;
    hu_autonomy_init(&state, 8192);
    HU_ASSERT_EQ(hu_autonomy_add_goal(NULL, "x", 1, 0.5), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_autonomy_add_goal(&state, NULL, 0, 0.5), HU_ERR_INVALID_ARGUMENT);
    hu_autonomy_goal_t out;
    HU_ASSERT_EQ(hu_autonomy_get_next_goal(NULL, &out), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_autonomy_get_next_goal(&state, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_autonomy_mark_complete(NULL, 0), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_autonomy_consolidate(NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_autonomy_intrinsic_goal_on_failures(void) {
    hu_autonomy_state_t state;
    hu_autonomy_init(&state, 8192);
    hu_error_t err = hu_autonomy_generate_intrinsic_goal(&state, 1, 5);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(state.goal_count, 1u);
    HU_ASSERT_TRUE(strstr(state.goals[0].description, "failure") != NULL);
    HU_ASSERT_TRUE(state.goals[0].priority >= 0.8);
}

static void test_autonomy_intrinsic_goal_proactive(void) {
    hu_autonomy_state_t state;
    hu_autonomy_init(&state, 8192);
    hu_error_t err = hu_autonomy_generate_intrinsic_goal(&state, 0, 0);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(state.goal_count, 1u);
    HU_ASSERT_TRUE(strstr(state.goals[0].description, "check") != NULL ||
                   strstr(state.goals[0].description, "Proactively") != NULL);
}

static void test_autonomy_intrinsic_goal_generated_daily_target(void) {
    hu_autonomy_state_t state;
    hu_autonomy_init(&state, 8192);
    hu_error_t err = hu_autonomy_generate_intrinsic_goal(&state, 0, 0);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(state.goal_count > 0u);

    hu_autonomy_state_t state2;
    hu_autonomy_init(&state2, 8192);
    err = hu_autonomy_generate_intrinsic_goal(&state2, 2, 5);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(state2.goal_count > 0u);
    HU_ASSERT_TRUE(state2.goals[0].priority >= 0.8);
}

static void test_autonomy_externalize_restore(void) {
    hu_autonomy_state_t state;
    hu_autonomy_init(&state, 4096);
    hu_autonomy_add_goal(&state, "Build report", 12, 0.7);
    hu_autonomy_add_goal(&state, "Review code", 11, 0.5);
    state.context_tokens_used = 2000;

    char buf[2048];
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_autonomy_externalize_state(&state, buf, sizeof(buf), &out_len), HU_OK);
    HU_ASSERT_TRUE(out_len > 0);

    hu_autonomy_state_t restored;
    hu_autonomy_init(&restored, 0);
    HU_ASSERT_EQ(hu_autonomy_restore_state(&restored, buf, out_len), HU_OK);
    HU_ASSERT_EQ(restored.goal_count, 2u);
    HU_ASSERT_EQ(restored.context_budget, 4096u);
    HU_ASSERT_EQ(restored.context_tokens_used, 2000u);
    HU_ASSERT_TRUE(restored.goals[0].priority >= 0.6);
}

static void test_autonomy_externalize_null(void) {
    hu_autonomy_state_t state;
    hu_autonomy_init(&state, 8192);
    char buf[64];
    size_t len = 0;
    HU_ASSERT_EQ(hu_autonomy_externalize_state(NULL, buf, 64, &len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_autonomy_restore_state(NULL, buf, 1), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_autonomy_generate_intrinsic_goal(NULL, 0, 0), HU_ERR_INVALID_ARGUMENT);
}

#ifdef HU_ENABLE_SQLITE
/* #4 self-initiated agenda: hu_autonomy_seed_intrinsic_goal persists ONE goal
 * into the (prompt-read) goal engine when the contact's agenda is empty, honors
 * off/shadow/on, and never piles up. Pre/post pinned non-vacuously. */
static void test_autonomy_seed_intrinsic_goal_persists_when_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_goal_engine_t ge;
    HU_ASSERT_EQ(hu_goal_engine_create(&alloc, db, &ge), HU_OK);
    HU_ASSERT_EQ(hu_goal_init_tables(&ge), HU_OK);

    const char *c = "contact_a";
    size_t cl = 9;
    size_t n = 99;
    HU_ASSERT_EQ(hu_goal_count(&ge, c, cl, &n), HU_OK);
    HU_ASSERT_EQ((int)n, 0); /* precondition: empty agenda */

    /* off → no-op, nothing persisted */
    int64_t id = 7;
    HU_ASSERT_EQ(hu_autonomy_seed_intrinsic_goal(&ge, c, cl, 0, 1000, &id), HU_OK);
    HU_ASSERT_EQ((int)id, 0);
    hu_goal_count(&ge, c, cl, &n);
    HU_ASSERT_EQ((int)n, 0);

    /* shadow → computes + logs, persists nothing */
    HU_ASSERT_EQ(hu_autonomy_seed_intrinsic_goal(&ge, c, cl, 1, 1000, &id), HU_OK);
    HU_ASSERT_EQ((int)id, 0);
    hu_goal_count(&ge, c, cl, &n);
    HU_ASSERT_EQ((int)n, 0);

    /* on → persists exactly one self-initiated goal */
    HU_ASSERT_EQ(hu_autonomy_seed_intrinsic_goal(&ge, c, cl, 2, 1000, &id), HU_OK);
    HU_ASSERT(id > 0);
    hu_goal_count(&ge, c, cl, &n);
    HU_ASSERT_EQ((int)n, 1);

    /* idempotent: agenda now non-empty → no pile-up */
    int64_t id2 = 5;
    HU_ASSERT_EQ(hu_autonomy_seed_intrinsic_goal(&ge, c, cl, 2, 1000, &id2), HU_OK);
    HU_ASSERT_EQ((int)id2, 0);
    hu_goal_count(&ge, c, cl, &n);
    HU_ASSERT_EQ((int)n, 1);

    hu_goal_engine_deinit(&ge);
    sqlite3_close(db);
}
#endif

void run_autonomy_tests(void) {
    HU_TEST_SUITE("Autonomy");
    HU_RUN_TEST(test_autonomy_init_defaults);
    HU_RUN_TEST(test_autonomy_add_goal);
    HU_RUN_TEST(test_autonomy_get_next_goal);
    HU_RUN_TEST(test_autonomy_mark_complete);
    HU_RUN_TEST(test_autonomy_needs_consolidation_over_budget);
    HU_RUN_TEST(test_autonomy_consolidate_resets);
    HU_RUN_TEST(test_autonomy_null_args_returns_error);
    HU_RUN_TEST(test_autonomy_intrinsic_goal_on_failures);
    HU_RUN_TEST(test_autonomy_intrinsic_goal_proactive);
    HU_RUN_TEST(test_autonomy_intrinsic_goal_generated_daily_target);
    HU_RUN_TEST(test_autonomy_externalize_restore);
    HU_RUN_TEST(test_autonomy_externalize_null);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_autonomy_seed_intrinsic_goal_persists_when_empty);
#endif
}
