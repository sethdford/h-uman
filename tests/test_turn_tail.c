/* tests/test_turn_tail.c — contract tests for hu_turn_tail and hu_turn_exhausted
 * (src/agent/turn/turn_tail.c, S17–S18 of the hu_agent_turn carve): the end of
 * each tool iteration (replan on tool failure, mid-turn and goal-relevant
 * retrieval, scratchpad, checkpoint) and the observer events of the
 * tool-iterations-exhausted exit. */
#include "human/agent/checkpoint.h"
#include "human/agent/scratchpad.h"
#include "human/agent/turn.h"
#include "human/core/allocator.h"
#include "human/observer.h"
#include "test_framework.h"
#include "turn_test_fixture.h"
#include <string.h>

/* tests/ is not on src/agent's include path (same as test_turn_tools.c). */
hu_error_t hu_agent_internal_append_history(hu_agent_t *agent, hu_role_t role, const char *content,
                                            size_t content_len, const char *name, size_t name_len,
                                            const char *tool_call_id, size_t tool_call_id_len);

static const char k_plan[] = "[PLAN]: 2 steps planned, 1 completed";

static bool tl_append(tf_fixture_t *f, hu_role_t role, const char *text) {
    if (role == HU_ROLE_TOOL)
        return hu_agent_internal_append_history(&f->agent, role, text, strlen(text), "memory_list",
                                                11, "call_1", 6) == HU_OK;
    return hu_agent_internal_append_history(&f->agent, role, text, strlen(text), NULL, 0, NULL,
                                            0) == HU_OK;
}

/* Earlier conversation: three user/assistant exchanges, so the replan scan's
 * 8-entry window is full once this turn's tool results land after it. */
static bool tl_pad(tf_fixture_t *f) {
    for (int i = 0; i < 3; i++) {
        if (!tl_append(f, HU_ROLE_USER, "list my things") ||
            !tl_append(f, HU_ROLE_ASSISTANT, "sure, looking"))
            return false;
    }
    return true;
}

/* hu_turn_ctx_new marks the turn's start in history, so a test creates the
 * context first and appends this turn's tool results after it, as
 * hu_agent_turn does. */
static hu_turn_ctx_t *tl_ctx(tf_fixture_t *f, const char *msg, uint32_t iter) {
    hu_turn_ctx_t *turn_ctx = hu_turn_ctx_new(&f->agent, msg, strlen(msg), &f->resp, &f->resp_len);
    if (turn_ctx)
        turn_ctx->loop.iter = iter;
    return turn_ctx;
}

static void tl_set_plan(hu_turn_ctx_t *turn_ctx) {
    turn_ctx->context.plan_ctx = k_plan;
    turn_ctx->context.plan_ctx_len = sizeof(k_plan) - 1;
}

static size_t tl_count_replans(const tf_fixture_t *f) {
    size_t n = 0;
    for (size_t i = 0; i < f->agent.history_count; i++) {
        const hu_owned_message_t *m = &f->agent.history[i];
        if (m->role == HU_ROLE_SYSTEM && m->content && strncmp(m->content, "[REPLAN", 7) == 0)
            n++;
    }
    return n;
}

static void turn_tail_rejects_a_null_context(void) {
    HU_ASSERT_EQ(hu_turn_tail(NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_turn_exhausted(NULL), HU_ERR_INVALID_ARGUMENT);
}

/* Two failed tool results + a plan: the HU_IS_TEST planner stub returns one
 * step, so the stage appends exactly this system note. */
static void turn_tail_replans_after_two_tool_failures(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    HU_ASSERT_TRUE(tl_pad(&f));
    hu_turn_ctx_t *turn_ctx = tl_ctx(&f, "list my things", 1);
    HU_ASSERT_NOT_NULL(turn_ctx);
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "Error: disk full"));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "denied: not allowed"));
    size_t before = f.agent.history_count;
    tl_set_plan(turn_ctx);
    HU_ASSERT_EQ(hu_turn_tail(turn_ctx), HU_OK);
    HU_ASSERT_EQ(f.agent.history_count, before + 1);
    const hu_owned_message_t *m = &f.agent.history[f.agent.history_count - 1];
    HU_ASSERT_EQ(m->role, HU_ROLE_SYSTEM);
    HU_ASSERT_STR_EQ(m->content, "[REPLAN after 2 tool failures]: 1 new steps");
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}

