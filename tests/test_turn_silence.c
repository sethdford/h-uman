/* tests/test_turn_silence.c — contract tests for hu_turn_silence
 * (src/agent/turn/turn_silence.c, S8 of the hu_agent_turn carve). */
#include "human/agent/turn.h"
#include "test_framework.h"
#include "turn_test_fixture.h"
#include <string.h>

static void turn_silence_rejects_a_null_context(void) {
    hu_turn_step_t st = hu_turn_silence(NULL);
    HU_ASSERT_EQ(st.kind, HU_TURN_STEP_RETURN);
    HU_ASSERT_EQ(st.err, HU_ERR_INVALID_ARGUMENT);
}

static void turn_silence_question_takes_the_full_response_path(void) {
    const char *msg = "can you tell me more about that?";
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    char *resp = NULL;
    size_t resp_len = 0;
    hu_turn_ctx_t *turn_ctx = hu_turn_ctx_new(&f.agent, msg, strlen(msg), &resp, &resp_len);
    HU_ASSERT_NOT_NULL(turn_ctx);
    hu_turn_step_t st = hu_turn_silence(turn_ctx);
    HU_ASSERT_EQ(st.kind, HU_TURN_STEP_CONTINUE);
    HU_ASSERT_NULL(resp);
    HU_ASSERT_EQ(resp_len, 0);
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}

/* Same input as the characterization case silence_presence, whose golden
 * (recorded from unmodified code) pins the reply as "I'm here.", 9 bytes:
 * hu_silence_build_acknowledgment's PRESENCE text (src/humanness.c). */
static void turn_silence_grief_answers_with_presence(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    hu_turn_ctx_t *turn_ctx = hu_turn_ctx_new(&f.agent, "my dad died", 11, &f.resp, &f.resp_len);
    HU_ASSERT_NOT_NULL(turn_ctx);
    hu_turn_step_t st = hu_turn_silence(turn_ctx);
    HU_ASSERT_EQ(st.kind, HU_TURN_STEP_RETURN);
    HU_ASSERT_EQ(st.err, HU_OK);
    HU_ASSERT_STR_EQ(f.resp, "I'm here.");
    HU_ASSERT_EQ(f.resp_len, 9);
    HU_ASSERT_EQ(f.trp.calls, 0);
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}

void run_turn_silence_tests(void) {
    HU_TEST_SUITE("TurnSilence");
    HU_RUN_TEST(turn_silence_rejects_a_null_context);
    HU_RUN_TEST(turn_silence_question_takes_the_full_response_path);
    HU_RUN_TEST(turn_silence_grief_answers_with_presence);
}
