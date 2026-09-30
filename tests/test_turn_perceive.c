/* tests/test_turn_perceive.c — contract tests for hu_turn_perceive
 * (src/agent/turn/turn_perceive.c, S2 of the hu_agent_turn carve). */
#include "human/agent/turn.h"
#include "human/memory/stm.h"
#include "test_framework.h"
#include "turn_test_fixture.h"
#include <string.h>

static void turn_perceive_rejects_a_null_context(void) {
    HU_ASSERT_EQ(hu_turn_perceive(NULL), HU_ERR_INVALID_ARGUMENT);
}

static void turn_perceive_short_message_asks_for_a_brief_reply(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    size_t stm_before = hu_stm_count(&f.agent.stm);
    hu_turn_ctx_t *turn_ctx = hu_turn_ctx_new(&f.agent, "ok cool", 7, &f.resp, &f.resp_len);
    HU_ASSERT_NOT_NULL(turn_ctx);
    HU_ASSERT_EQ(hu_turn_perceive(turn_ctx), HU_OK);
    HU_ASSERT_NOT_NULL(turn_ctx->perception.tone_hint);
    HU_ASSERT_STR_CONTAINS(turn_ctx->perception.tone_hint, "very short message");
    HU_ASSERT_EQ(turn_ctx->perception.tone_hint_len, strlen(turn_ctx->perception.tone_hint));
    HU_ASSERT_EQ(hu_stm_count(&f.agent.stm), stm_before + 1); /* the user turn was recorded */
    HU_ASSERT_GT(turn_ctx->perception.cognition_budget.max_memory_entries, 0);
    HU_ASSERT_NULL(turn_ctx->perception.acp_context); /* no inter-agent inbox */
    HU_ASSERT_NULL(turn_ctx->perception.pref_ctx);    /* no memory */
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}

static void turn_perceive_long_message_asks_for_a_considered_reply(void) {
    char msg[480];
    for (size_t i = 0; i < sizeof(msg) - 1; i++)
        msg[i] = (i % 6 == 5) ? ' ' : 'a';
    msg[sizeof(msg) - 1] = '\0';
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    hu_turn_ctx_t *turn_ctx = hu_turn_ctx_new(&f.agent, msg, strlen(msg), &f.resp, &f.resp_len);
    HU_ASSERT_NOT_NULL(turn_ctx);
    HU_ASSERT_EQ(hu_turn_perceive(turn_ctx), HU_OK);
    HU_ASSERT_NOT_NULL(turn_ctx->perception.tone_hint);
    HU_ASSERT_STR_CONTAINS(turn_ctx->perception.tone_hint, "long, thoughtful message");
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}

static void turn_perceive_mid_length_message_sets_no_rhythm_hint(void) {
    const char *msg = "can you remind me what we decided about the trip next month";
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    hu_turn_ctx_t *turn_ctx = hu_turn_ctx_new(&f.agent, msg, strlen(msg), &f.resp, &f.resp_len);
    HU_ASSERT_NOT_NULL(turn_ctx);
    turn_ctx->perception.tone_hint = "sentinel"; /* the stage must overwrite it */
    turn_ctx->perception.tone_hint_len = 8;
    HU_ASSERT_EQ(hu_turn_perceive(turn_ctx), HU_OK);
    HU_ASSERT_NULL(turn_ctx->perception.tone_hint);
    HU_ASSERT_EQ(turn_ctx->perception.tone_hint_len, 0);
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}

void run_turn_perceive_tests(void) {
    HU_TEST_SUITE("TurnPerceive");
    HU_RUN_TEST(turn_perceive_rejects_a_null_context);
    HU_RUN_TEST(turn_perceive_short_message_asks_for_a_brief_reply);
    HU_RUN_TEST(turn_perceive_long_message_asks_for_a_considered_reply);
    HU_RUN_TEST(turn_perceive_mid_length_message_sets_no_rhythm_hint);
}