/* A conversation shorter than the 8-entry window still replans: the floor of
 * the backward scan once wrapped (history_count - 8 on a size_t), so with
 * fewer than 8 entries the scan never ran and the failures went unseen. */
static void turn_tail_replans_in_a_history_shorter_than_the_window(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    hu_turn_ctx_t *turn_ctx = tl_ctx(&f, "list my things", 1);
    HU_ASSERT_NOT_NULL(turn_ctx);
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_USER, "list my things"));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "Error: disk full"));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "denied: not allowed"));
    HU_ASSERT_EQ(f.agent.history_count, 3);
    tl_set_plan(turn_ctx);
    HU_ASSERT_EQ(hu_turn_tail(turn_ctx), HU_OK);
    HU_ASSERT_EQ(f.agent.history_count, 4);
    const hu_owned_message_t *m = &f.agent.history[3];
    HU_ASSERT_EQ(m->role, HU_ROLE_SYSTEM);
    HU_ASSERT_STR_EQ(m->content, "[REPLAN after 2 tool failures]: 1 new steps");
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}

/* Control for the test above: same failures, no plan in progress. */
static void turn_tail_without_a_plan_does_not_replan(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    HU_ASSERT_TRUE(tl_pad(&f));
    hu_turn_ctx_t *turn_ctx = tl_ctx(&f, "list my things", 1);
    HU_ASSERT_NOT_NULL(turn_ctx);
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "Error: disk full"));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "denied: not allowed"));
    size_t before = f.agent.history_count;
    HU_ASSERT_EQ(hu_turn_tail(turn_ctx), HU_OK);
    HU_ASSERT_EQ(f.agent.history_count, before);
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}

/* Control: a plan but only one failure is below the replan threshold. */
static void turn_tail_one_failure_does_not_replan(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    HU_ASSERT_TRUE(tl_pad(&f));
    hu_turn_ctx_t *turn_ctx = tl_ctx(&f, "list my things", 1);
    HU_ASSERT_NOT_NULL(turn_ctx);
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "listed 2 items: alpha, beta"));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "Error: x"));
    size_t before = f.agent.history_count;
    tl_set_plan(turn_ctx);
    HU_ASSERT_EQ(hu_turn_tail(turn_ctx), HU_OK);
    HU_ASSERT_EQ(f.agent.history_count, before);
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}

/* Failures from an EARLIER turn are history, not this turn's progress: they
 * sit inside the 8-entry window but must not trigger a replan (one extra LLM
 * call) when this turn's own tools all succeeded. */
static void turn_tail_ignores_tool_failures_from_a_previous_turn(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_USER, "list my things"));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "Error: disk full"));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "denied: not allowed"));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_ASSISTANT, "sorry, that failed"));
    hu_turn_ctx_t *turn_ctx = tl_ctx(&f, "try again", 1);
    HU_ASSERT_NOT_NULL(turn_ctx);
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_USER, "try again"));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "listed 2 items: alpha, beta"));
    HU_ASSERT_LE(f.agent.history_count, 8); /* the prior failures are inside the window */
    size_t before = f.agent.history_count;
    tl_set_plan(turn_ctx);
    HU_ASSERT_EQ(hu_turn_tail(turn_ctx), HU_OK);
    HU_ASSERT_EQ(f.agent.history_count, before);
    HU_ASSERT_EQ(tl_count_replans(&f), 0);
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}

/* One failure set, one replan: a later iteration of the same turn with no new
 * failures does not replan again for the failures already handled. */
static void turn_tail_replans_once_per_failure_set(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    hu_turn_ctx_t *turn_ctx = tl_ctx(&f, "list my things", 1);
    HU_ASSERT_NOT_NULL(turn_ctx);
    tl_set_plan(turn_ctx);
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_USER, "list my things"));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "Error: disk full"));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "denied: not allowed"));
    HU_ASSERT_EQ(hu_turn_tail(turn_ctx), HU_OK);
    HU_ASSERT_EQ(tl_count_replans(&f), 1);
    /* iteration 2: one more tool call, which succeeds */
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_ASSISTANT, ""));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "listed 2 items: alpha, beta"));
    turn_ctx->loop.iter = 2;
    size_t before = f.agent.history_count;
    HU_ASSERT_EQ(hu_turn_tail(turn_ctx), HU_OK);
    HU_ASSERT_EQ(f.agent.history_count, before);
    HU_ASSERT_EQ(tl_count_replans(&f), 1);
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}

/* Control for the test above: NEW failures after a replan do replan again,
 * and the note counts only the new ones. */
