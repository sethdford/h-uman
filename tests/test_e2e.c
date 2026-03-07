#include "seaclaw/agent.h"
#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/health.h"
#include "seaclaw/observability/log_observer.h"
#include "seaclaw/provider.h"
#include "seaclaw/tool.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─────────────────────────────────────────────────────────────────────────
 * Mock provider — returns fixed "mock response" from chat()
 * ───────────────────────────────────────────────────────────────────────── */

typedef struct mock_provider {
    const char *name;
} mock_provider_t;

static sc_error_t mock_chat_with_system(void *ctx, sc_allocator_t *alloc, const char *system_prompt,
                                        size_t system_prompt_len, const char *message,
                                        size_t message_len, const char *model, size_t model_len,
                                        double temperature, char **out, size_t *out_len) {
    (void)ctx;
    (void)system_prompt;
    (void)system_prompt_len;
    (void)message;
    (void)message_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    const char *resp = "mock response";
    *out = sc_strndup(alloc, resp, strlen(resp));
    *out_len = *out ? strlen(resp) : 0;
    return *out ? SC_OK : SC_ERR_OUT_OF_MEMORY;
}

static sc_error_t mock_chat(void *ctx, sc_allocator_t *alloc, const sc_chat_request_t *request,
                            const char *model, size_t model_len, double temperature,
                            sc_chat_response_t *out) {
    (void)ctx;
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    const char *resp = "mock response";
    out->content = sc_strndup(alloc, resp, strlen(resp));
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
    return out->content ? SC_OK : SC_ERR_OUT_OF_MEMORY;
}

static bool mock_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}
static const char *mock_get_name(void *ctx) {
    return ((mock_provider_t *)ctx)->name;
}
static void mock_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const sc_provider_vtable_t mock_provider_vtable = {
    .chat_with_system = mock_chat_with_system,
    .chat = mock_chat,
    .supports_native_tools = mock_supports_native_tools,
    .get_name = mock_get_name,
    .deinit = mock_deinit,
};

static sc_provider_t mock_provider_create(sc_allocator_t *alloc, mock_provider_t *ctx) {
    (void)alloc;
    ctx->name = "mock";
    return (sc_provider_t){.ctx = ctx, .vtable = &mock_provider_vtable};
}

/* ─────────────────────────────────────────────────────────────────────────
 * Mock tool
 * ───────────────────────────────────────────────────────────────────────── */
typedef struct mock_tool {
    const char *name;
} mock_tool_t;

static sc_error_t mock_tool_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                                    sc_tool_result_t *out) {
    (void)ctx;
    (void)alloc;
    (void)args;
    *out = sc_tool_result_ok("mock tool output", 16);
    return SC_OK;
}
static const char *mock_tool_name(void *ctx) {
    return ((mock_tool_t *)ctx)->name;
}
static const char *mock_tool_desc(void *ctx) {
    (void)ctx;
    return "A mock tool";
}
static const char *mock_tool_params(void *ctx) {
    (void)ctx;
    return "{}";
}
static void mock_tool_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const sc_tool_vtable_t mock_tool_vtable = {
    .execute = mock_tool_execute,
    .name = mock_tool_name,
    .description = mock_tool_desc,
    .parameters_json = mock_tool_params,
    .deinit = mock_tool_deinit,
};

/* ─────────────────────────────────────────────────────────────────────────
 * Tests
 * ───────────────────────────────────────────────────────────────────────── */

static void test_agent_from_config_basic(void) {
    sc_allocator_t alloc = sc_system_allocator();
    mock_provider_t mock_ctx;
    sc_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    sc_error_t err =
        sc_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);

    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(agent.model_name);
    SC_ASSERT_STR_EQ(agent.model_name, "gpt-4o");
    SC_ASSERT_NOT_NULL(agent.default_provider);
    SC_ASSERT_EQ(agent.history_count, 0);

    sc_agent_deinit(&agent);
}

