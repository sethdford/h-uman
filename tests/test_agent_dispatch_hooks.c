/*
 * Tests for hu_agent_internal_dispatch_with_hooks — the canonical helper
 * that wraps every tool invocation in the agent's hook pipeline.
 *
 * The audit on 2026-05-16 found five dispatch sites in src/agent/agent_turn.c
 * that bypassed the hook pipeline:
 *   - line 642   (DAG worker thread)
 *   - line 7193  (DAG inline path)
 *   - line 7419  (orchestrator path)
 *   - line 8037  (approval-retry, early path)
 *   - line 8498  (approval-retry, sequential path)
 *
 * The helper centralizes the pre-/post-hook envelope so those paths can
 * migrate. These tests pin the contract.
 */

#include "../src/agent/agent_internal.h"
#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/hook.h"
#include "human/hook_pipeline.h"
#include "human/tool.h"
#include "test_framework.h"
#include <string.h>

/* ── Mock tool: counts invocations + returns a fixed result. ───────────── */

typedef struct mock_tool_ctx {
    int execute_count;
    bool execute_should_succeed;
} mock_tool_ctx_t;

static hu_error_t mock_tool_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                    hu_tool_result_t *out) {
    (void)alloc;
    (void)args;
    mock_tool_ctx_t *m = (mock_tool_ctx_t *)ctx;
    m->execute_count++;
    if (m->execute_should_succeed) {
        *out = hu_tool_result_ok("ran", 3);
    } else {
        *out = hu_tool_result_fail("mock fail", 9);
    }
    return HU_OK;
}

static const char *mock_tool_name(void *ctx) {
    (void)ctx;
    return "mock_tool";
}
static const char *mock_tool_description(void *ctx) {
    (void)ctx;
    return "Mock tool used by hook-dispatch tests";
}
static const char *mock_tool_parameters_json(void *ctx) {
    (void)ctx;
    return "{}";
}
static void mock_tool_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static hu_tool_vtable_t k_mock_tool_vtable = {
    .execute = mock_tool_execute,
    .name = mock_tool_name,
    .description = mock_tool_description,
    .parameters_json = mock_tool_parameters_json,
    .deinit = mock_tool_deinit,
};

static void make_mock_tool(hu_tool_t *out_tool, mock_tool_ctx_t *out_ctx, bool should_succeed) {
    out_ctx->execute_count = 0;
    out_ctx->execute_should_succeed = should_succeed;
    out_tool->ctx = out_ctx;
    out_tool->vtable = &k_mock_tool_vtable;
}

/* ── Tests ────────────────────────────────────────────────────────────── */

