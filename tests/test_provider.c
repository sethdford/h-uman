#include "test_framework.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/provider.h"
#include "seaclaw/providers/openai.h"
#include "seaclaw/providers/anthropic.h"
#include "seaclaw/providers/reliable.h"
#include "seaclaw/providers/router.h"
#include "seaclaw/providers/codex_cli.h"
#include "seaclaw/providers/openai_codex.h"
#include "seaclaw/providers/error_classify.h"
#include "seaclaw/providers/sse.h"
#include "seaclaw/providers/scrub.h"
#include "seaclaw/providers/factory.h"
#include <string.h>

static void test_openai_create_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_openai_create(&alloc, "test-key", 8, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prov.ctx);
    SC_ASSERT_NOT_NULL(prov.vtable);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_get_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_openai_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    const char *name = prov.vtable->get_name(prov.ctx);
    SC_ASSERT_STR_EQ(name, "openai");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_supports_native_tools(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_openai_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_provider_factory_openai(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "openai", 6,
        "sk-test", 7, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openai");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_provider_factory_anthropic(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "anthropic", 9,
        "test-key", 8, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(prov.vtable != NULL);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "anthropic");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_chat_mock_in_test(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_openai_create(&alloc, "test-key", 8, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);

    sc_chat_message_t msgs[1];
    msgs[0].role = SC_ROLE_USER;
    msgs[0].content = "hello";
    msgs[0].content_len = 5;
    msgs[0].name = NULL;
    msgs[0].name_len = 0;
    msgs[0].tool_call_id = NULL;
    msgs[0].tool_call_id_len = 0;
    msgs[0].content_parts = NULL;
    msgs[0].content_parts_count = 0;

    sc_chat_request_t req = {
        .messages = msgs,
        .messages_count = 1,
        .model = "gpt-4",
        .model_len = 5,
        .temperature = 0.7,
        .max_tokens = 0,
        .tools = NULL,
        .tools_count = 0,
        .timeout_secs = 0,
        .reasoning_effort = NULL,
        .reasoning_effort_len = 0,
    };

    sc_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4", 5, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    SC_ASSERT_TRUE(resp.content_len > 0);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_chat_with_tools_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_openai_create(&alloc, "test-key", 8, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);

    sc_chat_message_t msgs[1];
    msgs[0].role = SC_ROLE_USER;
    msgs[0].content = "list files";
    msgs[0].content_len = 10;
    msgs[0].name = NULL;
    msgs[0].name_len = 0;
    msgs[0].tool_call_id = NULL;
    msgs[0].tool_call_id_len = 0;
    msgs[0].content_parts = NULL;
    msgs[0].content_parts_count = 0;
    msgs[0].tool_calls = NULL;
    msgs[0].tool_calls_count = 0;

    sc_tool_spec_t tools[1] = {{
        .name = "shell",
        .name_len = 5,
        .description = "Run shell command",
        .description_len = 16,
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}}}",
        .parameters_json_len = 55,
    }};

    sc_chat_request_t req = {
        .messages = msgs,
        .messages_count = 1,
        .model = "gpt-4",
        .model_len = 5,
        .temperature = 0.7,
        .max_tokens = 0,
        .tools = tools,
        .tools_count = 1,
        .timeout_secs = 0,
        .reasoning_effort = NULL,
        .reasoning_effort_len = 0,
    };

    sc_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4", 5, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(resp.tool_calls_count, 1u);
    SC_ASSERT_NOT_NULL(resp.tool_calls);
    SC_ASSERT_TRUE(resp.tool_calls[0].name_len == 5);
    SC_ASSERT_TRUE(memcmp(resp.tool_calls[0].name, "shell", 5) == 0);
    SC_ASSERT_TRUE(resp.tool_calls[0].arguments_len > 0);
    sc_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_wraps_inner_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t inner;
    sc_error_t err = sc_openai_create(&alloc, "key", 3, NULL, 0, &inner);
    SC_ASSERT_EQ(err, SC_OK);

    sc_provider_t reliable;
    err = sc_reliable_create(&alloc, inner, 2, 50, &reliable);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(reliable.vtable->get_name(reliable.ctx), "openai");

    reliable.vtable->deinit(reliable.ctx, &alloc);
}

static void test_reliable_retries_then_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t inner;
    sc_error_t err = sc_openai_create(&alloc, "key", 3, NULL, 0, &inner);
    SC_ASSERT_EQ(err, SC_OK);

    sc_provider_t reliable;
    err = sc_reliable_create(&alloc, inner, 3, 50, &reliable);
    SC_ASSERT_EQ(err, SC_OK);

    sc_chat_message_t msgs[1] = {{.role = SC_ROLE_USER, .content = "hi", .content_len = 2,
        .name = NULL, .name_len = 0, .tool_call_id = NULL, .tool_call_id_len = 0,
        .content_parts = NULL, .content_parts_count = 0}};
    sc_chat_request_t req = {.messages = msgs, .messages_count = 1, .model = "gpt-4", .model_len = 5,
        .temperature = 0.7, .max_tokens = 0, .tools = NULL, .tools_count = 0,
        .timeout_secs = 0, .reasoning_effort = NULL, .reasoning_effort_len = 0};
    sc_chat_response_t resp = {0};
    err = reliable.vtable->chat(reliable.ctx, &alloc, &req, "gpt-4", 5, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    reliable.vtable->deinit(reliable.ctx, &alloc);
}