static void test_agent_from_config_null_alloc(void) {
    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    mock_provider_t mock_ctx;
    sc_provider_t prov = mock_provider_create(NULL, &mock_ctx);
    sc_error_t err =
        sc_agent_from_config(&agent, NULL, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_agent_from_config_null_provider(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    sc_provider_t prov = {0};
    sc_error_t err =
        sc_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_agent_turn_simple(void) {
    sc_allocator_t alloc = sc_system_allocator();
    mock_provider_t mock_ctx;
    sc_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    sc_error_t err =
        sc_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    SC_ASSERT_EQ(err, SC_OK);

    char *response = NULL;
    size_t response_len = 0;
    err = sc_agent_turn(&agent, "hello", 5, &response, &response_len);

    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(response);
    SC_ASSERT_STR_EQ(response, "mock response");
    SC_ASSERT_EQ(response_len, strlen("mock response"));

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    sc_agent_deinit(&agent);
}

static void test_agent_slash_help(void) {
    sc_allocator_t alloc = sc_system_allocator();
    mock_provider_t mock_ctx;
    sc_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    sc_error_t err =
        sc_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    SC_ASSERT_EQ(err, SC_OK);

    char *response = NULL;
    size_t response_len = 0;
    err = sc_agent_turn(&agent, "/help", 5, &response, &response_len);

    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(response);
    SC_ASSERT_TRUE(strstr(response, "Commands:") != NULL);
    SC_ASSERT_TRUE(strstr(response, "/clear") != NULL);

    if (response) {
        alloc.free(alloc.ctx, response, strlen(response) + 1);
    }
    sc_agent_deinit(&agent);
}

static void test_agent_slash_clear(void) {
    sc_allocator_t alloc = sc_system_allocator();
    mock_provider_t mock_ctx;
    sc_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    sc_error_t err =
        sc_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    SC_ASSERT_EQ(err, SC_OK);

    /* Do a turn to add history */
    char *r1 = NULL;
    size_t r1_len = 0;
    err = sc_agent_turn(&agent, "hi", 2, &r1, &r1_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(agent.history_count, 2); /* user + assistant */
    if (r1)
        alloc.free(alloc.ctx, r1, r1_len + 1);

    /* Send /clear */
    char *r2 = NULL;
    size_t r2_len = 0;
    err = sc_agent_turn(&agent, "/clear", 6, &r2, &r2_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(r2, "History cleared.");
    SC_ASSERT_EQ(agent.history_count, 0);

    if (r2)
        alloc.free(alloc.ctx, r2, strlen(r2) + 1);
    sc_agent_deinit(&agent);
}

static void test_agent_sessions_command(void) {
    sc_allocator_t alloc = sc_system_allocator();
    mock_provider_t mock_ctx;
    sc_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    sc_error_t err =
        sc_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    SC_ASSERT_EQ(err, SC_OK);

    char *response = NULL;
    size_t response_len = 0;
    err = sc_agent_turn(&agent, "/sessions", 9, &response, &response_len);

    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(response);
    SC_ASSERT_TRUE(strstr(response, "Active sessions") != NULL);

    if (response)
        alloc.free(alloc.ctx, response, strlen(response) + 1);
    sc_agent_deinit(&agent);
}

static void test_agent_kill_command(void) {
    sc_allocator_t alloc = sc_system_allocator();
    mock_provider_t mock_ctx;
    sc_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    sc_error_t err =
        sc_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    SC_ASSERT_EQ(err, SC_OK);

    /* Do a turn to add history */
    char *r1 = NULL;
    size_t r1_len = 0;
    err = sc_agent_turn(&agent, "hi", 2, &r1, &r1_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(agent.history_count, 2);
    if (r1)
        alloc.free(alloc.ctx, r1, r1_len + 1);

    /* Send /kill */
    char *r2 = NULL;
    size_t r2_len = 0;
    err = sc_agent_turn(&agent, "/kill", 5, &r2, &r2_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(strstr(r2, "Session killed") != NULL);
    SC_ASSERT_EQ(agent.history_count, 0);

    if (r2)
        alloc.free(alloc.ctx, r2, strlen(r2) + 1);
    sc_agent_deinit(&agent);
}

static void test_agent_retry_command(void) {
    sc_allocator_t alloc = sc_system_allocator();
    mock_provider_t mock_ctx;
    sc_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    sc_error_t err =
        sc_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    SC_ASSERT_EQ(err, SC_OK);

    /* Do a turn to add history */
    char *r1 = NULL;
    size_t r1_len = 0;
    err = sc_agent_turn(&agent, "hello", 5, &r1, &r1_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(agent.history_count, 2);
    if (r1)
        alloc.free(alloc.ctx, r1, r1_len + 1);

    /* Send /retry */
    char *r2 = NULL;
    size_t r2_len = 0;
    err = sc_agent_turn(&agent, "/retry", 6, &r2, &r2_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(strstr(r2, "Last response removed") != NULL);
    SC_ASSERT_EQ(agent.history_count, 1);

    if (r2)
        alloc.free(alloc.ctx, r2, strlen(r2) + 1);
    sc_agent_deinit(&agent);
}

static void test_agent_slash_model(void) {
    sc_allocator_t alloc = sc_system_allocator();
    mock_provider_t mock_ctx;
    sc_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    sc_error_t err =
        sc_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(agent.model_name, "gpt-4o");

    /* Change model via /model */
    char *response = NULL;
    size_t response_len = 0;
    err = sc_agent_turn(&agent, "/model claude-3", 15, &response, &response_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(agent.model_name, "claude-3");
    SC_ASSERT_NOT_NULL(response);
    SC_ASSERT_TRUE(strstr(response, "claude-3") != NULL);

    if (response)
        alloc.free(alloc.ctx, response, strlen(response) + 1);
    sc_agent_deinit(&agent);
}

static void test_agent_slash_status(void) {
    sc_allocator_t alloc = sc_system_allocator();
    mock_provider_t mock_ctx;
    sc_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    sc_error_t err =
        sc_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    SC_ASSERT_EQ(err, SC_OK);

    char *response = NULL;
    size_t response_len = 0;
    err = sc_agent_turn(&agent, "/status", 7, &response, &response_len);

    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(response);
    SC_ASSERT_TRUE(strstr(response, "mock") != NULL);
    SC_ASSERT_TRUE(strstr(response, "gpt-4o") != NULL);

    if (response)
        alloc.free(alloc.ctx, response, strlen(response) + 1);
    sc_agent_deinit(&agent);
}

static void test_config_defaults(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg;

    /* Use a HOME that has no .seaclaw/config.json to get pure defaults */
    const char *h = getenv("HOME");
    char *old_home = h ? strdup(h) : NULL;
    setenv("HOME", "/tmp/seaclaw_test_noconfig_xyz", 1);

    sc_error_t err = sc_config_load(&backing, &cfg);

    if (old_home) {
        setenv("HOME", old_home, 1);
        free(old_home);
    } else {
        unsetenv("HOME");
    }

    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(cfg.default_provider);
    SC_ASSERT_STR_EQ(cfg.default_provider, "gemini");
    SC_ASSERT_FLOAT_EQ(cfg.default_temperature, 0.7, 0.001);
    SC_ASSERT_NOT_NULL(cfg.gateway_host);
    SC_ASSERT_EQ(cfg.gateway_port, 3000);

    sc_config_deinit(&cfg);
}

static void test_json_parse_malformed(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    sc_error_t err;

    /* Empty string */
    err = sc_json_parse(&alloc, "", 0, &val);
    SC_ASSERT_NEQ(err, SC_OK);
    SC_ASSERT_NULL(val);

    /* Single brace */
    val = NULL;
    err = sc_json_parse(&alloc, "{", 1, &val);
    SC_ASSERT_NEQ(err, SC_OK);

    /* Unclosed string */
    val = NULL;
    err = sc_json_parse(&alloc, "\"hello", 6, &val);
    SC_ASSERT_NEQ(err, SC_OK);

    /* Invalid token */
    val = NULL;
    err = sc_json_parse(&alloc, "undefined", 9, &val);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_json_parse_null_input(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;

    sc_error_t err = sc_json_parse(&alloc, NULL, 0, &val);

    SC_ASSERT_NEQ(err, SC_OK);
    SC_ASSERT_NULL(val);
}

static void test_tool_result_ownership(void) {
    sc_allocator_t alloc = sc_system_allocator();

    /* Not owned — static/borrowed */
    sc_tool_result_t r_ok = sc_tool_result_ok("static output", 13);
    SC_ASSERT_TRUE(r_ok.success);
    SC_ASSERT_FALSE(r_ok.output_owned);
    SC_ASSERT_FALSE(r_ok.error_msg_owned);

    /* Owned — caller must free */
    char *owned_buf = sc_strndup(&alloc, "owned output", 12);
    SC_ASSERT_NOT_NULL(owned_buf);
    sc_tool_result_t r_owned = sc_tool_result_ok_owned(owned_buf, 12);
    SC_ASSERT_TRUE(r_owned.success);
    SC_ASSERT_TRUE(r_owned.output_owned);
    SC_ASSERT_FALSE(r_owned.error_msg_owned);

    sc_tool_result_free(&alloc, &r_owned);
}

static void test_agent_with_tool(void) {
    sc_allocator_t alloc = sc_system_allocator();
    mock_provider_t mock_ctx;
    mock_tool_t tool_ctx = {.name = "mock_tool"};
    sc_provider_t prov = mock_provider_create(&alloc, &mock_ctx);
    sc_tool_t tool = {.ctx = &tool_ctx, .vtable = &mock_tool_vtable};

    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    sc_error_t err =
        sc_agent_from_config(&agent, &alloc, prov, &tool, 1, NULL, NULL, NULL, NULL, "gpt-4", 5,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(agent.tools_count, 1u);

    char *response = NULL;
    size_t response_len = 0;
    err = sc_agent_turn(&agent, "hi", 2, &response, &response_len);
    SC_ASSERT_EQ(err, SC_OK);
    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    sc_agent_deinit(&agent);
}

static void test_agent_multi_turn(void) {
    sc_allocator_t alloc = sc_system_allocator();
    mock_provider_t mock_ctx;
    sc_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    sc_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4", 5,
                         "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);

    char *r1 = NULL, *r2 = NULL;
    size_t l1 = 0, l2 = 0;
    sc_agent_turn(&agent, "first", 5, &r1, &l1);
    sc_agent_turn(&agent, "second", 6, &r2, &l2);

    SC_ASSERT_EQ(agent.history_count, 4u);
    if (r1)
        alloc.free(alloc.ctx, r1, l1 + 1);
    if (r2)
        alloc.free(alloc.ctx, r2, l2 + 1);
    sc_agent_deinit(&agent);
}

static void test_config_load_then_parse(void) {
    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    setenv("HOME", "/tmp/seaclaw_e2e_config_test", 1);

    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg = {0};
    sc_error_t err = sc_config_load(&backing, &cfg);
    SC_ASSERT_EQ(err, SC_OK);

    err = sc_config_parse_json(&cfg, "{\"default_model\":\"claude-3-haiku\"}", 35);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(cfg.default_model, "claude-3-haiku");

    sc_config_deinit(&cfg);
    if (old_home) {
        setenv("HOME", old_home, 1);
        free(old_home);
    } else
        unsetenv("HOME");
}

static void test_tool_result_fail(void) {
    sc_tool_result_t r = sc_tool_result_fail("error message", 12);
    SC_ASSERT_FALSE(r.success);
    SC_ASSERT_STR_EQ(r.error_msg, "error message");
}

static void test_health_mark_ok(void) {
    sc_health_mark_ok("test_component");
}

static void test_health_mark_error(void) {
    sc_health_mark_error("test_component", "test error");
}

static void test_agent_custom_instructions(void) {
    sc_allocator_t alloc = sc_system_allocator();
    mock_provider_t mock_ctx;
    sc_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    sc_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4", 5,
                         "openai", 6, 0.7, ".", 1, 25, 50, false, 0, "You are helpful", 15, NULL, 0,
                         NULL);

    SC_ASSERT_NOT_NULL(agent.custom_instructions);
    sc_agent_deinit(&agent);
}

static void test_agent_from_config_max_iterations(void) {
    sc_allocator_t alloc = sc_system_allocator();
    mock_provider_t mock_ctx;
    sc_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    sc_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4", 5,
                         "openai", 6, 0.7, ".", 1, 10, 20, false, 0, NULL, 0, NULL, 0, NULL);

    SC_ASSERT_EQ(agent.max_tool_iterations, 10u);
    SC_ASSERT_EQ(agent.max_history_messages, 20u);
    sc_agent_deinit(&agent);
}

static void test_agent_deinit_cleans(void) {
    sc_allocator_t alloc = sc_system_allocator();
    mock_provider_t mock_ctx;
    sc_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    sc_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4", 5,
                         "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);

    sc_agent_deinit(&agent);
    SC_ASSERT_NULL(agent.model_name);
}

static void test_agent_turn_empty_message(void) {
    sc_allocator_t alloc = sc_system_allocator();
    mock_provider_t mock_ctx;
    sc_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    sc_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4", 5,
                         "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);

    char *r = NULL;
    size_t len = 0;
    sc_error_t err = sc_agent_turn(&agent, "", 0, &r, &len);
    SC_ASSERT_EQ(err, SC_OK);
    if (r)
        alloc.free(alloc.ctx, r, len + 1);
    sc_agent_deinit(&agent);
}

static void test_agent_with_observer(void) {
    sc_allocator_t alloc = sc_system_allocator();
    mock_provider_t mock_ctx;
    sc_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    FILE *f = tmpfile();
    SC_ASSERT_NOT_NULL(f);
    sc_observer_t obs = sc_log_observer_create(&alloc, f);

    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    sc_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, &obs, NULL, "gpt-4", 5,
                         "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);

    char *r = NULL;
    size_t len = 0;
    sc_agent_turn(&agent, "hi", 2, &r, &len);
    if (r)
        alloc.free(alloc.ctx, r, len + 1);

    sc_agent_deinit(&agent);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
    fclose(f);
}

static void test_provider_create_from_config(void) {
    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    setenv("HOME", "/tmp/seaclaw_e2e_provider", 1);

    sc_allocator_t alloc = sc_system_allocator();
    sc_config_t cfg = {0};
    sc_config_load(&alloc, &cfg);

    SC_ASSERT_NOT_NULL(cfg.default_provider);

    sc_config_deinit(&cfg);
    if (old_home) {
        setenv("HOME", old_home, 1);
        free(old_home);
    } else
        unsetenv("HOME");
}

static void test_config_validate_after_load(void) {
    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    setenv("HOME", "/tmp/seaclaw_e2e_validate", 1);

    sc_allocator_t alloc = sc_system_allocator();
    sc_config_t cfg = {0};
    sc_config_load(&alloc, &cfg);
    sc_error_t err = sc_config_validate(&cfg);

    SC_ASSERT_EQ(err, SC_OK);
    sc_config_deinit(&cfg);
    if (old_home) {
        setenv("HOME", old_home, 1);
        free(old_home);
    } else
        unsetenv("HOME");
}

/* ─────────────────────────────────────────────────────────────────────────
 * Tool-call round-trip: mock provider returns a tool call on first chat(),
 * agent executes it, feeds the result back, provider returns final text.
 * ───────────────────────────────────────────────────────────────────────── */

typedef struct tc_mock_provider {
    const char *name;
    int call_count;
} tc_mock_provider_t;

static sc_error_t tc_mock_chat(void *ctx, sc_allocator_t *alloc, const sc_chat_request_t *request,
                               const char *model, size_t model_len, double temperature,
                               sc_chat_response_t *out) {
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    tc_mock_provider_t *m = (tc_mock_provider_t *)ctx;
    memset(out, 0, sizeof(*out));

    if (m->call_count == 0) {
        /* First call: return a tool call instead of text */
        m->call_count++;
        sc_tool_call_t *tc = (sc_tool_call_t *)alloc->alloc(alloc->ctx, sizeof(sc_tool_call_t));
        if (!tc)
            return SC_ERR_OUT_OF_MEMORY;
        memset(tc, 0, sizeof(*tc));
        tc->id = sc_strndup(alloc, "call_1", 6);
        tc->id_len = 6;
        tc->name = sc_strndup(alloc, "mock_tool", 9);
        tc->name_len = 9;
        tc->arguments = sc_strndup(alloc, "{}", 2);
        tc->arguments_len = 2;
        out->tool_calls = tc;
        out->tool_calls_count = 1;
        out->content = NULL;
        out->content_len = 0;
        return SC_OK;
    }
    /* Subsequent calls: return final text */
    m->call_count++;
    const char *resp = "tool call done";
    out->content = sc_strndup(alloc, resp, strlen(resp));
    out->content_len = out->content ? strlen(resp) : 0;
    out->tool_calls = NULL;
    out->tool_calls_count = 0;
    return out->content ? SC_OK : SC_ERR_OUT_OF_MEMORY;
}

static sc_error_t tc_mock_chat_with_system(void *ctx, sc_allocator_t *alloc,
                                           const char *system_prompt, size_t system_prompt_len,
                                           const char *message, size_t message_len,
                                           const char *model, size_t model_len, double temperature,
                                           char **out, size_t *out_len) {
    (void)ctx;
    (void)system_prompt;
    (void)system_prompt_len;
    (void)message;
    (void)message_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    const char *resp = "tool call done";
    *out = sc_strndup(alloc, resp, strlen(resp));
    *out_len = *out ? strlen(resp) : 0;
    return *out ? SC_OK : SC_ERR_OUT_OF_MEMORY;
}

static bool tc_mock_supports_native_tools(void *ctx) {
    (void)ctx;
    return true;
}
static const char *tc_mock_get_name(void *ctx) {
    return ((tc_mock_provider_t *)ctx)->name;
}
static void tc_mock_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const sc_provider_vtable_t tc_mock_vtable = {
    .chat_with_system = tc_mock_chat_with_system,
    .chat = tc_mock_chat,
    .supports_native_tools = tc_mock_supports_native_tools,
    .get_name = tc_mock_get_name,
    .deinit = tc_mock_deinit,
};

static void test_agent_tool_call_round_trip(void) {
    sc_allocator_t alloc = sc_system_allocator();
    tc_mock_provider_t mock_ctx = {.name = "tc_mock", .call_count = 0};
    sc_provider_t prov = {.ctx = &mock_ctx, .vtable = &tc_mock_vtable};

    mock_tool_t tool_ctx = {.name = "mock_tool"};
    sc_tool_t tool = {.ctx = &tool_ctx, .vtable = &mock_tool_vtable};

    sc_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    sc_error_t err =
        sc_agent_from_config(&agent, &alloc, prov, &tool, 1, NULL, NULL, NULL, NULL, "gpt-4", 5,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(agent.tools_count, 1u);

    char *response = NULL;
    size_t response_len = 0;
    err = sc_agent_turn(&agent, "call the tool", 13, &response, &response_len);

    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(response);
    SC_ASSERT_STR_EQ(response, "tool call done");
    SC_ASSERT_EQ(mock_ctx.call_count, 2);
    SC_ASSERT_TRUE(agent.history_count >= 4);

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    sc_agent_deinit(&agent);
}

/* ── Outcome Tracker ─────────────────────────────────────────────────── */

#include "seaclaw/agent/outcomes.h"

static void test_outcome_tracker_init(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);
    SC_ASSERT_EQ(tracker.total, 0u);
    SC_ASSERT_EQ(tracker.tool_successes, 0u);
    SC_ASSERT_EQ(tracker.tool_failures, 0u);
    SC_ASSERT_FALSE(tracker.auto_apply_feedback);
}

static void test_outcome_record_tool_success(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);
    sc_outcome_record_tool(&tracker, "shell", true, "ran ls");
    SC_ASSERT_EQ(tracker.total, 1u);
    SC_ASSERT_EQ(tracker.tool_successes, 1u);
}

static void test_outcome_record_tool_failure(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);
    sc_outcome_record_tool(&tracker, "shell", false, "permission denied");
    SC_ASSERT_EQ(tracker.tool_failures, 1u);
}

static void test_outcome_record_correction(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);
    sc_outcome_record_correction(&tracker, "original", "corrected");
    SC_ASSERT_EQ(tracker.corrections, 1u);
}

static void test_outcome_record_positive(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);
    sc_outcome_record_positive(&tracker, "thanks");
    SC_ASSERT_EQ(tracker.positives, 1u);
}

static void test_outcome_get_recent(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);
    sc_outcome_record_tool(&tracker, "shell", true, "ok");
    size_t count = 0;
    const sc_outcome_entry_t *entries = sc_outcome_get_recent(&tracker, &count);
    SC_ASSERT_NOT_NULL(entries);
    SC_ASSERT_EQ(count, 1u);
}

static void test_outcome_build_summary(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);
    sc_outcome_record_tool(&tracker, "shell", true, "ok");
    size_t len = 0;
    char *summary = sc_outcome_build_summary(&tracker, &alloc, &len);
    SC_ASSERT_NOT_NULL(summary);
    SC_ASSERT_TRUE(len > 0);
    alloc.free(alloc.ctx, summary, len + 1);
}

static void test_outcome_detect_repeated_failure(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);
    SC_ASSERT_FALSE(sc_outcome_detect_repeated_failure(&tracker, "shell", 3));
    sc_outcome_record_tool(&tracker, "shell", false, "fail1");
    sc_outcome_record_tool(&tracker, "shell", false, "fail2");
    sc_outcome_record_tool(&tracker, "shell", false, "fail3");
    SC_ASSERT_TRUE(sc_outcome_detect_repeated_failure(&tracker, "shell", 3));
}

void run_e2e_tests(void) {
    SC_TEST_SUITE("E2E");
    SC_RUN_TEST(test_agent_from_config_basic);
    SC_RUN_TEST(test_agent_from_config_null_alloc);
    SC_RUN_TEST(test_agent_from_config_null_provider);
    SC_RUN_TEST(test_agent_turn_simple);
    SC_RUN_TEST(test_agent_slash_help);
    SC_RUN_TEST(test_agent_slash_clear);
    SC_RUN_TEST(test_agent_sessions_command);
    SC_RUN_TEST(test_agent_kill_command);
    SC_RUN_TEST(test_agent_retry_command);
    SC_RUN_TEST(test_agent_slash_model);
    SC_RUN_TEST(test_agent_slash_status);
    SC_RUN_TEST(test_config_defaults);
    SC_RUN_TEST(test_json_parse_malformed);
    SC_RUN_TEST(test_json_parse_null_input);
    SC_RUN_TEST(test_tool_result_ownership);

    SC_RUN_TEST(test_agent_with_tool);
    SC_RUN_TEST(test_agent_multi_turn);
    SC_RUN_TEST(test_config_load_then_parse);
    SC_RUN_TEST(test_tool_result_fail);
    SC_RUN_TEST(test_health_mark_ok);
    SC_RUN_TEST(test_health_mark_error);
    SC_RUN_TEST(test_agent_custom_instructions);
    SC_RUN_TEST(test_agent_from_config_max_iterations);
    SC_RUN_TEST(test_agent_deinit_cleans);
    SC_RUN_TEST(test_agent_turn_empty_message);
    SC_RUN_TEST(test_agent_with_observer);
    SC_RUN_TEST(test_provider_create_from_config);
    SC_RUN_TEST(test_config_validate_after_load);
    SC_RUN_TEST(test_agent_tool_call_round_trip);

    SC_RUN_TEST(test_outcome_tracker_init);
    SC_RUN_TEST(test_outcome_record_tool_success);
    SC_RUN_TEST(test_outcome_record_tool_failure);
    SC_RUN_TEST(test_outcome_record_correction);
    SC_RUN_TEST(test_outcome_record_positive);
    SC_RUN_TEST(test_outcome_get_recent);
    SC_RUN_TEST(test_outcome_build_summary);
    SC_RUN_TEST(test_outcome_detect_repeated_failure);
}