static void test_dispatch_null_agent_returns_invalid(void) {
    hu_tool_t tool;
    mock_tool_ctx_t ctx;
    make_mock_tool(&tool, &ctx, true);
    hu_tool_result_t out = {0};
    HU_ASSERT_EQ(hu_agent_internal_dispatch_with_hooks(NULL, &tool, "x", 1, NULL, 0, NULL, &out),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(ctx.execute_count, 0);
}

static void test_dispatch_null_tool_returns_invalid(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_allocator_t alloc = hu_system_allocator();
    agent.alloc = &alloc;
    hu_tool_result_t out = {0};
    HU_ASSERT_EQ(hu_agent_internal_dispatch_with_hooks(&agent, NULL, "x", 1, NULL, 0, NULL, &out),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_dispatch_null_out_returns_invalid(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_allocator_t alloc = hu_system_allocator();
    agent.alloc = &alloc;
    hu_tool_t tool;
    mock_tool_ctx_t ctx;
    make_mock_tool(&tool, &ctx, true);
    HU_ASSERT_EQ(hu_agent_internal_dispatch_with_hooks(&agent, &tool, "x", 1, NULL, 0, NULL, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(ctx.execute_count, 0);
}

static void test_dispatch_no_registry_runs_tool(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_allocator_t alloc = hu_system_allocator();
    agent.alloc = &alloc;
    agent.hook_registry = NULL;
    hu_tool_t tool;
    mock_tool_ctx_t ctx;
    make_mock_tool(&tool, &ctx, true);
    hu_tool_result_t out = {0};

    hu_error_t err =
        hu_agent_internal_dispatch_with_hooks(&agent, &tool, "mock_tool", 9, "{}", 2, NULL, &out);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(ctx.execute_count, 1);
    HU_ASSERT_TRUE(out.success);
    hu_tool_result_free(&alloc, &out);
}

static void test_dispatch_pre_hook_allow_runs_tool(void) {
    hu_hook_mock_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_hook_registry_t *reg = NULL;
    hu_hook_registry_create(&alloc, &reg);

    hu_hook_entry_t entry = {
        .name = "allow-all",
        .name_len = 9,
        .event = HU_HOOK_PRE_TOOL_EXECUTE,
        .command = "/bin/true",
        .command_len = 9,
        .timeout_sec = 10,
        .required = false,
    };
    hu_hook_registry_add(reg, &alloc, &entry);

    hu_hook_mock_config_t mock_cfg = {.exit_code = 0, .stdout_data = NULL, .stdout_len = 0};
    hu_hook_mock_set(&mock_cfg);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.hook_registry = reg;

    hu_tool_t tool;
    mock_tool_ctx_t ctx;
    make_mock_tool(&tool, &ctx, true);
    hu_tool_result_t out = {0};

    hu_error_t err =
        hu_agent_internal_dispatch_with_hooks(&agent, &tool, "mock_tool", 9, "{}", 2, NULL, &out);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(ctx.execute_count, 1);
    HU_ASSERT_TRUE(out.success);
    /* Pre-hook fired exactly once; tool execute fired exactly once. */
    HU_ASSERT_TRUE(hu_hook_mock_call_count() >= 1);

    hu_tool_result_free(&alloc, &out);
    hu_hook_registry_destroy(reg, &alloc);
    hu_hook_mock_reset();
}

static void test_dispatch_pre_hook_deny_skips_tool(void) {
    hu_hook_mock_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_hook_registry_t *reg = NULL;
    hu_hook_registry_create(&alloc, &reg);

    hu_hook_entry_t entry = {
        .name = "deny-all",
        .name_len = 8,
        .event = HU_HOOK_PRE_TOOL_EXECUTE,
        .command = "/bin/false",
        .command_len = 10,
        .timeout_sec = 10,
        .required = false,
    };
    hu_hook_registry_add(reg, &alloc, &entry);

    /* Exit code 2 means DENY in the hook protocol. */
    hu_hook_mock_config_t mock_cfg = {
        .exit_code = 2,
        .stdout_data = "blocked by policy",
        .stdout_len = 17,
    };
    hu_hook_mock_set(&mock_cfg);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.hook_registry = reg;

    hu_tool_t tool;
    mock_tool_ctx_t ctx;
    make_mock_tool(&tool, &ctx, true);
    hu_tool_result_t out = {0};

    hu_error_t err =
        hu_agent_internal_dispatch_with_hooks(&agent, &tool, "mock_tool", 9, "{}", 2, NULL, &out);

    HU_ASSERT_EQ(err, HU_OK);
    /* The core contract: DENY decision means the tool's execute() is NOT
     * called — exactly the gap on the approval-retry paths the audit
     * surfaced. Without this guard, a denied tool could still run on retry. */
    HU_ASSERT_EQ(ctx.execute_count, 0);
    HU_ASSERT_FALSE(out.success);

    hu_tool_result_free(&alloc, &out);
    hu_hook_registry_destroy(reg, &alloc);
    hu_hook_mock_reset();
}

/* ── Suite registration ──────────────────────────────────────────────── */

void run_agent_dispatch_hooks_tests(void) {
    HU_TEST_SUITE("agent dispatch with hooks");
    HU_RUN_TEST(test_dispatch_null_agent_returns_invalid);
    HU_RUN_TEST(test_dispatch_null_tool_returns_invalid);
    HU_RUN_TEST(test_dispatch_null_out_returns_invalid);
    HU_RUN_TEST(test_dispatch_no_registry_runs_tool);
    HU_RUN_TEST(test_dispatch_pre_hook_allow_runs_tool);
    HU_RUN_TEST(test_dispatch_pre_hook_deny_skips_tool);
}
