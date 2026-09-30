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
hu_error_t hu_agent_internal_append_history(hu_agent_t *agent, hu_role_t role, const char *content,
                                            size_t content_len, const char *name, size_t name_len,
                                            const char *tool_call_id, size_t tool_call_id_len);

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

/* ── S16 security guard (turn_tools_causal_and_history_guard) ──────────────
 * The guard runs AFTER a tool executed successfully and can replace its
 * result with a failure: CausalArmor for HIGH-risk tools (a non-USER history
 * segment is the only source of a key term in the call's arguments), then the
 * interaction-history scorer for MEDIUM+ tools (escalating tool-call history).
 * It is called from both the dispatcher path and the sequential fallback, so
 * each path gets a block test and a minimal-difference control. */

static const char k_ca_blocked[] = "blocked: untrusted content dominates tool decision";
static const char k_hs_blocked[] = "blocked: suspicious tool-call history pattern";
static const char k_fw_output[] = "wrote 9 bytes";

/* A HIGH-risk tool: hu_tool_risk_level() classifies by name only, and
 * "file_write" is HIGH (src/security/policy.c). The fixture's single tool
 * slot is re-pointed at this vtable after tf_open. */
static hu_error_t tt_fw_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                hu_tool_result_t *out) {
    (void)ctx;
    (void)alloc;
    (void)args;
    *out = hu_tool_result_ok(k_fw_output, sizeof(k_fw_output) - 1);
    return HU_OK;
}
static const char *tt_fw_name(void *ctx) {
    (void)ctx;
    return "file_write";
}
static const char *tt_fw_desc(void *ctx) {
    (void)ctx;
    return "Write a file";
}
static const char *tt_fw_params(void *ctx) {
    (void)ctx;
    return "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}}}";
}
static const hu_tool_vtable_t k_fw_vtable = {
    .execute = tt_fw_execute,
    .name = tt_fw_name,
    .description = tt_fw_desc,
    .parameters_json = tt_fw_params,
};

static void tt_use_file_write_tool(tf_fixture_t *f) {
    f->agent.tools[0].vtable = &k_fw_vtable;
}

/* History for the CausalArmor cases. The call's only key term (>3 chars) is
 * "plumquartz". carrier_role decides who said it: HU_ROLE_TOOL (untrusted —
 * it is the only source of the term, so it dominates and the call is not
 * grounded in the user's words) or HU_ROLE_USER (trusted — control). Nothing
 * else differs between the block case and its control. */
static bool tt_seed_causal_history(tf_fixture_t *f, hu_role_t carrier_role) {
    static const char ask[] = "please save the note";
    static const char carrier[] = "the note says plumquartz";
    if (hu_agent_internal_append_history(&f->agent, HU_ROLE_USER, ask, sizeof(ask) - 1, NULL, 0,
                                         NULL, 0) != HU_OK)
        return false;
    /* Unnamed, so the history scorer never sees it (it only reads named tool
     * entries) — this case isolates CausalArmor. */
    if (hu_agent_internal_append_history(&f->agent, carrier_role, carrier, sizeof(carrier) - 1,
                                         NULL, 0, NULL, 0) != HU_OK)
        return false;
    hu_tool_call_t call = {.id = "call_fw",
                           .id_len = 7,
                           .name = "file_write",
                           .name_len = 10,
                           .arguments = "{\"text\":\"plumquartz\"}",
                           .arguments_len = 21};
    return hu_agent_internal_append_history_with_tool_calls(&f->agent, "", 0, &call, 1) == HU_OK;
}

/* Sequential-fallback seam: hu_turn_tools falls back to its sequential loop
 * when hu_dispatcher_dispatch fails. Under HU_IS_TEST the dispatcher's only
 * failure is OOM on its results array (calls_count * sizeof(hu_tool_result_t),
 * src/agent/dispatcher.c dispatch_sequential_ex), so a wrapper allocator
 * fails the first allocation of exactly that size once armed. The fallback is
 * observable independently: the dispatcher path advances
 * turn_tool_results_count only when dispatch returned HU_OK, so an unchanged
 * count proves the fallback ran (and would catch the seam hitting the wrong
 * allocation). */
static hu_allocator_t tt_real_alloc;
static bool tt_fail_armed;
static size_t tt_fail_size;
static int tt_fail_hits;

static void *tt_failing_alloc(void *ctx, size_t size) {
    if (tt_fail_armed && size == tt_fail_size) {
        tt_fail_armed = false;
        tt_fail_hits++;
        return NULL;
    }
    return tt_real_alloc.alloc(ctx, size);
}

static void tt_arm_dispatcher_oom(tf_fixture_t *f, size_t calls_count) {
    tt_real_alloc = f->alloc;
    tt_fail_armed = true;
    tt_fail_size = calls_count * sizeof(hu_tool_result_t);
    tt_fail_hits = 0;
    f->alloc.alloc = tt_failing_alloc; /* the agent holds &f->alloc */
}

static void tt_disarm_dispatcher_oom(tf_fixture_t *f) {
    tt_fail_armed = false;
    f->alloc.alloc = tt_real_alloc.alloc;
}

/* Runs hu_turn_tools on the fixture (count carried in as 3) and returns the
 * content of the HU_ROLE_TOOL entry it appended, or NULL. fallback selects
 * the sequential path via the dispatcher-OOM seam and asserts it was taken. */