static void test_router_resolves_hint(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t fast_prov, smart_prov;
    sc_openai_create(&alloc, "a", 1, NULL, 0, &fast_prov);
    sc_anthropic_create(&alloc, "b", 1, NULL, 0, &smart_prov);

    const char *names[] = {"fast", "smart"};
    size_t name_lens[] = {4, 5};
    sc_provider_t providers[] = {fast_prov, smart_prov};
    sc_router_route_entry_t routes[1] = {
        {.hint = "reasoning", .hint_len = 9, .route = {.provider_name = "smart", .provider_name_len = 5, .model = "claude-opus", .model_len = 11}}
    };

    sc_provider_t router;
    sc_error_t err = sc_router_create(&alloc, names, name_lens, 2, providers,
        routes, 1, "default", 7, &router);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(router.vtable->get_name(router.ctx), "router");
    router.vtable->deinit(router.ctx, &alloc);
    fast_prov.vtable->deinit(fast_prov.ctx, &alloc);
    smart_prov.vtable->deinit(smart_prov.ctx, &alloc);
}

static void test_codex_cli_create_and_chat(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_codex_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "codex-cli");
    sc_chat_message_t msgs[1] = {{.role = SC_ROLE_USER, .content = "hello", .content_len = 5,
        .name = NULL, .name_len = 0, .tool_call_id = NULL, .tool_call_id_len = 0,
        .content_parts = NULL, .content_parts_count = 0}};
    sc_chat_request_t req = {.messages = msgs, .messages_count = 1, .model = "m", .model_len = 1,
        .temperature = 0.7, .max_tokens = 0, .tools = NULL, .tools_count = 0,
        .timeout_secs = 0, .reasoning_effort = NULL, .reasoning_effort_len = 0};
    sc_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "m", 1, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_codex_create_and_chat(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_openai_codex_create(&alloc, "test-key", 8, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openai-codex");
    sc_chat_message_t msgs[1] = {{.role = SC_ROLE_USER, .content = "hi", .content_len = 2,
        .name = NULL, .name_len = 0, .tool_call_id = NULL, .tool_call_id_len = 0,
        .content_parts = NULL, .content_parts_count = 0}};
    sc_chat_request_t req = {.messages = msgs, .messages_count = 1, .model = "o4-mini", .model_len = 7,
        .temperature = 0.7, .max_tokens = 0, .tools = NULL, .tools_count = 0,
        .timeout_secs = 0, .reasoning_effort = NULL, .reasoning_effort_len = 0};
    sc_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "o4-mini", 7, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_error_classify_non_retryable(void) {
    SC_ASSERT_TRUE(sc_error_is_non_retryable("400 Bad Request", 16));
    SC_ASSERT_TRUE(sc_error_is_non_retryable("401 Unauthorized", 16));
    SC_ASSERT_FALSE(sc_error_is_non_retryable("429 Too Many Requests", 21));
}

static void test_error_classify_rate_limited(void) {
    SC_ASSERT_TRUE(sc_error_is_rate_limited("429 rate limit exceeded", 22));
    SC_ASSERT_FALSE(sc_error_is_rate_limited("401 Unauthorized", 15));
}

static void test_sse_parse_delta(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *line = "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}";
    sc_sse_line_result_t res;
    sc_error_t err = sc_sse_parse_line(&alloc, line, strlen(line), &res);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(res.tag, SC_SSE_DELTA);
    SC_ASSERT_NOT_NULL(res.delta);
    SC_ASSERT_STR_EQ(res.delta, "Hello");
    if (res.delta) alloc.free(alloc.ctx, res.delta, res.delta_len + 1);
}

static void test_sse_parse_done(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *line = "data: [DONE]";
    sc_sse_line_result_t res;
    sc_error_t err = sc_sse_parse_line(&alloc, line, strlen(line), &res);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(res.tag, SC_SSE_DONE);
}

static void test_scrub_redacts_sk(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *input = "key sk-1234567890abcdef";
    char *out = sc_scrub_secret_patterns(&alloc, input, strlen(input));
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(strstr(out, "[REDACTED]") != NULL);
    SC_ASSERT_TRUE(strstr(out, "sk-1234567890abcdef") == NULL);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_factory_codex_cli(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "codex_cli", 9, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "codex-cli");
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_openai_codex(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "openai-codex", 12, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openai-codex");
    prov.vtable->deinit(prov.ctx, &alloc);
}

void run_provider_tests(void) {
    SC_TEST_SUITE("Provider");
    SC_RUN_TEST(test_openai_create_succeeds);
    SC_RUN_TEST(test_openai_get_name);
    SC_RUN_TEST(test_openai_supports_native_tools);
    SC_RUN_TEST(test_provider_factory_openai);
    SC_RUN_TEST(test_provider_factory_anthropic);
    SC_RUN_TEST(test_openai_chat_mock_in_test);
    SC_RUN_TEST(test_openai_chat_with_tools_mock);
    SC_RUN_TEST(test_reliable_wraps_inner_succeeds);
    SC_RUN_TEST(test_reliable_retries_then_succeeds);
    SC_RUN_TEST(test_router_resolves_hint);
    SC_RUN_TEST(test_codex_cli_create_and_chat);
    SC_RUN_TEST(test_openai_codex_create_and_chat);
    SC_RUN_TEST(test_error_classify_non_retryable);
    SC_RUN_TEST(test_error_classify_rate_limited);
    SC_RUN_TEST(test_sse_parse_delta);
    SC_RUN_TEST(test_sse_parse_done);
    SC_RUN_TEST(test_scrub_redacts_sk);
    SC_RUN_TEST(test_factory_codex_cli);
    SC_RUN_TEST(test_factory_openai_codex);
}
