/* tests/test_turn_context.c — contract tests for hu_turn_context
 * (src/agent/turn/turn_context.c, S4 of the hu_agent_turn carve). */
#include "human/agent/turn.h"
#include "test_framework.h"
#include <string.h>

static void turn_context_rejects_a_null_context(void) {
    HU_ASSERT_EQ(hu_turn_context(NULL), HU_ERR_INVALID_ARGUMENT);
}

#ifdef HU_ENABLE_SQLITE
#include "turn_test_fixture.h"

static const char k_plan[] = "[PLAN]: 2 steps planned, 1 completed";

static void turn_context_folds_the_plan_into_the_intelligence_context(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, true, HU_AUTONOMY_AUTONOMOUS));
    hu_turn_ctx_t *turn_ctx =
        hu_turn_ctx_new(&f.agent, "what should I do next", 21, &f.resp, &f.resp_len);
    HU_ASSERT_NOT_NULL(turn_ctx);
    turn_ctx->context.plan_ctx = k_plan;
    turn_ctx->context.plan_ctx_len = sizeof(k_plan) - 1;
    HU_ASSERT_EQ(hu_turn_context(turn_ctx), HU_OK);
    HU_ASSERT_NOT_NULL(turn_ctx->context.intelligence_ctx);
    HU_ASSERT_STR_CONTAINS(turn_ctx->context.intelligence_ctx,
                           "### [PLAN]: 2 steps planned, 1 completed");
    HU_ASSERT_EQ(turn_ctx->context.intelligence_ctx_len,
                 strlen(turn_ctx->context.intelligence_ctx));
    hu_turn_ctx_free(turn_ctx); /* owns all nine outputs: ASan proves each is released once */
    tf_close(&f);
}

static void turn_context_without_a_plan_leaves_it_out(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, true, HU_AUTONOMY_AUTONOMOUS));
    hu_turn_ctx_t *turn_ctx =
        hu_turn_ctx_new(&f.agent, "what should I do next", 21, &f.resp, &f.resp_len);
    HU_ASSERT_NOT_NULL(turn_ctx);
    HU_ASSERT_EQ(hu_turn_context(turn_ctx), HU_OK);
    HU_ASSERT_TRUE(!turn_ctx->context.intelligence_ctx ||
                   strstr(turn_ctx->context.intelligence_ctx, "[PLAN]") == NULL);
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}
#endif /* HU_ENABLE_SQLITE */

void run_turn_context_tests(void) {
    HU_TEST_SUITE("TurnContext");
    HU_RUN_TEST(turn_context_rejects_a_null_context);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(turn_context_folds_the_plan_into_the_intelligence_context);
    HU_RUN_TEST(turn_context_without_a_plan_leaves_it_out);
#endif
}
