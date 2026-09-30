/* tests/test_turn_ctx.c — contract tests for the per-turn context
 * (src/agent/turn/turn_ctx.c) and the hu_agent_turn wrapper that owns it.
 * Pins ownership (Review Focus 3), a nested turn (Focus 4) and OOM at the
 * turn's first allocation (Focus 5). */
#include "human/agent.h"
#include "human/agent/turn.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "test_framework.h"
#include "turn_test_fixture.h"
#include <string.h>

static void turn_ctx_new_records_inputs_and_starts_empty(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    HU_ASSERT_NOT_NULL(ta);
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    const char *msg = "hi";
    char sentinel = 'x';
    char *resp = &sentinel;
    size_t resp_len = 7;
    hu_turn_ctx_t *turn_ctx = hu_turn_ctx_new(&agent, msg, 2, &resp, &resp_len);
    HU_ASSERT_NOT_NULL(turn_ctx);
    HU_ASSERT_TRUE(turn_ctx->alloc == &alloc);
    HU_ASSERT_TRUE(turn_ctx->in.agent == &agent);
    HU_ASSERT_TRUE(turn_ctx->in.msg == msg);
    HU_ASSERT_EQ(turn_ctx->in.msg_len, 2);
    HU_ASSERT_TRUE(turn_ctx->in.response_out == &resp);
    HU_ASSERT_TRUE(turn_ctx->in.response_len_out == &resp_len);
    HU_ASSERT_NULL(turn_ctx->retrieval.memory_ctx);
    HU_ASSERT_NULL(turn_ctx->retrieval.graph_ctx);
    HU_ASSERT_FALSE(turn_ctx->retrieval.memory_ctx_nonempty);
    /* the turn body owns the out-params; creating the context must not touch them */
    HU_ASSERT_TRUE(resp == &sentinel);
    HU_ASSERT_EQ(resp_len, 7);
    hu_turn_ctx_free(turn_ctx);
    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

static void turn_ctx_new_needs_an_agent_with_an_allocator(void) {
    HU_ASSERT_NULL(hu_turn_ctx_new(NULL, "hi", 2, NULL, NULL));
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent)); /* alloc == NULL */
    HU_ASSERT_NULL(hu_turn_ctx_new(&agent, "hi", 2, NULL, NULL));
}

/* A stage output that was never unpacked is still owned by the context. */
static void turn_ctx_free_releases_still_owned_outputs(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    HU_ASSERT_NOT_NULL(ta);
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    hu_turn_ctx_t *turn_ctx = hu_turn_ctx_new(&agent, "hi", 2, NULL, NULL);
    HU_ASSERT_NOT_NULL(turn_ctx);
    turn_ctx->retrieval.memory_ctx = hu_strndup(&alloc, "mem", 3);
    turn_ctx->retrieval.memory_ctx_len = 3;
    turn_ctx->retrieval.graph_ctx = hu_strndup(&alloc, "graph", 5);
    turn_ctx->retrieval.graph_ctx_len = 5;
    HU_ASSERT_GT(hu_tracking_allocator_leaks(ta), 0); /* precondition: live allocations */
    hu_turn_ctx_free(turn_ctx);
    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

static void *tc_fail_alloc(void *ctx, size_t size) {
    (void)ctx;
    (void)size;
    return NULL;
}
static void *tc_fail_realloc(void *ctx, void *p, size_t old_size, size_t new_size) {
    (void)ctx;
    (void)p;
    (void)old_size;
    (void)new_size;
    return NULL;
}
static void tc_fail_free(void *ctx, void *p, size_t size) {
    (void)ctx;
    (void)p;
    (void)size;
}

/* Review Focus 5: the context is the first allocation of a turn. */
static void agent_turn_reports_oom_when_the_turn_context_cannot_be_allocated(void) {
    tf_fixture_t f;
    HU_ASSERT_TRUE(tf_open(&f, NULL, 0, false, HU_AUTONOMY_AUTONOMOUS));
    hu_allocator_t failing = {
        .ctx = NULL, .alloc = tc_fail_alloc, .realloc = tc_fail_realloc, .free = tc_fail_free};
    hu_allocator_t *real = f.agent.alloc;
    f.agent.alloc = &failing;
    char sentinel = 'x';
    char *resp = &sentinel;
    size_t resp_len = 99;
    hu_error_t err = hu_agent_turn(&f.agent, "hello", 5, &resp, &resp_len);
    f.agent.alloc = real; /* restore before any assert can longjmp */
    size_t calls = f.trp.calls;
    tf_close(&f);
    HU_ASSERT_EQ(err, HU_ERR_OUT_OF_MEMORY);
    HU_ASSERT_NULL(resp);
    HU_ASSERT_EQ(resp_len, 0);
    HU_ASSERT_EQ(calls, 0);
}

/* Review Focus 4: a tool that runs a whole turn on a second agent (spawn.c
 * does this) — each hu_agent_turn call must get its own heap context. */
typedef struct tc_nested {
    hu_agent_t *child;
    bool child_ok;
} tc_nested_t;

static hu_error_t tc_nested_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                    hu_tool_result_t *out) {
    (void)alloc;
    (void)args;
    tc_nested_t *n = (tc_nested_t *)ctx;
    char *r = NULL;
    size_t rl = 0;
    hu_error_t e = hu_agent_turn(n->child, "hi", 2, &r, &rl);
    n->child_ok = e == HU_OK && r && strcmp(r, "all set here") == 0;
    if (r)
        n->child->alloc->free(n->child->alloc->ctx, r, rl + 1);
    *out = n->child_ok ? hu_tool_result_ok("child ok", 8) : hu_tool_result_fail("child failed", 12);
    return HU_OK;
}

