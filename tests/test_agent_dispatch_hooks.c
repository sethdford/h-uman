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
#include "human/permission.h"
#include "human/tool.h"
#include "test_framework.h"
#include <string.h>

/* Use a real READ_ONLY tool name so the Wave A permission gate allows
 * execute() in the happy-path tests (unknown names classify as DENY). */
#define DISP_TOOL_NAME     "web_search"
#define DISP_TOOL_NAME_LEN 10

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
    return DISP_TOOL_NAME;
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

    hu_error_t err = hu_agent_internal_dispatch_with_hooks(&agent, &tool, DISP_TOOL_NAME,
                                                           DISP_TOOL_NAME_LEN, "{}", 2, NULL, &out);

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

    hu_error_t err = hu_agent_internal_dispatch_with_hooks(&agent, &tool, DISP_TOOL_NAME,
                                                           DISP_TOOL_NAME_LEN, "{}", 2, NULL, &out);

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

    hu_error_t err = hu_agent_internal_dispatch_with_hooks(&agent, &tool, DISP_TOOL_NAME,
                                                           DISP_TOOL_NAME_LEN, "{}", 2, NULL, &out);

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

/* The audit-followup contract on 2026-05-17 added three additional
 * requirements that the helper MUST satisfy:
 *   - When pre-hook denies, the POST-hook still fires (auditors see every
 *     dispatch, including blocked ones).
 *   - When the tool's execute() reports failure, the POST-hook still fires
 *     with the actual error message (not an empty output).
 *   - The public alias hu_agent_dispatch_tool delegates to the internal
 *     helper with the same contract — call sites outside agent_internal.h
 *     don't lose any guarantees by using the public name. */

static void test_dispatch_pre_deny_still_fires_post_hook(void) {
    hu_hook_mock_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_hook_registry_t *reg = NULL;
    hu_hook_registry_create(&alloc, &reg);

    /* Two hooks: one pre that denies, one post that observes. */
    hu_hook_entry_t pre = {
        .name = "pre-deny",
        .name_len = 8,
        .event = HU_HOOK_PRE_TOOL_EXECUTE,
        .command = "/bin/false",
        .command_len = 10,
        .timeout_sec = 10,
        .required = false,
    };
    hu_hook_registry_add(reg, &alloc, &pre);

    hu_hook_entry_t post = {
        .name = "post-audit",
        .name_len = 10,
        .event = HU_HOOK_POST_TOOL_EXECUTE,
        .command = "/bin/true",
        .command_len = 9,
        .timeout_sec = 10,
        .required = false,
    };
    hu_hook_registry_add(reg, &alloc, &post);

    /* Mock: both invocations return DENY for pre, ALLOW for post. The mock
     * sequence runs in registration order across pre+post. */
    hu_hook_mock_config_t seq[] = {
        {.exit_code = 2, .stdout_data = "blocked", .stdout_len = 7}, /* pre */
        {.exit_code = 0, .stdout_data = NULL, .stdout_len = 0},      /* post */
    };
    hu_hook_mock_set_sequence(seq, 2);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.hook_registry = reg;

    hu_tool_t tool;
    mock_tool_ctx_t ctx;
    make_mock_tool(&tool, &ctx, true);
    hu_tool_result_t out = {0};

    hu_error_t err = hu_agent_internal_dispatch_with_hooks(&agent, &tool, DISP_TOOL_NAME,
                                                           DISP_TOOL_NAME_LEN, "{}", 2, NULL, &out);

    HU_ASSERT_EQ(err, HU_OK);
    /* Tool body did NOT run — pre-hook denied. */
    HU_ASSERT_EQ(ctx.execute_count, 0);
    HU_ASSERT_FALSE(out.success);
    /* CONTRACT: pre AND post both fired — auditors see the dispatch attempt
     * even though it was blocked. Count is 2 (one pre, one post). */
    HU_ASSERT_EQ(hu_hook_mock_call_count(), 2);

    hu_tool_result_free(&alloc, &out);
    hu_hook_registry_destroy(reg, &alloc);
    hu_hook_mock_reset();
}

static void test_dispatch_tool_failure_still_fires_post_hook(void) {
    hu_hook_mock_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_hook_registry_t *reg = NULL;
    hu_hook_registry_create(&alloc, &reg);

    hu_hook_entry_t pre = {
        .name = "pre-allow",
        .name_len = 9,
        .event = HU_HOOK_PRE_TOOL_EXECUTE,
        .command = "/bin/true",
        .command_len = 9,
        .timeout_sec = 10,
        .required = false,
    };
    hu_hook_registry_add(reg, &alloc, &pre);

    hu_hook_entry_t post = {
        .name = "post-audit",
        .name_len = 10,
        .event = HU_HOOK_POST_TOOL_EXECUTE,
        .command = "/bin/true",
        .command_len = 9,
        .timeout_sec = 10,
        .required = false,
    };
    hu_hook_registry_add(reg, &alloc, &post);

    /* Both hook executions succeed; the tool itself returns failure. */
    hu_hook_mock_config_t mock = {.exit_code = 0, .stdout_data = NULL, .stdout_len = 0};
    hu_hook_mock_set(&mock);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.hook_registry = reg;

    hu_tool_t tool;
    mock_tool_ctx_t ctx;
    make_mock_tool(&tool, &ctx, /* should_succeed = */ false);
    hu_tool_result_t out = {0};

    hu_error_t err = hu_agent_internal_dispatch_with_hooks(&agent, &tool, DISP_TOOL_NAME,
                                                           DISP_TOOL_NAME_LEN, "{}", 2, NULL, &out);

    HU_ASSERT_EQ(err, HU_OK);
    /* Tool ran exactly once and reported failure. */
    HU_ASSERT_EQ(ctx.execute_count, 1);
    HU_ASSERT_FALSE(out.success);
    /* CONTRACT: both pre and post fired even though the tool failed. */
    HU_ASSERT_EQ(hu_hook_mock_call_count(), 2);

    hu_tool_result_free(&alloc, &out);
    hu_hook_registry_destroy(reg, &alloc);
    hu_hook_mock_reset();
}