static const char *tt_run_one(tf_fixture_t *f, bool fallback, size_t *count_out) {
    size_t before = f->agent.history_count;
    hu_turn_ctx_t *turn_ctx = hu_turn_ctx_new(&f->agent, "save it", 7, &f->resp, &f->resp_len);
    if (!turn_ctx)
        return NULL;
    turn_ctx->loop.turn_tool_results_count = 3;
    if (fallback)
        tt_arm_dispatcher_oom(f, 1);
    hu_error_t err = hu_turn_tools(turn_ctx);
    if (fallback) {
        tt_disarm_dispatcher_oom(f);
        HU_ASSERT_EQ(tt_fail_hits, 1);
    }
    *count_out = turn_ctx->loop.turn_tool_results_count;
    hu_turn_ctx_free(turn_ctx);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(f->agent.history_count, before + 1);
    if (f->agent.history_count != before + 1)
        return NULL;
    const hu_owned_message_t *m = &f->agent.history[f->agent.history_count - 1];
    HU_ASSERT_EQ(m->role, HU_ROLE_TOOL);
    return m->content;
}

static void turn_tools_dispatcher_causal_armor_blocks_untrusted_dominance(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    tt_use_file_write_tool(&f);
    HU_ASSERT_TRUE(tt_seed_causal_history(&f, HU_ROLE_TOOL));
    size_t count = 0;
    const char *content = tt_run_one(&f, false, &count);
    HU_ASSERT_STR_EQ(content, k_ca_blocked);
    HU_ASSERT_EQ(count, 4); /* dispatcher path: the call still counts */
    tf_close(&f);
}

static void turn_tools_dispatcher_causal_armor_keeps_user_grounded_call(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    tt_use_file_write_tool(&f);
    HU_ASSERT_TRUE(tt_seed_causal_history(&f, HU_ROLE_USER));
    size_t count = 0;
    const char *content = tt_run_one(&f, false, &count);
    HU_ASSERT_STR_EQ(content, k_fw_output);
    HU_ASSERT_EQ(count, 4);
    tf_close(&f);
}

static void turn_tools_fallback_causal_armor_blocks_untrusted_dominance(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    tt_use_file_write_tool(&f);
    HU_ASSERT_TRUE(tt_seed_causal_history(&f, HU_ROLE_TOOL));
    size_t count = 0;
    const char *content = tt_run_one(&f, true, &count);
    HU_ASSERT_STR_EQ(content, k_ca_blocked);
    HU_ASSERT_EQ(count, 3); /* fallback after a failed dispatch does not count */
    tf_close(&f);
}

static void turn_tools_fallback_causal_armor_keeps_user_grounded_call(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    tt_use_file_write_tool(&f);
    HU_ASSERT_TRUE(tt_seed_causal_history(&f, HU_ROLE_USER));
    size_t count = 0;
    const char *content = tt_run_one(&f, true, &count);
    HU_ASSERT_STR_EQ(content, k_fw_output);
    HU_ASSERT_EQ(count, 3);
    tf_close(&f);
}

/* History for the history-scorer cases: six successful named tool results,
 * then a call to the fixture's memory_list (MEDIUM: unknown names default to
 * MEDIUM, below CausalArmor's HIGH gate). escalating=true replays
 * memory_recall, file_read, http_request twice: two LOW->MEDIUM risk steps
 * (+0.2 each) and two read->read->send chains (+0.3 each) score 1.0 >= 0.6.
 * The control uses six memory_list results: flat risk, no chain, score 0. */
static bool tt_seed_scorer_history(tf_fixture_t *f, bool escalating) {
    static const char *const esc[] = {"memory_recall", "file_read", "http_request",
                                      "memory_recall", "file_read", "http_request"};
    static const char ok[] = "ok";
    for (size_t i = 0; i < 6; i++) {
        const char *name = escalating ? esc[i] : "memory_list";
        if (hu_agent_internal_append_history(&f->agent, HU_ROLE_TOOL, ok, sizeof(ok) - 1, name,
                                             strlen(name), "prior", 5) != HU_OK)
            return false;
    }
    return tt_queue_one_call(f);
}

static void turn_tools_dispatcher_history_scorer_blocks_escalating_chain(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    HU_ASSERT_TRUE(tt_seed_scorer_history(&f, true));
    size_t count = 0;
    const char *content = tt_run_one(&f, false, &count);
    HU_ASSERT_STR_EQ(content, k_hs_blocked);
    HU_ASSERT_EQ(count, 4);
    tf_close(&f);
}

static void turn_tools_dispatcher_history_scorer_keeps_flat_history(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    HU_ASSERT_TRUE(tt_seed_scorer_history(&f, false));
    size_t count = 0;
    const char *content = tt_run_one(&f, false, &count);
    HU_ASSERT_STR_EQ(content, "listed 2 items: alpha, beta");
    HU_ASSERT_EQ(count, 4);
    tf_close(&f);
}

void run_turn_tools_tests(void) {
    HU_TEST_SUITE("TurnTools");
    HU_RUN_TEST(turn_tools_rejects_a_null_context);
    HU_RUN_TEST(turn_tools_runs_the_call_and_counts_the_result);
    HU_RUN_TEST(turn_tools_locked_agent_blocks_without_counting);
    HU_RUN_TEST(turn_tools_dispatcher_causal_armor_blocks_untrusted_dominance);
    HU_RUN_TEST(turn_tools_dispatcher_causal_armor_keeps_user_grounded_call);
    HU_RUN_TEST(turn_tools_fallback_causal_armor_blocks_untrusted_dominance);
    HU_RUN_TEST(turn_tools_fallback_causal_armor_keeps_user_grounded_call);
    HU_RUN_TEST(turn_tools_dispatcher_history_scorer_blocks_escalating_chain);
    HU_RUN_TEST(turn_tools_dispatcher_history_scorer_keeps_flat_history);
}