static void turn_tail_replans_again_for_new_failures_only(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    hu_turn_ctx_t *turn_ctx = tl_ctx(&f, "list my things", 1);
    HU_ASSERT_NOT_NULL(turn_ctx);
    tl_set_plan(turn_ctx);
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_USER, "list my things"));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "Error: disk full"));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "denied: not allowed"));
    HU_ASSERT_EQ(hu_turn_tail(turn_ctx), HU_OK);
    HU_ASSERT_EQ(tl_count_replans(&f), 1);
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "Error: timeout"));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "error: bad args"));
    turn_ctx->loop.iter = 2;
    HU_ASSERT_EQ(hu_turn_tail(turn_ctx), HU_OK);
    HU_ASSERT_EQ(tl_count_replans(&f), 2);
    const hu_owned_message_t *m = &f.agent.history[f.agent.history_count - 1];
    HU_ASSERT_STR_EQ(m->content, "[REPLAN after 2 tool failures]: 1 new steps");
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}

/* The scratchpad records this iteration's token and tool-result counts under
 * turn_<iter>; iteration 3 is not a checkpoint step (interval 5). */
static void turn_tail_records_the_iteration_in_the_scratchpad(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    HU_ASSERT_FALSE(hu_scratchpad_has(&f.agent.sota.scratchpad, "turn_3", 6));
    hu_turn_ctx_t *turn_ctx = tl_ctx(&f, "list my things", 3);
    HU_ASSERT_NOT_NULL(turn_ctx);
    turn_ctx->loop.turn_tokens = 42;
    turn_ctx->loop.turn_tool_results_count = 2;
    HU_ASSERT_EQ(hu_turn_tail(turn_ctx), HU_OK);
    const char *val = NULL;
    size_t val_len = 0;
    HU_ASSERT_EQ(hu_scratchpad_get(&f.agent.sota.scratchpad, "turn_3", 6, &val, &val_len), HU_OK);
    HU_ASSERT_EQ(val_len, strlen("tokens=42,tools=2"));
    HU_ASSERT_TRUE(memcmp(val, "tokens=42,tools=2", val_len) == 0);
    HU_ASSERT_EQ(f.agent.sota.checkpoint_store.count, 0);
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}

/* Every fifth iteration saves an "agent_turn" checkpoint with the iteration
 * and the agent's running token total. */
static void turn_tail_checkpoints_every_fifth_iteration(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    HU_ASSERT_EQ(f.agent.sota.checkpoint_store.count, 0);
    f.agent.total_tokens = 7;
    hu_turn_ctx_t *turn_ctx = tl_ctx(&f, "list my things", 5);
    HU_ASSERT_NOT_NULL(turn_ctx);
    HU_ASSERT_EQ(hu_turn_tail(turn_ctx), HU_OK);
    hu_checkpoint_t cp;
    HU_ASSERT_EQ(hu_checkpoint_load(&f.agent.sota.checkpoint_store, "agent_turn", 10, &cp), HU_OK);
    HU_ASSERT_EQ(cp.step, 5);
    HU_ASSERT_STR_EQ(cp.state_json, "{\"iter\":5,\"tokens\":7}");
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}

/* The checkpoint store owns each slot's state_json: a later save to the same
 * task frees the previous copy, and hu_agent_deinit frees the last one. */
static void turn_tail_checkpoint_state_is_released_by_agent_deinit(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    HU_ASSERT_NOT_NULL(ta);
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open_alloc(&f, hu_tracking_allocator_allocator(ta), NULL, 0, false,
                                 HU_AUTONOMY_AUTONOMOUS));
    hu_turn_ctx_t *turn_ctx = tl_ctx(&f, "list my things", 5);
    HU_ASSERT_NOT_NULL(turn_ctx);
    HU_ASSERT_EQ(hu_turn_tail(turn_ctx), HU_OK);
    turn_ctx->loop.iter = 10;
    HU_ASSERT_EQ(hu_turn_tail(turn_ctx), HU_OK);
    hu_turn_ctx_free(turn_ctx);
    HU_ASSERT_EQ(f.agent.sota.checkpoint_store.count, 1);
    HU_ASSERT_NOT_NULL(f.agent.sota.checkpoint_store.checkpoints[0].state_json);
    HU_ASSERT_STR_EQ(f.agent.sota.checkpoint_store.checkpoints[0].state_json,
                     "{\"iter\":10,\"tokens\":0}");
    tf_close(&f);
    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

