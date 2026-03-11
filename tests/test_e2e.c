#include "human/agent.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/health.h"
#include "human/memory.h"
#include "human/observability/log_observer.h"
#include "human/provider.h"
#include "human/tool.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef HU_HAS_PERSONA
#include "human/agent/collab_planning.h"
#include "human/context/authentic.h"
#include "human/context/behavioral.h"
#include "human/context/intelligence.h"
#include "human/context/rel_dynamics.h"
#endif

/* ─────────────────────────────────────────────────────────────────────────
 * Mock provider — returns fixed "mock response" from chat()
 * ───────────────────────────────────────────────────────────────────────── */

typedef struct mock_provider {
    const char *name;
} mock_provider_t;

static hu_error_t mock_chat_with_system(void *ctx, hu_allocator_t *alloc, const char *system_prompt,
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
    *out = hu_strndup(alloc, resp, strlen(resp));
    *out_len = *out ? strlen(resp) : 0;
    return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static hu_error_t mock_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                            const char *model, size_t model_len, double temperature,
                            hu_chat_response_t *out) {
    (void)ctx;
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    const char *resp = "mock response";
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

static bool mock_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}
static const char *mock_get_name(void *ctx) {
    return ((mock_provider_t *)ctx)->name;
}
static void mock_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const hu_provider_vtable_t mock_provider_vtable = {
    .chat_with_system = mock_chat_with_system,
    .chat = mock_chat,
    .supports_native_tools = mock_supports_native_tools,
    .get_name = mock_get_name,
    .deinit = mock_deinit,
};

static hu_provider_t mock_provider_create(hu_allocator_t *alloc, mock_provider_t *ctx) {
    (void)alloc;
    ctx->name = "mock";
    return (hu_provider_t){.ctx = ctx, .vtable = &mock_provider_vtable};
}

/* ─────────────────────────────────────────────────────────────────────────
 * Mock tool
 * ───────────────────────────────────────────────────────────────────────── */
typedef struct mock_tool {
    const char *name;
} mock_tool_t;