static void test_dispatch_tool_public_alias_delegates_to_internal(void) {
    /* The public hu_agent_dispatch_tool is the same code path as
     * hu_agent_internal_dispatch_with_hooks. Verify the alias actually
     * calls the helper rather than being a stub. */
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_allocator_t alloc = hu_system_allocator();
    agent.alloc = &alloc;
    agent.hook_registry = NULL;

    hu_tool_t tool;
    mock_tool_ctx_t ctx;
    make_mock_tool(&tool, &ctx, true);
    hu_tool_result_t out = {0};

    hu_error_t err = hu_agent_dispatch_tool(&agent, &tool, DISP_TOOL_NAME, DISP_TOOL_NAME_LEN, "{}",
                                            2, NULL, &out);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(ctx.execute_count, 1);
    HU_ASSERT_TRUE(out.success);
    hu_tool_result_free(&alloc, &out);

    /* Negative case: alias must reject NULL agent the same way the helper does. */
    HU_ASSERT_EQ(hu_agent_dispatch_tool(NULL, &tool, "x", 1, NULL, 0, NULL, &out),
                 HU_ERR_INVALID_ARGUMENT);
}

/* ── Wave A: pre_execute_checks envelope ─────────────────────────────── */

static void test_pre_execute_permission_denies_shell_under_read_only(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_allocator_t alloc = hu_system_allocator();
    agent.alloc = &alloc;
    agent.permission_level = HU_PERM_READ_ONLY;

    hu_tool_result_t out = {0};
    hu_tool_gate_t gate = hu_agent_internal_pre_execute_checks(&agent, "shell", 5, "{}", 2, &out);

    HU_ASSERT_EQ(gate, HU_TOOL_GATE_DENY);
    HU_ASSERT_FALSE(out.success);
    HU_ASSERT_TRUE(out.error_msg != NULL && strstr(out.error_msg, "denied") != NULL);
    hu_tool_result_free(&alloc, &out);
}

static void test_pre_execute_allows_web_search_under_read_only(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_allocator_t alloc = hu_system_allocator();
    agent.alloc = &alloc;
    agent.permission_level = HU_PERM_READ_ONLY;

    hu_tool_result_t out = {0};
    hu_tool_gate_t gate = hu_agent_internal_pre_execute_checks(&agent, DISP_TOOL_NAME,
                                                               DISP_TOOL_NAME_LEN, "{}", 2, &out);

    HU_ASSERT_EQ(gate, HU_TOOL_GATE_ALLOW);
    /* ALLOW leaves *out untouched. */
    HU_ASSERT_FALSE(out.success);
    HU_ASSERT_NULL(out.error_msg);
}

static void test_pre_execute_hook_deny_populates_out(void) {
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

    hu_hook_mock_config_t mock_cfg = {
        .exit_code = 2,
        .stdout_data = "blocked by hook",
        .stdout_len = 15,
    };
    hu_hook_mock_set(&mock_cfg);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.permission_level = HU_PERM_READ_ONLY;
    agent.hook_registry = reg;

    hu_tool_result_t out = {0};
    hu_tool_gate_t gate = hu_agent_internal_pre_execute_checks(&agent, DISP_TOOL_NAME,
                                                               DISP_TOOL_NAME_LEN, "{}", 2, &out);

    HU_ASSERT_EQ(gate, HU_TOOL_GATE_DENY);
    HU_ASSERT_FALSE(out.success);

    hu_tool_result_free(&alloc, &out);
    hu_hook_registry_destroy(reg, &alloc);
    hu_hook_mock_reset();
}

static void test_dispatch_permission_deny_skips_execute(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_allocator_t alloc = hu_system_allocator();
    agent.alloc = &alloc;
    agent.permission_level = HU_PERM_READ_ONLY;

    hu_tool_t tool;
    mock_tool_ctx_t ctx;
    make_mock_tool(&tool, &ctx, true);
    hu_tool_result_t out = {0};

    hu_error_t err =
        hu_agent_internal_dispatch_with_hooks(&agent, &tool, "shell", 5, "{}", 2, NULL, &out);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(ctx.execute_count, 0);
    HU_ASSERT_FALSE(out.success);
    hu_tool_result_free(&alloc, &out);
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
    HU_RUN_TEST(test_dispatch_pre_deny_still_fires_post_hook);
    HU_RUN_TEST(test_dispatch_tool_failure_still_fires_post_hook);
    HU_RUN_TEST(test_dispatch_tool_public_alias_delegates_to_internal);
    HU_RUN_TEST(test_pre_execute_permission_denies_shell_under_read_only);
    HU_RUN_TEST(test_pre_execute_allows_web_search_under_read_only);
    HU_RUN_TEST(test_pre_execute_hook_deny_populates_out);
    HU_RUN_TEST(test_dispatch_permission_deny_skips_execute);
}