static void agent_turn_nested_turn_gets_its_own_context(void) {
    static const trp_step_t parent_script[] = {
        {.err = HU_OK, .tool_calls = {{"call_1", "memory_list", "{}"}}, .tool_calls_count = 1},
        {.err = HU_OK, .content = "parent done"},
    };
    /* The child's scripted reply must survive the outbound validator chain on
     * the first try ("child done" is rejected and repaired to the off-script
     * "ok."), or the comparison below measures the repair, not the nesting. */
    static const trp_step_t child_script[] = {{.err = HU_OK, .content = "all set here"}};
    static const hu_tool_vtable_t nested_vt = {
        .execute = tc_nested_execute,
        .name = tf_tool_name,
        .description = tf_tool_desc,
        .parameters_json = tf_tool_params,
    };
    tf_fixture_t parent, child;
    HU_ASSERT_TRUE(tf_open(&parent, parent_script, 2, false, HU_AUTONOMY_AUTONOMOUS));
    HU_ASSERT_TRUE(tf_open(&child, child_script, 1, false, HU_AUTONOMY_AUTONOMOUS));
    tc_nested_t nested = {.child = &child.agent, .child_ok = false};
    parent.agent.tools[0].ctx = &nested;
    parent.agent.tools[0].vtable = &nested_vt;
    hu_error_t err =
        hu_agent_turn(&parent.agent, "use the tool", 12, &parent.resp, &parent.resp_len);
    bool parent_saw_child = strstr(parent.trp.log, "child ok") != NULL;
    bool parent_done = parent.resp && strcmp(parent.resp, "parent done") == 0;
    tf_close(&child);
    tf_close(&parent);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(nested.child_ok);
    HU_ASSERT_TRUE(parent_saw_child);
    HU_ASSERT_TRUE(parent_done);
}

void run_turn_ctx_tests(void) {
    HU_TEST_SUITE("TurnCtx");
    HU_RUN_TEST(turn_ctx_new_records_inputs_and_starts_empty);
    HU_RUN_TEST(turn_ctx_new_needs_an_agent_with_an_allocator);
    HU_RUN_TEST(turn_ctx_free_releases_still_owned_outputs);
    HU_RUN_TEST(agent_turn_reports_oom_when_the_turn_context_cannot_be_allocated);
    HU_RUN_TEST(agent_turn_nested_turn_gets_its_own_context);
}
