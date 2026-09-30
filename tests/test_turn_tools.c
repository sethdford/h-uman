/* tests/test_turn_tools.c — contract tests for hu_turn_tools
 * (src/agent/turn/turn_tools.c, S16 of the hu_agent_turn carve): the stage
 * executes the tool calls of the newest assistant message in agent->history,
 * appends their results, and advances the turn's tool-result count. */
#include "human/agent/turn.h"
#include "human/provider.h"
#include "test_framework.h"
#include "turn_test_fixture.h"
#include <string.h>

/* tests/ is not on src/agent's include path (same as test_agent_turn_transport.c). */
hu_error_t hu_agent_internal_append_history_with_tool_calls(hu_agent_t *agent, const char *content,
                                                            size_t content_len,
                                                            const hu_tool_call_t *tool_calls,
                                                            size_t tool_calls_count);

static bool tt_queue_one_call(tf_fixture_t *f) {
    hu_tool_call_t call = {.id = "call_1",
                           .id_len = 6,
                           .name = "memory_list",
                           .name_len = 11,
                           .arguments = "{\"q\":\"a\"}",
                           .arguments_len = 9};
    return hu_agent_internal_append_history_with_tool_calls(&f->agent, "", 0, &call, 1) == HU_OK;
}

static void turn_tools_rejects_a_null_context(void) {
    HU_ASSERT_EQ(hu_turn_tools(NULL), HU_ERR_INVALID_ARGUMENT);
}

static void turn_tools_runs_the_call_and_counts_the_result(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    HU_ASSERT_TRUE(tt_queue_one_call(&f));
    size_t before = f.agent.history_count;
    hu_turn_ctx_t *turn_ctx = hu_turn_ctx_new(&f.agent, "list my things", 14, &f.resp, &f.resp_len);
    HU_ASSERT_NOT_NULL(turn_ctx);
    turn_ctx->loop.turn_tool_results_count = 3; /* carried from earlier iterations */
    HU_ASSERT_EQ(hu_turn_tools(turn_ctx), HU_OK);
    HU_ASSERT_EQ(turn_ctx->loop.turn_tool_results_count, 4);
    HU_ASSERT_EQ(f.agent.history_count, before + 1);
    const hu_owned_message_t *m = &f.agent.history[f.agent.history_count - 1];
    HU_ASSERT_EQ(m->role, HU_ROLE_TOOL);
    HU_ASSERT_STR_CONTAINS(m->content, "listed 2 items: alpha, beta");
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}

static void turn_tools_locked_agent_blocks_without_counting(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_LOCKED));
    HU_ASSERT_TRUE(tt_queue_one_call(&f));
    size_t before = f.agent.history_count;
    hu_turn_ctx_t *turn_ctx = hu_turn_ctx_new(&f.agent, "list my things", 14, &f.resp, &f.resp_len);
    HU_ASSERT_NOT_NULL(turn_ctx);
    turn_ctx->loop.turn_tool_results_count = 3;
    HU_ASSERT_EQ(hu_turn_tools(turn_ctx), HU_OK);
    HU_ASSERT_EQ(turn_ctx->loop.turn_tool_results_count, 3);
    HU_ASSERT_EQ(f.agent.history_count, before + 1);
    /* Prefix, not equality: the moved block passes the 39-byte literal with a
     * hardcoded length of 38, so the stored text ends "...locked mod" (the
     * known off-by-one documented in test_agent_turn_characterization.c's
     * locked_autonomy case). A verbatim move may not fix it; a later fix
     * still satisfies this assertion. */
    HU_ASSERT_STR_CONTAINS(f.agent.history[f.agent.history_count - 1].content,
                           "Action blocked: agent is in locked mod");
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}

void run_turn_tools_tests(void) {
    HU_TEST_SUITE("TurnTools");
    HU_RUN_TEST(turn_tools_rejects_a_null_context);
    HU_RUN_TEST(turn_tools_runs_the_call_and_counts_the_result);
    HU_RUN_TEST(turn_tools_locked_agent_blocks_without_counting);
}
