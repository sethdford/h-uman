/* tests/test_turn_plan.c — contract tests for hu_turn_active_plan
 * (src/agent/turn/turn_plan.c): resuming an [ACTIVE_PLAN] system note from
 * the last 10 history entries, owned by the caller. */
#include "human/agent/turn.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include "turn_test_fixture.h"
#include <string.h>

/* tests/ is not on src/agent's include path (same as test_turn_tail.c). */
hu_error_t hu_agent_internal_append_history(hu_agent_t *agent, hu_role_t role, const char *content,
                                            size_t content_len, const char *name, size_t name_len,
                                            const char *tool_call_id, size_t tool_call_id_len);

static bool tp_append(tf_fixture_t *f, hu_role_t role, const char *text) {
    return hu_agent_internal_append_history(&f->agent, role, text, strlen(text), NULL, 0, NULL,
                                            0) == HU_OK;
}

static void test_turn_plan_returns_newest_active_plan_copy(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    HU_ASSERT_TRUE(tp_append(&f, HU_ROLE_SYSTEM, "[ACTIVE_PLAN] old: step 1"));
    HU_ASSERT_TRUE(tp_append(&f, HU_ROLE_USER, "keep going"));
    HU_ASSERT_TRUE(tp_append(&f, HU_ROLE_SYSTEM, "[ACTIVE_PLAN] new: step 2"));
    size_t len = 99;
    char *plan = hu_turn_active_plan(&f.agent, &len);
    HU_ASSERT_NOT_NULL(plan);
    HU_ASSERT_STR_EQ(plan, "[ACTIVE_PLAN] new: step 2");
    HU_ASSERT_EQ(len, strlen("[ACTIVE_PLAN] new: step 2"));
    /* The caller owns a copy, not a pointer into history. */
    HU_ASSERT_TRUE(plan != f.agent.history[f.agent.history_count - 1].content);
    f.agent.alloc->free(f.agent.alloc->ctx, plan, len + 1);
    tf_close(&f);
}

static void test_turn_plan_ignores_non_system_and_out_of_window(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    HU_ASSERT_TRUE(tp_append(&f, HU_ROLE_SYSTEM, "[ACTIVE_PLAN] too old"));
    for (int i = 0; i < 10; i++) /* pushes the plan out of the 10-entry window */
        HU_ASSERT_TRUE(tp_append(&f, HU_ROLE_USER, "[ACTIVE_PLAN] user text is not a plan"));
    size_t len = 99;
    HU_ASSERT_NULL(hu_turn_active_plan(&f.agent, &len));
    HU_ASSERT_EQ(len, (size_t)0);
    tf_close(&f);
}

static void test_turn_plan_null_args(void) {
    size_t len = 99;
    HU_ASSERT_NULL(hu_turn_active_plan(NULL, &len));
    HU_ASSERT_EQ(len, (size_t)0);
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    HU_ASSERT_NULL(hu_turn_active_plan(&f.agent, NULL));
    tf_close(&f);
}

void run_turn_plan_tests(void) {
    HU_TEST_SUITE("TurnPlan");
    HU_RUN_TEST(test_turn_plan_returns_newest_active_plan_copy);
    HU_RUN_TEST(test_turn_plan_ignores_non_system_and_out_of_window);
    HU_RUN_TEST(test_turn_plan_null_args);
}