#ifdef HU_ENABLE_SQLITE
/* After a tool result, memory relevant to it is folded in as a system note;
 * from iteration 2 on, memory relevant to the user's goal is too. */
static void turn_tail_folds_memory_relevant_to_the_tool_result_and_the_goal(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, true, HU_AUTONOMY_AUTONOMOUS));
    HU_ASSERT_TRUE(tf_store(&f, "alpha_deadline", "the alpha project deadline is friday", NULL));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_USER, "when is the alpha project deadline"));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "alpha project deadline: not in calendar"));
    size_t before = f.agent.history_count;
    hu_turn_ctx_t *turn_ctx = tl_ctx(&f, "when is the alpha project deadline", 2);
    HU_ASSERT_NOT_NULL(turn_ctx);
    HU_ASSERT_EQ(hu_turn_tail(turn_ctx), HU_OK);
    HU_ASSERT_EQ(f.agent.history_count, before + 2);
    const hu_owned_message_t *mid = &f.agent.history[before];
    const hu_owned_message_t *goal = &f.agent.history[before + 1];
    HU_ASSERT_EQ(mid->role, HU_ROLE_SYSTEM);
    HU_ASSERT_TRUE(strncmp(mid->content, "[memory context from tool results]: ", 36) == 0);
    HU_ASSERT_STR_CONTAINS(mid->content, "friday");
    HU_ASSERT_EQ(goal->role, HU_ROLE_SYSTEM);
    HU_ASSERT_TRUE(strncmp(goal->content, "[goal-relevant memory]: ", 24) == 0);
    HU_ASSERT_STR_CONTAINS(goal->content, "friday");
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}

/* Control: on the first iteration only the tool-result note is added. */
static void turn_tail_skips_goal_memory_on_the_first_iteration(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, true, HU_AUTONOMY_AUTONOMOUS));
    HU_ASSERT_TRUE(tf_store(&f, "alpha_deadline", "the alpha project deadline is friday", NULL));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_USER, "when is the alpha project deadline"));
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_TOOL, "alpha project deadline: not in calendar"));
    size_t before = f.agent.history_count;
    hu_turn_ctx_t *turn_ctx = tl_ctx(&f, "when is the alpha project deadline", 1);
    HU_ASSERT_NOT_NULL(turn_ctx);
    HU_ASSERT_EQ(hu_turn_tail(turn_ctx), HU_OK);
    HU_ASSERT_EQ(f.agent.history_count, before + 1);
    HU_ASSERT_STR_CONTAINS(f.agent.history[before].content, "[memory context from tool results]");
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}
#endif /* HU_ENABLE_SQLITE */

typedef struct tl_obs {
    size_t count;
    hu_observer_event_tag_t tags[4];
    uint32_t iterations;
    const char *component;
    const char *message;
} tl_obs_t;

static void tl_obs_record(void *ctx, const hu_observer_event_t *ev) {
    tl_obs_t *o = (tl_obs_t *)ctx;
    if (o->count < 4)
        o->tags[o->count] = ev->tag;
    o->count++;
    if (ev->tag == HU_OBSERVER_EVENT_TOOL_ITERATIONS_EXHAUSTED)
        o->iterations = ev->data.tool_iterations_exhausted.iterations;
    if (ev->tag == HU_OBSERVER_EVENT_ERR) {
        o->component = ev->data.err.component;
        o->message = ev->data.err.message;
    }
}

/* The exhausted exit reports the iteration cap, then an agent error, in that
 * order; the turn-body frees and the HU_ERR_TIMEOUT return stay in
 * agent_turn_run (pinned end to end by the iteration_exhaustion golden). */
static void turn_exhausted_reports_the_cap_then_an_error(void) {
    static const hu_observer_vtable_t vt = {.record_event = tl_obs_record};
    tl_obs_t rec;
    memset(&rec, 0, sizeof(rec));
    hu_observer_t obs = {.ctx = &rec, .vtable = &vt};
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    f.agent.observer = &obs;
    hu_turn_ctx_t *turn_ctx = tl_ctx(&f, "keep listing", 4);
    HU_ASSERT_NOT_NULL(turn_ctx);
    HU_ASSERT_EQ(hu_turn_exhausted(turn_ctx), HU_OK);
    f.agent.observer = NULL;
    HU_ASSERT_EQ(rec.count, 2);
    HU_ASSERT_EQ(rec.tags[0], HU_OBSERVER_EVENT_TOOL_ITERATIONS_EXHAUSTED);
    HU_ASSERT_EQ(rec.tags[1], HU_OBSERVER_EVENT_ERR);
    HU_ASSERT_GT(f.agent.max_tool_iterations, 0);
    HU_ASSERT_EQ(rec.iterations, f.agent.max_tool_iterations);
    HU_ASSERT_STR_EQ(rec.component, "agent");
    HU_ASSERT_STR_EQ(rec.message, "tool iterations exhausted");
    HU_ASSERT_NULL(f.resp);
    hu_turn_ctx_free(turn_ctx);
    tf_close(&f);
}

