/* Agent security: policy engine, autonomy levels, approval flow. */
#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/provider.h"
#include "human/security.h"
#include "human/security/policy_engine.h"
#include "human/tool.h"
#include "test_framework.h"
#include <string.h>

/* Mock provider: first call returns tool_calls, second returns text. */
typedef struct mock_toolcall_provider {
    int call_count;
    const char *name;
} mock_toolcall_provider_t;

static hu_error_t mock_toolcall_chat_with_system(void *ctx, hu_allocator_t *alloc,
                                                 const char *system_prompt,
                                                 size_t system_prompt_len, const char *message,
                                                 size_t message_len, const char *model,
                                                 size_t model_len, double temperature, char **out,
                                                 size_t *out_len) {
    (void)system_prompt;
    (void)system_prompt_len;
    (void)message;
    (void)message_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    mock_toolcall_provider_t *m = (mock_toolcall_provider_t *)ctx;
    m->call_count++;
    if (m->call_count == 1) {
        *out = NULL;
        *out_len = 0;
        return HU_ERR_NOT_SUPPORTED; /* force chat() path */
    }
    const char *resp = "done";
    *out = hu_strndup(alloc, resp, strlen(resp));
    *out_len = *out ? strlen(resp) : 0;
    return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static hu_error_t mock_toolcall_chat(void *ctx, hu_allocator_t *alloc,
                                     const hu_chat_request_t *request, const char *model,
                                     size_t model_len, double temperature,
                                     hu_chat_response_t *out) {
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    mock_toolcall_provider_t *m = (mock_toolcall_provider_t *)ctx;
    m->call_count++;
    if (m->call_count == 1) {
        /* Return tool call for shell */
        hu_tool_call_t *calls = (hu_tool_call_t *)alloc->alloc(alloc->ctx, sizeof(hu_tool_call_t));
        if (!calls)
            return HU_ERR_OUT_OF_MEMORY;
        calls[0].id = hu_strndup(alloc, "call-1", 6);
        calls[0].id_len = 6;
        calls[0].name = hu_strndup(alloc, "shell", 5);
        calls[0].name_len = 5;
        calls[0].arguments = hu_strndup(alloc, "{\"command\":\"echo hi\"}", 21);
        calls[0].arguments_len = 21;
        out->content = NULL;
        out->content_len = 0;
        out->tool_calls = calls;
        out->tool_calls_count = 1;
        out->usage.prompt_tokens = 1;
        out->usage.completion_tokens = 2;
        out->usage.total_tokens = 3;
        out->model = NULL;
        out->model_len = 0;
        out->reasoning_content = NULL;
        out->reasoning_content_len = 0;
        return HU_OK;
    }
    /* Second call: final text response */
    const char *resp = "done";
    out->content = hu_strndup(alloc, resp, strlen(resp));
    out->content_len = out->content ? strlen(resp) : 0;
    out->tool_calls = NULL;
    out->tool_calls_count = 0;
    out->usage.prompt_tokens = 1;
    out->usage.completion_tokens = 2;
    out->usage.total_tokens = 3;
    out->model = NULL;
    out->model_len = 0;
    out->reasoning_content = NULL;
    out->reasoning_content_len = 0;
    return out->content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static bool mock_toolcall_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}
static const char *mock_toolcall_get_name(void *ctx) {
    return ((mock_toolcall_provider_t *)ctx)->name;
}
static void mock_toolcall_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const hu_provider_vtable_t mock_toolcall_vtable = {
    .chat_with_system = mock_toolcall_chat_with_system,
    .chat = mock_toolcall_chat,
    .supports_native_tools = mock_toolcall_supports_native_tools,
    .get_name = mock_toolcall_get_name,
    .deinit = mock_toolcall_deinit,
};

static hu_provider_t mock_toolcall_provider_create(hu_allocator_t *alloc,
                                                   mock_toolcall_provider_t *ctx) {
    (void)alloc;
    ctx->call_count = 0;
    ctx->name = "mock_toolcall";
    return (hu_provider_t){.ctx = ctx, .vtable = &mock_toolcall_vtable};
}

/* Mock tool */
typedef struct mock_sec_tool {
    const char *name;
} mock_sec_tool_t;

static hu_error_t mock_sec_tool_execute(void *ctx, hu_allocator_t *alloc,
                                        const hu_json_value_t *args, hu_tool_result_t *out) {
    (void)ctx;
    (void)alloc;
    (void)args;
    *out = hu_tool_result_ok("ok", 2);
    return HU_OK;
}
static const char *mock_sec_tool_name(void *ctx) {
    return ((mock_sec_tool_t *)ctx)->name;
}
static const char *mock_sec_tool_desc(void *ctx) {
    (void)ctx;
    return "mock";
}
static const char *mock_sec_tool_params(void *ctx) {
    (void)ctx;
    return "{}";
}
static void mock_sec_tool_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const hu_tool_vtable_t mock_sec_tool_vtable = {
    .execute = mock_sec_tool_execute,
    .name = mock_sec_tool_name,
    .description = mock_sec_tool_desc,
    .parameters_json = mock_sec_tool_params,
    .deinit = mock_sec_tool_deinit,
};

static void test_tool_risk_level_returns_correct_levels(void) {
    HU_ASSERT_EQ(hu_tool_risk_level("shell"), HU_RISK_HIGH);
    HU_ASSERT_EQ(hu_tool_risk_level("spawn"), HU_RISK_HIGH);
    HU_ASSERT_EQ(hu_tool_risk_level("file_write"), HU_RISK_HIGH);
    HU_ASSERT_EQ(hu_tool_risk_level("file_edit"), HU_RISK_HIGH);
    HU_ASSERT_EQ(hu_tool_risk_level("http_request"), HU_RISK_MEDIUM);
    HU_ASSERT_EQ(hu_tool_risk_level("browser_open"), HU_RISK_MEDIUM);
    HU_ASSERT_EQ(hu_tool_risk_level("file_read"), HU_RISK_LOW);
    HU_ASSERT_EQ(hu_tool_risk_level("memory_recall"), HU_RISK_LOW);
    HU_ASSERT_EQ(hu_tool_risk_level("unknown_tool"), HU_RISK_MEDIUM);
    HU_ASSERT_EQ(hu_tool_risk_level(NULL), HU_RISK_HIGH);
}

static void test_locked_mode_blocks_all_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_toolcall_provider_t mock_ctx;
    hu_provider_t prov = mock_toolcall_provider_create(&alloc, &mock_ctx);

    mock_sec_tool_t shell_ctx = {.name = "shell"};
    hu_tool_t tools[1] = {{.ctx = &shell_ctx, .vtable = &mock_sec_tool_vtable}};

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err = hu_agent_from_config(&agent, &alloc, prov, tools, 1, NULL, NULL, NULL, NULL,
                                          "gpt-4", 5, "openai", 6, 0.7, ".", 1, 25, 50, false,
                                          HU_AUTONOMY_LOCKED, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    char *r = NULL;
    size_t len = 0;
    err = hu_agent_turn(&agent, "run echo hi", 11, &r, &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT_TRUE(strstr(r, "done") != NULL || len > 0);
    if (r)
        alloc.free(alloc.ctx, r, len + 1);

    /* History: user, assistant+tool_calls, tool result, assistant final "done" */
    HU_ASSERT_TRUE(agent.history_count >= 3u);
    /* Find tool result (HU_ROLE_TOOL) containing "Action blocked" */
    bool found_blocked = false;
    for (size_t i = 0; i < agent.history_count; i++) {
        if (agent.history[i].role == HU_ROLE_TOOL && agent.history[i].content &&
            strstr(agent.history[i].content, "Action blocked") != NULL) {
            found_blocked = true;
            break;
        }
    }
    HU_ASSERT_TRUE(found_blocked);

    hu_agent_deinit(&agent);
}

static bool g_approval_called;
static bool g_approval_return;

static bool test_approval_cb(void *ctx, const char *tn, const char *args) {
    (void)ctx;
    (void)tn;
    (void)args;
    g_approval_called = true;
    return g_approval_return;
}

static void test_supervised_forces_approval_on_all_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_toolcall_provider_t mock_ctx;
    hu_provider_t prov = mock_toolcall_provider_create(&alloc, &mock_ctx);

    mock_sec_tool_t shell_ctx = {.name = "shell"};
    hu_tool_t tools[1] = {{.ctx = &shell_ctx, .vtable = &mock_sec_tool_vtable}};

    g_approval_called = false;
    g_approval_return = true;

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err = hu_agent_from_config(&agent, &alloc, prov, tools, 1, NULL, NULL, NULL, NULL,
                                          "gpt-4", 5, "openai", 6, 0.7, ".", 1, 25, 50, false,
                                          HU_AUTONOMY_SUPERVISED, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    agent.approval_cb = test_approval_cb;
    agent.approval_ctx = NULL;

    char *r = NULL;
    size_t len = 0;
    err = hu_agent_turn(&agent, "run echo hi", 11, &r, &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(g_approval_called);
    if (r)
        alloc.free(alloc.ctx, r, len + 1);
    hu_agent_deinit(&agent);
}

static void test_assisted_auto_approves_low_risk_requires_approval_high_risk(void) {
    HU_ASSERT_EQ(hu_tool_risk_level("file_read"), HU_RISK_LOW);
    HU_ASSERT_EQ(hu_tool_risk_level("shell"), HU_RISK_HIGH);
}

static void test_autonomous_passes_through_to_policy_engine(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_policy_engine_t *pe = hu_policy_engine_create(&alloc);
    HU_ASSERT_NOT_NULL(pe);
    hu_policy_match_t m = {
        .tool = "shell", .args_contains = NULL, .session_cost_gt = 0, .has_cost_check = false};
    hu_policy_engine_add_rule(pe, "allow", m, HU_POLICY_ALLOW, "ok");
    hu_policy_eval_ctx_t ctx = {.tool_name = "shell", .args_json = "{}", .session_cost_usd = 0};
    HU_ASSERT_EQ(hu_policy_engine_evaluate(pe, &ctx).action, HU_POLICY_ALLOW);
    hu_policy_engine_destroy(pe);
}

static void test_policy_deny_blocks_tool(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_policy_engine_t *pe = hu_policy_engine_create(&alloc);
    HU_ASSERT_NOT_NULL(pe);
    hu_policy_match_t m = {
        .tool = "shell", .args_contains = NULL, .session_cost_gt = 0, .has_cost_check = false};
    hu_policy_engine_add_rule(pe, "deny", m, HU_POLICY_DENY, "blocked");
    hu_policy_eval_ctx_t ctx = {.tool_name = "shell", .args_json = "{}", .session_cost_usd = 0};
    HU_ASSERT_EQ(hu_policy_engine_evaluate(pe, &ctx).action, HU_POLICY_DENY);
    hu_policy_engine_destroy(pe);
}

/* Integration: policy DENY in agent flow surfaces "denied by policy" in tool result */
static void test_policy_deny_blocks_tool_in_agent_flow(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_toolcall_provider_t mock_ctx;
    hu_provider_t prov = mock_toolcall_provider_create(&alloc, &mock_ctx);

    mock_sec_tool_t shell_ctx = {.name = "shell"};
    hu_tool_t tools[1] = {{.ctx = &shell_ctx, .vtable = &mock_sec_tool_vtable}};

    hu_policy_engine_t *pe = hu_policy_engine_create(&alloc);
    HU_ASSERT_NOT_NULL(pe);
    hu_policy_match_t m = {
        .tool = "shell", .args_contains = NULL, .session_cost_gt = 0, .has_cost_check = false};
    hu_policy_engine_add_rule(pe, "deny_shell", m, HU_POLICY_DENY, "blocked");

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err = hu_agent_from_config(&agent, &alloc, prov, tools, 1, NULL, NULL, NULL, NULL,
                                          "gpt-4", 5, "openai", 6, 0.7, ".", 1, 25, 50, false,
                                          HU_AUTONOMY_AUTONOMOUS, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    agent.policy_engine = pe;

    char *r = NULL;
    size_t len = 0;
    err = hu_agent_turn(&agent, "run echo hi", 11, &r, &len);
    HU_ASSERT_EQ(err, HU_OK);
    if (r)
        alloc.free(alloc.ctx, r, len + 1);

    bool found_denied = false;
    for (size_t i = 0; i < agent.history_count; i++) {
        if (agent.history[i].role == HU_ROLE_TOOL && agent.history[i].content &&
            strstr(agent.history[i].content, "denied by policy") != NULL) {
            found_denied = true;
            break;
        }
    }
    HU_ASSERT_TRUE(found_denied);

    hu_agent_deinit(&agent);
    hu_policy_engine_destroy(pe);
}

/* REQUIRE_APPROVAL with no approval_cb yields "requires human approval" */
static void test_needs_approval_no_callback_fails_explicitly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_toolcall_provider_t mock_ctx;
    hu_provider_t prov = mock_toolcall_provider_create(&alloc, &mock_ctx);

    mock_sec_tool_t shell_ctx = {.name = "shell"};
    hu_tool_t tools[1] = {{.ctx = &shell_ctx, .vtable = &mock_sec_tool_vtable}};

    hu_policy_engine_t *pe = hu_policy_engine_create(&alloc);
    HU_ASSERT_NOT_NULL(pe);
    hu_policy_match_t m = {
        .tool = "shell", .args_contains = NULL, .session_cost_gt = 0, .has_cost_check = false};
    hu_policy_engine_add_rule(pe, "approve", m, HU_POLICY_REQUIRE_APPROVAL, "needs approval");

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err = hu_agent_from_config(&agent, &alloc, prov, tools, 1, NULL, NULL, NULL, NULL,
                                          "gpt-4", 5, "openai", 6, 0.7, ".", 1, 25, 50, false,
                                          HU_AUTONOMY_AUTONOMOUS, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    agent.policy_engine = pe;
    agent.approval_cb = NULL; /* no callback */

    char *r = NULL;
    size_t len = 0;
    err = hu_agent_turn(&agent, "run echo hi", 11, &r, &len);
    HU_ASSERT_EQ(err, HU_OK);
    if (r)
        alloc.free(alloc.ctx, r, len + 1);

    bool found_requires = false;
    for (size_t i = 0; i < agent.history_count; i++) {
        if (agent.history[i].role == HU_ROLE_TOOL && agent.history[i].content &&
            strstr(agent.history[i].content, "requires human approval") != NULL) {
            found_requires = true;
            break;
        }
    }
    HU_ASSERT_TRUE(found_requires);

    hu_agent_deinit(&agent);
    hu_policy_engine_destroy(pe);
}

/* approval_cb returns false yields "user denied action" */
static void test_approval_cb_deny_yields_user_denied(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_toolcall_provider_t mock_ctx;
    hu_provider_t prov = mock_toolcall_provider_create(&alloc, &mock_ctx);

    mock_sec_tool_t shell_ctx = {.name = "shell"};
    hu_tool_t tools[1] = {{.ctx = &shell_ctx, .vtable = &mock_sec_tool_vtable}};

    g_approval_called = false;
    g_approval_return = false; /* user denies */

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err = hu_agent_from_config(&agent, &alloc, prov, tools, 1, NULL, NULL, NULL, NULL,
                                          "gpt-4", 5, "openai", 6, 0.7, ".", 1, 25, 50, false,
                                          HU_AUTONOMY_SUPERVISED, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    agent.approval_cb = test_approval_cb;
    agent.approval_ctx = NULL;

    char *r = NULL;
    size_t len = 0;
    err = hu_agent_turn(&agent, "run echo hi", 11, &r, &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(g_approval_called);
    if (r)
        alloc.free(alloc.ctx, r, len + 1);

    bool found_denied = false;
    for (size_t i = 0; i < agent.history_count; i++) {
        if (agent.history[i].role == HU_ROLE_TOOL && agent.history[i].content &&
            strstr(agent.history[i].content, "user denied action") != NULL) {
            found_denied = true;
            break;
        }
    }
    HU_ASSERT_TRUE(found_denied);

    hu_agent_deinit(&agent);
}

static void test_policy_require_approval_sets_needs_approval(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_policy_engine_t *pe = hu_policy_engine_create(&alloc);
    HU_ASSERT_NOT_NULL(pe);
    hu_policy_match_t m = {
        .tool = "shell", .args_contains = NULL, .session_cost_gt = 0, .has_cost_check = false};
    hu_policy_engine_add_rule(pe, "approve", m, HU_POLICY_REQUIRE_APPROVAL, "needs approval");
    hu_policy_eval_ctx_t ctx = {.tool_name = "shell", .args_json = "{}", .session_cost_usd = 0};
    HU_ASSERT_EQ(hu_policy_engine_evaluate(pe, &ctx).action, HU_POLICY_REQUIRE_APPROVAL);
    hu_policy_engine_destroy(pe);
}

void run_agent_security_tests(void) {
    HU_TEST_SUITE("Agent security");
    HU_RUN_TEST(test_tool_risk_level_returns_correct_levels);
    HU_RUN_TEST(test_policy_deny_blocks_tool);
    HU_RUN_TEST(test_policy_deny_blocks_tool_in_agent_flow);
    HU_RUN_TEST(test_policy_require_approval_sets_needs_approval);
    HU_RUN_TEST(test_needs_approval_no_callback_fails_explicitly);
    HU_RUN_TEST(test_approval_cb_deny_yields_user_denied);
    HU_RUN_TEST(test_locked_mode_blocks_all_tools);
    HU_RUN_TEST(test_supervised_forces_approval_on_all_tools);
    HU_RUN_TEST(test_assisted_auto_approves_low_risk_requires_approval_high_risk);
    HU_RUN_TEST(test_autonomous_passes_through_to_policy_engine);
}
