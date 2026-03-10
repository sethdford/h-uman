#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "human/providers/anthropic.h"
#include "human/providers/codex_cli.h"
#include "human/providers/error_classify.h"
#include "human/providers/factory.h"
#include "human/providers/ollama.h"
#include "human/providers/openai.h"
#include "human/providers/openai_codex.h"
#include "human/providers/reliable.h"
#include "human/providers/router.h"
#include "human/providers/scrub.h"
#include "human/providers/sse.h"
#include "test_framework.h"
#include <string.h>

static void test_openai_create_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_openai_create(&alloc, "test-key", 8, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    HU_ASSERT_NOT_NULL(prov.vtable);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_get_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_openai_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    const char *name = prov.vtable->get_name(prov.ctx);
    HU_ASSERT_STR_EQ(name, "openai");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_supports_native_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_openai_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_provider_factory_openai(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "openai", 6, "sk-test", 7, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openai");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_provider_factory_anthropic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "anthropic", 9, "test-key", 8, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(prov.vtable != NULL);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "anthropic");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_chat_mock_in_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_openai_create(&alloc, "test-key", 8, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);

    hu_chat_message_t msgs[1];
    msgs[0].role = HU_ROLE_USER;
    msgs[0].content = "hello";
    msgs[0].content_len = 5;
    msgs[0].name = NULL;
    msgs[0].name_len = 0;
    msgs[0].tool_call_id = NULL;
    msgs[0].tool_call_id_len = 0;
    msgs[0].content_parts = NULL;
    msgs[0].content_parts_count = 0;

    hu_chat_request_t req = {
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

    hu_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4", 5, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    HU_ASSERT_TRUE(resp.content_len > 0);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_chat_with_tools_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_openai_create(&alloc, "test-key", 8, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);

    hu_chat_message_t msgs[1];
    msgs[0].role = HU_ROLE_USER;
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

    hu_tool_spec_t tools[1] = {{
        .name = "shell",
        .name_len = 5,
        .description = "Run shell command",
        .description_len = 16,
        .parameters_json =
            "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}}}",
        .parameters_json_len = 55,
    }};

    hu_chat_request_t req = {
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

    hu_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4", 5, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(resp.tool_calls_count, 1u);
    HU_ASSERT_NOT_NULL(resp.tool_calls);
    HU_ASSERT_TRUE(resp.tool_calls[0].name_len == 5);
    HU_ASSERT_TRUE(memcmp(resp.tool_calls[0].name, "shell", 5) == 0);
    HU_ASSERT_TRUE(resp.tool_calls[0].arguments_len > 0);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_wraps_inner_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t inner;
    hu_error_t err = hu_openai_create(&alloc, "key", 3, NULL, 0, &inner);
    HU_ASSERT_EQ(err, HU_OK);

    hu_provider_t reliable;
    err = hu_reliable_create(&alloc, inner, 2, 50, &reliable);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(reliable.vtable->get_name(reliable.ctx), "openai");

    reliable.vtable->deinit(reliable.ctx, &alloc);
}

static void test_reliable_retries_then_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t inner;
    hu_error_t err = hu_openai_create(&alloc, "key", 3, NULL, 0, &inner);
    HU_ASSERT_EQ(err, HU_OK);

    hu_provider_t reliable;
    err = hu_reliable_create(&alloc, inner, 3, 50, &reliable);
    HU_ASSERT_EQ(err, HU_OK);

    hu_chat_message_t msgs[1] = {{.role = HU_ROLE_USER,
                                  .content = "hi",
                                  .content_len = 2,
                                  .name = NULL,
                                  .name_len = 0,
                                  .tool_call_id = NULL,
                                  .tool_call_id_len = 0,
                                  .content_parts = NULL,
                                  .content_parts_count = 0}};
    hu_chat_request_t req = {.messages = msgs,
                             .messages_count = 1,
                             .model = "gpt-4",
                             .model_len = 5,
                             .temperature = 0.7,
                             .max_tokens = 0,
                             .tools = NULL,
                             .tools_count = 0,
                             .timeout_secs = 0,
                             .reasoning_effort = NULL,
                             .reasoning_effort_len = 0};
    hu_chat_response_t resp = {0};
    err = reliable.vtable->chat(reliable.ctx, &alloc, &req, "gpt-4", 5, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    hu_chat_response_free(&alloc, &resp);
    reliable.vtable->deinit(reliable.ctx, &alloc);
}

static void test_router_resolves_hint(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t fast_prov, smart_prov;
    hu_openai_create(&alloc, "a", 1, NULL, 0, &fast_prov);
    hu_anthropic_create(&alloc, "b", 1, NULL, 0, &smart_prov);

    const char *names[] = {"fast", "smart"};
    size_t name_lens[] = {4, 5};
    hu_provider_t providers[] = {fast_prov, smart_prov};
    hu_router_route_entry_t routes[1] = {{.hint = "reasoning",
                                          .hint_len = 9,
                                          .route = {.provider_name = "smart",
                                                    .provider_name_len = 5,
                                                    .model = "claude-opus",
                                                    .model_len = 11}}};

    hu_provider_t router;
    hu_error_t err =
        hu_router_create(&alloc, names, name_lens, 2, providers, routes, 1, "default", 7, &router);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(router.vtable->get_name(router.ctx), "router");
    router.vtable->deinit(router.ctx, &alloc);
    fast_prov.vtable->deinit(fast_prov.ctx, &alloc);
    smart_prov.vtable->deinit(smart_prov.ctx, &alloc);
}

static void test_multi_model_router_below_threshold_uses_fast(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t fast_prov, standard_prov;
    hu_ollama_create(&alloc, NULL, 0, NULL, 0, &fast_prov);
    hu_openai_create(&alloc, "key", 3, NULL, 0, &standard_prov);

    hu_multi_model_router_config_t cfg = {
        .fast = fast_prov,
        .standard = standard_prov,
        .powerful = {0},
        .complexity_threshold_low = 50,
        .complexity_threshold_high = 500,
    };
    hu_provider_t router;
    hu_error_t err = hu_multi_model_router_create(&alloc, &cfg, &router);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(router.vtable->get_name(router.ctx), "router");

    /* Short message: ~1 token -> fast */
    hu_chat_message_t msgs[1] = {{.role = HU_ROLE_USER,
                                  .content = "hi",
                                  .content_len = 2,
                                  .name = NULL,
                                  .name_len = 0,
                                  .tool_call_id = NULL,
                                  .tool_call_id_len = 0,
                                  .content_parts = NULL,
                                  .content_parts_count = 0}};
    hu_chat_request_t req = {.messages = msgs,
                             .messages_count = 1,
                             .model = "gpt-4",
                             .model_len = 5,
                             .temperature = 0.7,
                             .max_tokens = 0,
                             .tools = NULL,
                             .tools_count = 0,
                             .timeout_secs = 0,
                             .reasoning_effort = NULL,
                             .reasoning_effort_len = 0};
    hu_chat_response_t resp = {0};
    err = router.vtable->chat(router.ctx, &alloc, &req, "gpt-4", 5, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    hu_chat_response_free(&alloc, &resp);
    router.vtable->deinit(router.ctx, &alloc);
}

static void test_multi_model_router_between_thresholds_uses_standard(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t fast_prov, standard_prov;
    hu_ollama_create(&alloc, NULL, 0, NULL, 0, &fast_prov);
    hu_openai_create(&alloc, "key", 3, NULL, 0, &standard_prov);

    hu_multi_model_router_config_t cfg = {
        .fast = fast_prov,
        .standard = standard_prov,
        .powerful = {0},
        .complexity_threshold_low = 50,
        .complexity_threshold_high = 500,
    };
    hu_provider_t router;
    hu_error_t err = hu_multi_model_router_create(&alloc, &cfg, &router);
    HU_ASSERT_EQ(err, HU_OK);

    /* ~100 tokens: between 50 and 500 -> standard */
    char buf[420];
    for (size_t i = 0; i < sizeof(buf) - 1; i++)
        buf[i] = 'x';
    buf[sizeof(buf) - 1] = '\0';
    hu_chat_message_t msgs[1] = {{.role = HU_ROLE_USER,
                                  .content = buf,
                                  .content_len = sizeof(buf) - 1,
                                  .name = NULL,
                                  .name_len = 0,
                                  .tool_call_id = NULL,
                                  .tool_call_id_len = 0,
                                  .content_parts = NULL,
                                  .content_parts_count = 0}};
    hu_chat_request_t req = {.messages = msgs,
                             .messages_count = 1,
                             .model = "gpt-4",
                             .model_len = 5,
                             .temperature = 0.7,
                             .max_tokens = 0,
                             .tools = NULL,
                             .tools_count = 0,
                             .timeout_secs = 0,
                             .reasoning_effort = NULL,
                             .reasoning_effort_len = 0};
    hu_chat_response_t resp = {0};
    err = router.vtable->chat(router.ctx, &alloc, &req, "gpt-4", 5, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    hu_chat_response_free(&alloc, &resp);
    router.vtable->deinit(router.ctx, &alloc);
}

static void test_multi_model_router_above_threshold_uses_powerful(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t standard_prov, powerful_prov;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &standard_prov);
    hu_anthropic_create(&alloc, "key", 3, NULL, 0, &powerful_prov);

    hu_multi_model_router_config_t cfg = {
        .fast = {0},
        .standard = standard_prov,
        .powerful = powerful_prov,
        .complexity_threshold_low = 50,
        .complexity_threshold_high = 500,
    };
    hu_provider_t router;
    hu_error_t err = hu_multi_model_router_create(&alloc, &cfg, &router);
    HU_ASSERT_EQ(err, HU_OK);

    /* ~600 tokens -> powerful */
    char buf[2500];
    for (size_t i = 0; i < sizeof(buf) - 1; i++)
        buf[i] = 'x';
    buf[sizeof(buf) - 1] = '\0';
    hu_chat_message_t msgs[1] = {{.role = HU_ROLE_USER,
                                  .content = buf,
                                  .content_len = sizeof(buf) - 1,
                                  .name = NULL,
                                  .name_len = 0,
                                  .tool_call_id = NULL,
                                  .tool_call_id_len = 0,
                                  .content_parts = NULL,
                                  .content_parts_count = 0}};
    hu_chat_request_t req = {.messages = msgs,
                             .messages_count = 1,
                             .model = "claude",
                             .model_len = 6,
                             .temperature = 0.7,
                             .max_tokens = 0,
                             .tools = NULL,
                             .tools_count = 0,
                             .timeout_secs = 0,
                             .reasoning_effort = NULL,
                             .reasoning_effort_len = 0};
    hu_chat_response_t resp = {0};
    err = router.vtable->chat(router.ctx, &alloc, &req, "claude", 6, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    hu_chat_response_free(&alloc, &resp);
    router.vtable->deinit(router.ctx, &alloc);
}

static void test_multi_model_router_missing_fast_falls_back_to_standard(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t standard_prov;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &standard_prov);

    hu_multi_model_router_config_t cfg = {
        .fast = {0},
        .standard = standard_prov,
        .powerful = {0},
        .complexity_threshold_low = 50,
        .complexity_threshold_high = 500,
    };
    hu_provider_t router;
    hu_error_t err = hu_multi_model_router_create(&alloc, &cfg, &router);
    HU_ASSERT_EQ(err, HU_OK);

    /* Very short -> would use fast, but fast missing -> standard */
    hu_chat_message_t msgs[1] = {{.role = HU_ROLE_USER,
                                  .content = "hi",
                                  .content_len = 2,
                                  .name = NULL,
                                  .name_len = 0,
                                  .tool_call_id = NULL,
                                  .tool_call_id_len = 0,
                                  .content_parts = NULL,
                                  .content_parts_count = 0}};
    hu_chat_request_t req = {.messages = msgs,
                             .messages_count = 1,
                             .model = "gpt-4",
                             .model_len = 5,
                             .temperature = 0.7,
                             .max_tokens = 0,
                             .tools = NULL,
                             .tools_count = 0,
                             .timeout_secs = 0,
                             .reasoning_effort = NULL,
                             .reasoning_effort_len = 0};
    hu_chat_response_t resp = {0};
    err = router.vtable->chat(router.ctx, &alloc, &req, "gpt-4", 5, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    hu_chat_response_free(&alloc, &resp);
    router.vtable->deinit(router.ctx, &alloc);
}

static void test_codex_cli_create_and_chat(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_codex_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "codex-cli");
    hu_chat_message_t msgs[1] = {{.role = HU_ROLE_USER,
                                  .content = "hello",
                                  .content_len = 5,
                                  .name = NULL,
                                  .name_len = 0,
                                  .tool_call_id = NULL,
                                  .tool_call_id_len = 0,
                                  .content_parts = NULL,
                                  .content_parts_count = 0}};
    hu_chat_request_t req = {.messages = msgs,
                             .messages_count = 1,
                             .model = "m",
                             .model_len = 1,
                             .temperature = 0.7,
                             .max_tokens = 0,
                             .tools = NULL,
                             .tools_count = 0,
                             .timeout_secs = 0,
                             .reasoning_effort = NULL,
                             .reasoning_effort_len = 0};
    hu_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "m", 1, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    hu_chat_response_free(&alloc, &resp);
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_codex_create_and_chat(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_openai_codex_create(&alloc, "test-key", 8, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openai-codex");
    hu_chat_message_t msgs[1] = {{.role = HU_ROLE_USER,
                                  .content = "hi",
                                  .content_len = 2,
                                  .name = NULL,
                                  .name_len = 0,
                                  .tool_call_id = NULL,
                                  .tool_call_id_len = 0,
                                  .content_parts = NULL,
                                  .content_parts_count = 0}};
    hu_chat_request_t req = {.messages = msgs,
                             .messages_count = 1,
                             .model = "o4-mini",
                             .model_len = 7,
                             .temperature = 0.7,
                             .max_tokens = 0,
                             .tools = NULL,
                             .tools_count = 0,
                             .timeout_secs = 0,
                             .reasoning_effort = NULL,
                             .reasoning_effort_len = 0};
    hu_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "o4-mini", 7, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    hu_chat_response_free(&alloc, &resp);
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_error_classify_non_retryable(void) {
    HU_ASSERT_TRUE(hu_error_is_non_retryable("400 Bad Request", 16));
    HU_ASSERT_TRUE(hu_error_is_non_retryable("401 Unauthorized", 16));
    HU_ASSERT_FALSE(hu_error_is_non_retryable("429 Too Many Requests", 21));
}

static void test_error_classify_rate_limited(void) {
    HU_ASSERT_TRUE(hu_error_is_rate_limited("429 rate limit exceeded", 22));
    HU_ASSERT_FALSE(hu_error_is_rate_limited("401 Unauthorized", 15));
}

static void test_sse_parse_delta(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *line = "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}";
    hu_sse_line_result_t res;
    hu_error_t err = hu_sse_parse_line(&alloc, line, strlen(line), &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(res.tag, HU_SSE_DELTA);
    HU_ASSERT_NOT_NULL(res.delta);
    HU_ASSERT_STR_EQ(res.delta, "Hello");
    if (res.delta)
        alloc.free(alloc.ctx, res.delta, res.delta_len + 1);
}

static void test_sse_parse_done(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *line = "data: [DONE]";
    hu_sse_line_result_t res;
    hu_error_t err = hu_sse_parse_line(&alloc, line, strlen(line), &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(res.tag, HU_SSE_DONE);
}

static void test_scrub_redacts_sk(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *input = "key sk-1234567890abcdef";
    char *out = hu_scrub_secret_patterns(&alloc, input, strlen(input));
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "[REDACTED]") != NULL);
    HU_ASSERT_TRUE(strstr(out, "sk-1234567890abcdef") == NULL);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_factory_codex_cli(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "codex_cli", 9, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "codex-cli");
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_openai_codex(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "openai-codex", 12, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openai-codex");
    prov.vtable->deinit(prov.ctx, &alloc);
}

void run_provider_tests(void) {
    HU_TEST_SUITE("Provider");
    HU_RUN_TEST(test_openai_create_succeeds);
    HU_RUN_TEST(test_openai_get_name);
    HU_RUN_TEST(test_openai_supports_native_tools);
    HU_RUN_TEST(test_provider_factory_openai);
    HU_RUN_TEST(test_provider_factory_anthropic);
    HU_RUN_TEST(test_openai_chat_mock_in_test);
    HU_RUN_TEST(test_openai_chat_with_tools_mock);
    HU_RUN_TEST(test_reliable_wraps_inner_succeeds);
    HU_RUN_TEST(test_reliable_retries_then_succeeds);
    HU_RUN_TEST(test_router_resolves_hint);
    HU_RUN_TEST(test_multi_model_router_below_threshold_uses_fast);
    HU_RUN_TEST(test_multi_model_router_between_thresholds_uses_standard);
    HU_RUN_TEST(test_multi_model_router_above_threshold_uses_powerful);
    HU_RUN_TEST(test_multi_model_router_missing_fast_falls_back_to_standard);
    HU_RUN_TEST(test_codex_cli_create_and_chat);
    HU_RUN_TEST(test_openai_codex_create_and_chat);
    HU_RUN_TEST(test_error_classify_non_retryable);
    HU_RUN_TEST(test_error_classify_rate_limited);
    HU_RUN_TEST(test_sse_parse_delta);
    HU_RUN_TEST(test_sse_parse_done);
    HU_RUN_TEST(test_scrub_redacts_sk);
    HU_RUN_TEST(test_factory_codex_cli);
    HU_RUN_TEST(test_factory_openai_codex);
}