#ifdef HU_ENABLE_SQLITE
/* Five distinct tool calls against the fixture's 4-iteration cap. */
static const trp_step_t k_tl_exhaust[] = {
    {.err = HU_OK, .tool_calls = {{"c1", "memory_list", "{\"q\":\"1\"}"}}, .tool_calls_count = 1},
    {.err = HU_OK, .tool_calls = {{"c2", "memory_list", "{\"q\":\"2\"}"}}, .tool_calls_count = 1},
    {.err = HU_OK, .tool_calls = {{"c3", "memory_list", "{\"q\":\"3\"}"}}, .tool_calls_count = 1},
    {.err = HU_OK, .tool_calls = {{"c4", "memory_list", "{\"q\":\"4\"}"}}, .tool_calls_count = 1},
    {.err = HU_OK, .tool_calls = {{"c5", "memory_list", "{\"q\":\"5\"}"}}, .tool_calls_count = 1},
};

/* End to end: a turn that resumes an [ACTIVE_PLAN] from history and runs out
 * of tool iterations frees its plan context on the HU_ERR_TIMEOUT exit, as
 * every other exit does. The precondition proves the plan was live this turn
 * (it reached the system prompt as "### [ACTIVE_PLAN] ..."). */
static void turn_exhausted_exit_releases_the_active_plan(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    HU_ASSERT_NOT_NULL(ta);
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open_alloc(&f, hu_tracking_allocator_allocator(ta), k_tl_exhaust,
                                 sizeof(k_tl_exhaust) / sizeof(k_tl_exhaust[0]), true,
                                 HU_AUTONOMY_AUTONOMOUS));
    HU_ASSERT_EQ(f.agent.max_tool_iterations, 4);
    HU_ASSERT_TRUE(tl_append(&f, HU_ROLE_SYSTEM,
                             "[ACTIVE_PLAN] 1/2 steps completed. Remaining: \n  Step 2: list"));
    char *resp = NULL;
    size_t resp_len = 0;
    hu_error_t err = hu_agent_turn(&f.agent, "keep listing", 12, &resp, &resp_len);
    HU_ASSERT_EQ(err, HU_ERR_TIMEOUT);
    HU_ASSERT_NULL(resp);
    HU_ASSERT_NOT_NULL(f.trp.log);
    HU_ASSERT_NOT_NULL(strstr(f.trp.log, "### [ACTIVE_PLAN] 1/2 steps completed."));
    tf_close(&f);
    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}
#endif /* HU_ENABLE_SQLITE */

void run_turn_tail_tests(void) {
    HU_TEST_SUITE("TurnTail");
    HU_RUN_TEST(turn_tail_rejects_a_null_context);
    HU_RUN_TEST(turn_tail_replans_after_two_tool_failures);
    HU_RUN_TEST(turn_tail_replans_in_a_history_shorter_than_the_window);
    HU_RUN_TEST(turn_tail_without_a_plan_does_not_replan);
    HU_RUN_TEST(turn_tail_one_failure_does_not_replan);
    HU_RUN_TEST(turn_tail_ignores_tool_failures_from_a_previous_turn);
    HU_RUN_TEST(turn_tail_replans_once_per_failure_set);
    HU_RUN_TEST(turn_tail_replans_again_for_new_failures_only);
    HU_RUN_TEST(turn_tail_records_the_iteration_in_the_scratchpad);
    HU_RUN_TEST(turn_tail_checkpoints_every_fifth_iteration);
    HU_RUN_TEST(turn_tail_checkpoint_state_is_released_by_agent_deinit);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(turn_tail_folds_memory_relevant_to_the_tool_result_and_the_goal);
    HU_RUN_TEST(turn_tail_skips_goal_memory_on_the_first_iteration);
#endif
    HU_RUN_TEST(turn_exhausted_reports_the_cap_then_an_error);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(turn_exhausted_exit_releases_the_active_plan);
#endif
}