static hu_error_t mock_tool_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                    hu_tool_result_t *out) {
    (void)ctx;
    (void)alloc;
    (void)args;
    *out = hu_tool_result_ok("mock tool output", 16);
    return HU_OK;
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
static void mock_tool_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const hu_tool_vtable_t mock_tool_vtable = {
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
    hu_allocator_t alloc = hu_system_allocator();
    mock_provider_t mock_ctx;
    hu_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err =
        hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(agent.model_name);
    HU_ASSERT_STR_EQ(agent.model_name, "gpt-4o");
    HU_ASSERT_NOT_NULL(agent.default_provider);
    HU_ASSERT_EQ(agent.history_count, 0);

    hu_agent_deinit(&agent);
}

static void test_agent_from_config_null_alloc(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    mock_provider_t mock_ctx;
    hu_provider_t prov = mock_provider_create(NULL, &mock_ctx);
    hu_error_t err =
        hu_agent_from_config(&agent, NULL, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_agent_from_config_null_provider(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_provider_t prov = {0};
    hu_error_t err =
        hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_agent_turn_simple(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_provider_t mock_ctx;
    hu_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err =
        hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn(&agent, "hello", 5, &response, &response_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(response);
    HU_ASSERT_STR_EQ(response, "mock response");
    HU_ASSERT_EQ(response_len, strlen("mock response"));

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    hu_agent_deinit(&agent);
}

static void test_agent_slash_help(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_provider_t mock_ctx;
    hu_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err =
        hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn(&agent, "/help", 5, &response, &response_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(response);
    HU_ASSERT_TRUE(strstr(response, "Commands:") != NULL);
    HU_ASSERT_TRUE(strstr(response, "/clear") != NULL);

    if (response) {
        alloc.free(alloc.ctx, response, strlen(response) + 1);
    }
    hu_agent_deinit(&agent);
}

static void test_agent_slash_clear(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_provider_t mock_ctx;
    hu_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err =
        hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    /* Do a turn to add history */
    char *r1 = NULL;
    size_t r1_len = 0;
    err = hu_agent_turn(&agent, "hi", 2, &r1, &r1_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(agent.history_count, 2); /* user + assistant */
    if (r1)
        alloc.free(alloc.ctx, r1, r1_len + 1);

    /* Send /clear */
    char *r2 = NULL;
    size_t r2_len = 0;
    err = hu_agent_turn(&agent, "/clear", 6, &r2, &r2_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(r2, "History cleared.");
    HU_ASSERT_EQ(agent.history_count, 0);

    if (r2)
        alloc.free(alloc.ctx, r2, strlen(r2) + 1);
    hu_agent_deinit(&agent);
}

static void test_agent_sessions_command(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_provider_t mock_ctx;
    hu_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err =
        hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn(&agent, "/sessions", 9, &response, &response_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(response);
    HU_ASSERT_TRUE(strstr(response, "Active sessions") != NULL);

    if (response)
        alloc.free(alloc.ctx, response, strlen(response) + 1);
    hu_agent_deinit(&agent);
}

static void test_agent_kill_command(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_provider_t mock_ctx;
    hu_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err =
        hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    /* Do a turn to add history */
    char *r1 = NULL;
    size_t r1_len = 0;
    err = hu_agent_turn(&agent, "hi", 2, &r1, &r1_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(agent.history_count, 2);
    if (r1)
        alloc.free(alloc.ctx, r1, r1_len + 1);

    /* Send /kill */
    char *r2 = NULL;
    size_t r2_len = 0;
    err = hu_agent_turn(&agent, "/kill", 5, &r2, &r2_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(r2, "Session killed") != NULL);
    HU_ASSERT_EQ(agent.history_count, 0);

    if (r2)
        alloc.free(alloc.ctx, r2, strlen(r2) + 1);
    hu_agent_deinit(&agent);
}

static void test_agent_retry_command(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_provider_t mock_ctx;
    hu_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err =
        hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    /* Do a turn to add history */
    char *r1 = NULL;
    size_t r1_len = 0;
    err = hu_agent_turn(&agent, "hello", 5, &r1, &r1_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(agent.history_count, 2);
    if (r1)
        alloc.free(alloc.ctx, r1, r1_len + 1);

    /* Send /retry */
    char *r2 = NULL;
    size_t r2_len = 0;
    err = hu_agent_turn(&agent, "/retry", 6, &r2, &r2_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(r2, "Last response removed") != NULL);
    HU_ASSERT_EQ(agent.history_count, 1);

    if (r2)
        alloc.free(alloc.ctx, r2, strlen(r2) + 1);
    hu_agent_deinit(&agent);
}

static void test_agent_slash_model(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_provider_t mock_ctx;
    hu_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err =
        hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(agent.model_name, "gpt-4o");

    /* Change model via /model */
    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn(&agent, "/model claude-3", 15, &response, &response_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(agent.model_name, "claude-3");
    HU_ASSERT_NOT_NULL(response);
    HU_ASSERT_TRUE(strstr(response, "claude-3") != NULL);

    if (response)
        alloc.free(alloc.ctx, response, strlen(response) + 1);
    hu_agent_deinit(&agent);
}

static void test_agent_slash_status(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_provider_t mock_ctx;
    hu_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err =
        hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn(&agent, "/status", 7, &response, &response_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(response);
    HU_ASSERT_TRUE(strstr(response, "mock") != NULL);
    HU_ASSERT_TRUE(strstr(response, "gpt-4o") != NULL);

    if (response)
        alloc.free(alloc.ctx, response, strlen(response) + 1);
    hu_agent_deinit(&agent);
}

static void test_config_defaults(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;

    /* Use a HOME that has no .human/config.json to get pure defaults */
    const char *h = getenv("HOME");
    char *old_home = h ? strdup(h) : NULL;
    setenv("HOME", "/tmp/human_test_noconfig_xyz", 1);

    hu_error_t err = hu_config_load(&backing, &cfg);

    if (old_home) {
        setenv("HOME", old_home, 1);
        free(old_home);
    } else {
        unsetenv("HOME");
    }

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(cfg.default_provider);
    HU_ASSERT_STR_EQ(cfg.default_provider, "gemini");
    HU_ASSERT_FLOAT_EQ(cfg.default_temperature, 0.7, 0.001);
    HU_ASSERT_NOT_NULL(cfg.gateway_host);
    HU_ASSERT_EQ(cfg.gateway_port, 3000);

    hu_config_deinit(&cfg);
}

static void test_json_parse_malformed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    hu_error_t err;

    /* Empty string */
    err = hu_json_parse(&alloc, "", 0, &val);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(val);

    /* Single brace */
    val = NULL;
    err = hu_json_parse(&alloc, "{", 1, &val);
    HU_ASSERT_NEQ(err, HU_OK);

    /* Unclosed string */
    val = NULL;
    err = hu_json_parse(&alloc, "\"hello", 6, &val);
    HU_ASSERT_NEQ(err, HU_OK);

    /* Invalid token */
    val = NULL;
    err = hu_json_parse(&alloc, "undefined", 9, &val);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_json_parse_null_input(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;

    hu_error_t err = hu_json_parse(&alloc, NULL, 0, &val);

    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(val);
}

static void test_tool_result_ownership(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Not owned — static/borrowed */
    hu_tool_result_t r_ok = hu_tool_result_ok("static output", 13);
    HU_ASSERT_TRUE(r_ok.success);
    HU_ASSERT_FALSE(r_ok.output_owned);
    HU_ASSERT_FALSE(r_ok.error_msg_owned);

    /* Owned — caller must free */
    char *owned_buf = hu_strndup(&alloc, "owned output", 12);
    HU_ASSERT_NOT_NULL(owned_buf);
    hu_tool_result_t r_owned = hu_tool_result_ok_owned(owned_buf, 12);
    HU_ASSERT_TRUE(r_owned.success);
    HU_ASSERT_TRUE(r_owned.output_owned);
    HU_ASSERT_FALSE(r_owned.error_msg_owned);

    hu_tool_result_free(&alloc, &r_owned);
}

static void test_agent_with_tool(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_provider_t mock_ctx;
    mock_tool_t tool_ctx = {.name = "mock_tool"};
    hu_provider_t prov = mock_provider_create(&alloc, &mock_ctx);
    hu_tool_t tool = {.ctx = &tool_ctx, .vtable = &mock_tool_vtable};

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err =
        hu_agent_from_config(&agent, &alloc, prov, &tool, 1, NULL, NULL, NULL, NULL, "gpt-4", 5,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(agent.tools_count, 1u);

    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn(&agent, "hi", 2, &response, &response_len);
    HU_ASSERT_EQ(err, HU_OK);
    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    hu_agent_deinit(&agent);
}

static void test_agent_multi_turn(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_provider_t mock_ctx;
    hu_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4", 5,
                         "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);

    char *r1 = NULL, *r2 = NULL;
    size_t l1 = 0, l2 = 0;
    hu_agent_turn(&agent, "first", 5, &r1, &l1);
    hu_agent_turn(&agent, "second", 6, &r2, &l2);

    HU_ASSERT_EQ(agent.history_count, 4u);
    if (r1)
        alloc.free(alloc.ctx, r1, l1 + 1);
    if (r2)
        alloc.free(alloc.ctx, r2, l2 + 1);
    hu_agent_deinit(&agent);
}

static void test_config_load_then_parse(void) {
    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    setenv("HOME", "/tmp/human_e2e_config_test", 1);

    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg = {0};
    hu_error_t err = hu_config_load(&backing, &cfg);
    HU_ASSERT_EQ(err, HU_OK);

    err = hu_config_parse_json(&cfg, "{\"default_model\":\"claude-3-haiku\"}", 35);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(cfg.default_model, "claude-3-haiku");

    hu_config_deinit(&cfg);
    if (old_home) {
        setenv("HOME", old_home, 1);
        free(old_home);
    } else
        unsetenv("HOME");
}

static void test_tool_result_fail(void) {
    hu_tool_result_t r = hu_tool_result_fail("error message", 12);
    HU_ASSERT_FALSE(r.success);
    HU_ASSERT_STR_EQ(r.error_msg, "error message");
}

static void test_health_mark_ok(void) {
    hu_health_mark_ok("test_component");
}

static void test_health_mark_error(void) {
    hu_health_mark_error("test_component", "test error");
}

static void test_agent_custom_instructions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_provider_t mock_ctx;
    hu_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4", 5,
                         "openai", 6, 0.7, ".", 1, 25, 50, false, 0, "You are helpful", 15, NULL, 0,
                         NULL);

    HU_ASSERT_NOT_NULL(agent.custom_instructions);
    hu_agent_deinit(&agent);
}

static void test_agent_from_config_max_iterations(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_provider_t mock_ctx;
    hu_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4", 5,
                         "openai", 6, 0.7, ".", 1, 10, 20, false, 0, NULL, 0, NULL, 0, NULL);

    HU_ASSERT_EQ(agent.max_tool_iterations, 10u);
    HU_ASSERT_EQ(agent.max_history_messages, 20u);
    hu_agent_deinit(&agent);
}

static void test_agent_deinit_cleans(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_provider_t mock_ctx;
    hu_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4", 5,
                         "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);

    hu_agent_deinit(&agent);
    HU_ASSERT_NULL(agent.model_name);
}

static void test_agent_turn_empty_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_provider_t mock_ctx;
    hu_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL, "gpt-4", 5,
                         "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);

    char *r = NULL;
    size_t len = 0;
    hu_error_t err = hu_agent_turn(&agent, "", 0, &r, &len);
    HU_ASSERT_EQ(err, HU_OK);
    if (r)
        alloc.free(alloc.ctx, r, len + 1);
    hu_agent_deinit(&agent);
}

static void test_agent_with_observer(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_provider_t mock_ctx;
    hu_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&alloc, f);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, &obs, NULL, "gpt-4", 5,
                         "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);

    char *r = NULL;
    size_t len = 0;
    hu_agent_turn(&agent, "hi", 2, &r, &len);
    if (r)
        alloc.free(alloc.ctx, r, len + 1);

    hu_agent_deinit(&agent);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
    fclose(f);
}

static void test_provider_create_from_config(void) {
    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    setenv("HOME", "/tmp/human_e2e_provider", 1);

    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    hu_config_load(&alloc, &cfg);

    HU_ASSERT_NOT_NULL(cfg.default_provider);

    hu_config_deinit(&cfg);
    if (old_home) {
        setenv("HOME", old_home, 1);
        free(old_home);
    } else
        unsetenv("HOME");
}

static void test_config_validate_after_load(void) {
    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    setenv("HOME", "/tmp/human_e2e_validate", 1);

    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    hu_config_load(&alloc, &cfg);
    hu_error_t err = hu_config_validate(&cfg);

    HU_ASSERT_EQ(err, HU_OK);
    hu_config_deinit(&cfg);
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

static hu_error_t tc_mock_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                               const char *model, size_t model_len, double temperature,
                               hu_chat_response_t *out) {
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    tc_mock_provider_t *m = (tc_mock_provider_t *)ctx;
    memset(out, 0, sizeof(*out));

    if (m->call_count == 0) {
        /* First call: return a tool call instead of text */
        m->call_count++;
        hu_tool_call_t *tc = (hu_tool_call_t *)alloc->alloc(alloc->ctx, sizeof(hu_tool_call_t));
        if (!tc)
            return HU_ERR_OUT_OF_MEMORY;
        memset(tc, 0, sizeof(*tc));
        tc->id = hu_strndup(alloc, "call_1", 6);
        tc->id_len = 6;
        tc->name = hu_strndup(alloc, "mock_tool", 9);
        tc->name_len = 9;
        tc->arguments = hu_strndup(alloc, "{}", 2);
        tc->arguments_len = 2;
        out->tool_calls = tc;
        out->tool_calls_count = 1;
        out->content = NULL;
        out->content_len = 0;
        return HU_OK;
    }
    /* Subsequent calls: return final text */
    m->call_count++;
    const char *resp = "tool call done";
    out->content = hu_strndup(alloc, resp, strlen(resp));
    out->content_len = out->content ? strlen(resp) : 0;
    out->tool_calls = NULL;
    out->tool_calls_count = 0;
    return out->content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static hu_error_t tc_mock_chat_with_system(void *ctx, hu_allocator_t *alloc,
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
    *out = hu_strndup(alloc, resp, strlen(resp));
    *out_len = *out ? strlen(resp) : 0;
    return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static bool tc_mock_supports_native_tools(void *ctx) {
    (void)ctx;
    return true;
}
static const char *tc_mock_get_name(void *ctx) {
    return ((tc_mock_provider_t *)ctx)->name;
}
static void tc_mock_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const hu_provider_vtable_t tc_mock_vtable = {
    .chat_with_system = tc_mock_chat_with_system,
    .chat = tc_mock_chat,
    .supports_native_tools = tc_mock_supports_native_tools,
    .get_name = tc_mock_get_name,
    .deinit = tc_mock_deinit,
};

static void test_agent_tool_call_round_trip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    tc_mock_provider_t mock_ctx = {.name = "tc_mock", .call_count = 0};
    hu_provider_t prov = {.ctx = &mock_ctx, .vtable = &tc_mock_vtable};

    mock_tool_t tool_ctx = {.name = "mock_tool"};
    hu_tool_t tool = {.ctx = &tool_ctx, .vtable = &mock_tool_vtable};

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err =
        hu_agent_from_config(&agent, &alloc, prov, &tool, 1, NULL, NULL, NULL, NULL, "gpt-4", 5,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(agent.tools_count, 1u);

    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn(&agent, "call the tool", 13, &response, &response_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(response);
    HU_ASSERT_STR_EQ(response, "tool call done");
    HU_ASSERT_EQ(mock_ctx.call_count, 2);
    HU_ASSERT_TRUE(agent.history_count >= 4);

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    hu_agent_deinit(&agent);
}

/* ── Outcome Tracker ─────────────────────────────────────────────────── */

#include "human/agent/outcomes.h"

static void test_outcome_tracker_init(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);
    HU_ASSERT_EQ(tracker.total, 0u);
    HU_ASSERT_EQ(tracker.tool_successes, 0u);
    HU_ASSERT_EQ(tracker.tool_failures, 0u);
    HU_ASSERT_FALSE(tracker.auto_apply_feedback);
}

static void test_outcome_record_tool_success(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);
    hu_outcome_record_tool(&tracker, "shell", true, "ran ls");
    HU_ASSERT_EQ(tracker.total, 1u);
    HU_ASSERT_EQ(tracker.tool_successes, 1u);
}

static void test_outcome_record_tool_failure(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);
    hu_outcome_record_tool(&tracker, "shell", false, "permission denied");
    HU_ASSERT_EQ(tracker.tool_failures, 1u);
}

static void test_outcome_record_correction(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);
    hu_outcome_record_correction(&tracker, "original", "corrected");
    HU_ASSERT_EQ(tracker.corrections, 1u);
}

static void test_outcome_record_positive(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);
    hu_outcome_record_positive(&tracker, "thanks");
    HU_ASSERT_EQ(tracker.positives, 1u);
}

static void test_outcome_get_recent(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);
    hu_outcome_record_tool(&tracker, "shell", true, "ok");
    size_t count = 0;
    const hu_outcome_entry_t *entries = hu_outcome_get_recent(&tracker, &count);
    HU_ASSERT_NOT_NULL(entries);
    HU_ASSERT_EQ(count, 1u);
}

static void test_outcome_build_summary(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);
    hu_outcome_record_tool(&tracker, "shell", true, "ok");
    size_t len = 0;
    char *summary = hu_outcome_build_summary(&tracker, &alloc, &len);
    HU_ASSERT_NOT_NULL(summary);
    HU_ASSERT_TRUE(len > 0);
    alloc.free(alloc.ctx, summary, len + 1);
}

static void test_outcome_detect_repeated_failure(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);
    HU_ASSERT_FALSE(hu_outcome_detect_repeated_failure(&tracker, "shell", 3));
    hu_outcome_record_tool(&tracker, "shell", false, "fail1");
    hu_outcome_record_tool(&tracker, "shell", false, "fail2");
    hu_outcome_record_tool(&tracker, "shell", false, "fail3");
    HU_ASSERT_TRUE(hu_outcome_detect_repeated_failure(&tracker, "shell", 3));
}

/* ── Multi-turn with SQLite memory (daemon module integrations) ───────────── */

#ifdef HU_ENABLE_SQLITE
static void test_agent_multi_turn_with_sqlite_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_provider_t mock_ctx;
    hu_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_error_t err =
        hu_agent_from_config(&agent, &alloc, prov, NULL, 0, &mem, NULL, NULL, NULL, "gpt-4o", 6,
                             "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    /* Turn 1: greeting */
    char *resp1 = NULL;
    size_t resp1_len = 0;
    err = hu_agent_turn(&agent, "hey, how are you?", 17, &resp1, &resp1_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp1);
    alloc.free(alloc.ctx, resp1, resp1_len + 1);

    /* Turn 2: follow-up */
    char *resp2 = NULL;
    size_t resp2_len = 0;
    err = hu_agent_turn(&agent, "what should we do this weekend?", 31, &resp2, &resp2_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp2);
    alloc.free(alloc.ctx, resp2, resp2_len + 1);

    /* Turn 3: emotional topic */
    char *resp3 = NULL;
    size_t resp3_len = 0;
    err = hu_agent_turn(&agent, "I've been feeling stressed about work lately", 44, &resp3,
                        &resp3_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp3);
    alloc.free(alloc.ctx, resp3, resp3_len + 1);

    hu_agent_deinit(&agent);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}
#endif

#ifdef HU_HAS_PERSONA
static void test_daemon_module_pipeline_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Phase 6: Authentic existence */
    hu_authentic_config_t auth_cfg = {
        .narration_probability = 0.10,
        .embodiment_probability = 0.08,
        .complaining_probability = 0.07,
    };
    hu_authentic_behavior_t behavior = hu_authentic_select(&auth_cfg, 0.5, false, 42);
    if (behavior != HU_AUTH_NONE) {
        char *dir = NULL;
        size_t dir_len = 0;
        hu_authentic_build_directive(&alloc, behavior, NULL, 0, &dir, &dir_len);
        if (dir)
            hu_str_free(&alloc, dir);
    }

    /* Phase 6: Cognitive load */
    double load = hu_cognitive_compute_load(3, 15, false);
    HU_ASSERT_TRUE(load >= 0.0 && load <= 1.0);
    if (load > 0.5) {
        hu_cognitive_state_t state = {
            .active_conversations = 3,
            .messages_this_hour = 15,
            .load_score = load,
        };
        char *dir = NULL;
        size_t dir_len = 0;
        hu_cognitive_build_directive(&alloc, &state, &dir, &dir_len);
        if (dir)
            hu_str_free(&alloc, dir);
    }

    /* Phase 6: Relationship dynamics */
    hu_rel_velocity_t vel = {
        .contact_id = "test_user",
        .contact_id_len = 9,
        .messages_sent_30d = 50,
        .messages_received_30d = 45,
        .initiations_sent_30d = 10,
        .initiations_received_30d = 8,
        .interaction_quality = 0.7f,
    };
    hu_rel_velocity_compute(&vel);
    hu_drift_signal_t drift = {
        .contact_id = "test_user",
        .contact_id_len = 9,
        .current_velocity = vel.velocity,
    };
    hu_repair_state_t repair = {0};
    char *rel_dir = NULL;
    size_t rel_len = 0;
    hu_rel_dynamics_build_prompt(&alloc, &vel, &drift, &repair, &rel_dir, &rel_len);
    if (rel_dir)
        hu_str_free(&alloc, rel_dir);

    /* Post-awareness: Collab planning */
    char *plan_ctx = NULL;
    size_t plan_len = 0;
    hu_collab_plan_build_prompt(&alloc, NULL, 0, NULL, 0, &plan_ctx, &plan_len);
    if (plan_ctx)
        hu_str_free(&alloc, plan_ctx);

    /* Post-awareness: Timezone */
    hu_timezone_info_t tz = hu_timezone_compute(-5, (uint64_t)time(NULL) * 1000);
    char *tz_dir = NULL;
    size_t tz_len = 0;
    hu_timezone_build_directive(&alloc, &tz, "test_user", 9, &tz_dir, &tz_len);
    if (tz_dir)
        hu_str_free(&alloc, tz_dir);

    /* Proactive: Plan proposal gating */
    bool should = hu_collab_plan_should_propose("test", 4, 0, 3, 0.5);
    (void)should;
}
#endif

void run_e2e_tests(void) {
    HU_TEST_SUITE("E2E");
    HU_RUN_TEST(test_agent_from_config_basic);
    HU_RUN_TEST(test_agent_from_config_null_alloc);
    HU_RUN_TEST(test_agent_from_config_null_provider);
    HU_RUN_TEST(test_agent_turn_simple);
    HU_RUN_TEST(test_agent_slash_help);
    HU_RUN_TEST(test_agent_slash_clear);
    HU_RUN_TEST(test_agent_sessions_command);
    HU_RUN_TEST(test_agent_kill_command);
    HU_RUN_TEST(test_agent_retry_command);
    HU_RUN_TEST(test_agent_slash_model);
    HU_RUN_TEST(test_agent_slash_status);
    HU_RUN_TEST(test_config_defaults);
    HU_RUN_TEST(test_json_parse_malformed);
    HU_RUN_TEST(test_json_parse_null_input);
    HU_RUN_TEST(test_tool_result_ownership);

    HU_RUN_TEST(test_agent_with_tool);
    HU_RUN_TEST(test_agent_multi_turn);
    HU_RUN_TEST(test_config_load_then_parse);
    HU_RUN_TEST(test_tool_result_fail);
    HU_RUN_TEST(test_health_mark_ok);
    HU_RUN_TEST(test_health_mark_error);
    HU_RUN_TEST(test_agent_custom_instructions);
    HU_RUN_TEST(test_agent_from_config_max_iterations);
    HU_RUN_TEST(test_agent_deinit_cleans);
    HU_RUN_TEST(test_agent_turn_empty_message);
    HU_RUN_TEST(test_agent_with_observer);
    HU_RUN_TEST(test_provider_create_from_config);
    HU_RUN_TEST(test_config_validate_after_load);
    HU_RUN_TEST(test_agent_tool_call_round_trip);

    HU_RUN_TEST(test_outcome_tracker_init);
    HU_RUN_TEST(test_outcome_record_tool_success);
    HU_RUN_TEST(test_outcome_record_tool_failure);
    HU_RUN_TEST(test_outcome_record_correction);
    HU_RUN_TEST(test_outcome_record_positive);
    HU_RUN_TEST(test_outcome_get_recent);
    HU_RUN_TEST(test_outcome_build_summary);
    HU_RUN_TEST(test_outcome_detect_repeated_failure);

#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_agent_multi_turn_with_sqlite_memory);
#endif
#ifdef HU_HAS_PERSONA
    HU_RUN_TEST(test_daemon_module_pipeline_no_crash);
#endif
}
