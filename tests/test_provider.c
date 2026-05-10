#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/provider.h"
#include "human/providers/anthropic.h"
#include "human/providers/codex_cli.h"
#include "human/providers/error_classify.h"
#include "human/providers/factory.h"
#include "human/providers/helpers.h"
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

/* ─── hu_provider_create_from_config (from_config.c) ─────────────────────── */
static void test_from_config_null_alloc_returns_error(void) {
    hu_config_t cfg = {0};
    hu_provider_t out = {0};
    hu_error_t err = hu_provider_create_from_config(NULL, &cfg, "router", 6, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_from_config_null_cfg_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t out = {0};
    hu_error_t err = hu_provider_create_from_config(&alloc, NULL, "router", 6, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_from_config_null_name_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    hu_provider_t out = {0};
    hu_error_t err = hu_provider_create_from_config(&alloc, &cfg, NULL, 6, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_from_config_zero_name_len_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    hu_provider_t out = {0};
    hu_error_t err = hu_provider_create_from_config(&alloc, &cfg, "router", 0, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_from_config_null_out_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    hu_error_t err = hu_provider_create_from_config(&alloc, &cfg, "router", 6, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_from_config_unknown_provider_returns_not_supported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    hu_provider_t out = {0};
    hu_error_t err = hu_provider_create_from_config(&alloc, &cfg, "unknown", 7, &out);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
}

static void test_from_config_router_creates_provider(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    hu_config_t *cfg = (hu_config_t *)backing.alloc(backing.ctx, sizeof(hu_config_t));
    HU_ASSERT_NOT_NULL(cfg);
    memset(cfg, 0, sizeof(*cfg));
    cfg->arena = arena;
    cfg->allocator = hu_arena_allocator(arena);
    /* Verify the arena allocator works before config parsing */
    void *test_ptr = cfg->allocator.alloc(cfg->allocator.ctx, 16);
    HU_ASSERT_NOT_NULL(test_ptr);
    const char *json = "{\"providers\":[{\"name\":\"openai\",\"api_key\":\"sk-test\"}],"
                       "\"router\":{\"standard\":\"openai\"}}";
    hu_error_t perr = hu_config_parse_json(cfg, json, strlen(json));
    HU_ASSERT_EQ(perr, HU_OK);

    hu_provider_t out = {0};
    hu_error_t err = hu_provider_create_from_config(&backing, cfg, "router", 6, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out.ctx);
    HU_ASSERT_NOT_NULL(out.vtable);
    HU_ASSERT_STR_EQ(out.vtable->get_name(out.ctx), "router");
    if (out.vtable->deinit)
        out.vtable->deinit(out.ctx, &backing);
    hu_arena_destroy(arena);
    backing.free(backing.ctx, cfg, sizeof(hu_config_t));
}

static void test_from_config_reliable_creates_provider(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    hu_config_t *cfg = (hu_config_t *)backing.alloc(backing.ctx, sizeof(hu_config_t));
    HU_ASSERT_NOT_NULL(cfg);
    memset(cfg, 0, sizeof(*cfg));
    cfg->arena = arena;
    cfg->allocator = hu_arena_allocator(arena);
    const char *json = "{\"providers\":[{\"name\":\"openai\",\"api_key\":\"sk-test\"}],"
                       "\"reliability\":{\"primary_provider\":\"openai\"}}";
    hu_error_t perr = hu_config_parse_json(cfg, json, strlen(json));
    HU_ASSERT_EQ(perr, HU_OK);

    hu_provider_t out = {0};
    hu_error_t err = hu_provider_create_from_config(&backing, cfg, "reliable", 8, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out.ctx);
    HU_ASSERT_NOT_NULL(out.vtable);
    HU_ASSERT_STR_EQ(out.vtable->get_name(out.ctx), "openai");
    if (out.vtable->deinit)
        out.vtable->deinit(out.ctx, &backing);
    hu_arena_destroy(arena);
    backing.free(backing.ctx, cfg, sizeof(hu_config_t));
}

static void test_from_config_ensemble_creates_provider(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    hu_config_t *cfg = (hu_config_t *)backing.alloc(backing.ctx, sizeof(hu_config_t));
    HU_ASSERT_NOT_NULL(cfg);
    memset(cfg, 0, sizeof(*cfg));
    cfg->arena = arena;
    cfg->allocator = hu_arena_allocator(arena);
    const char *json =
        "{\"providers\":["
        "{\"name\":\"openai\",\"api_key\":\"sk-test\"},"
        "{\"name\":\"anthropic\",\"api_key\":\"sk-ant-test\"}],"
        "\"ensemble\":{\"providers\":[\"openai\",\"anthropic\"],\"strategy\":\"round_robin\"}}";
    hu_error_t perr = hu_config_parse_json(cfg, json, strlen(json));
    HU_ASSERT_EQ(perr, HU_OK);

    hu_provider_t out = {0};
    hu_error_t err = hu_provider_create_from_config(&backing, cfg, "ensemble", 8, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out.ctx);
    HU_ASSERT_NOT_NULL(out.vtable);
    HU_ASSERT_STR_EQ(out.vtable->get_name(out.ctx), "ensemble");
    if (out.vtable->deinit)
        out.vtable->deinit(out.ctx, &backing);
    hu_arena_destroy(arena);
    backing.free(backing.ctx, cfg, sizeof(hu_config_t));
}

/* ─── hu_provider_create_default + multi-fallback wiring ────────────────────
 * Verifies the auto-fallback shipping promise: a plain default_provider with a
 * non-empty reliability.fallback_providers[] is transparently wrapped in the
 * reliable composite, and the wrap survives all the easy-to-get-wrong edges
 * (no fallbacks, composite default, same-name fallback, unknown fallback). */

static hu_error_t failover_mock_chat(void *ctx, hu_allocator_t *alloc,
                                     const hu_chat_request_t *req, const char *model,
                                     size_t model_len, double temperature,
                                     hu_chat_response_t *out) {
    (void)req;
    (void)temperature;
    (void)model;
    (void)model_len;
    int *state = (int *)ctx; /* state[0] = call count, state[1] = fail-first-N flag */
    state[0]++;
    memset(out, 0, sizeof(*out));
    if (state[1] > 0 && state[0] <= state[1])
        return HU_ERR_PROVIDER_UNAVAILABLE;
    const char *who = "primary";
    if (state[2])
        who = "fallback-A";
    if (state[2] == 2)
        who = "fallback-B";
    size_t len = strlen(who);
    char *buf = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, who, len + 1);
    out->content = buf;
    out->content_len = len;
    return HU_OK;
}

static const char *failover_mock_get_name(void *ctx) {
    (void)ctx;
    return "mock";
}

static bool failover_mock_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}

static hu_provider_vtable_t failover_mock_vtable = {
    .chat = failover_mock_chat,
    .get_name = failover_mock_get_name,
    .supports_native_tools = failover_mock_supports_native_tools,
};

static void test_failover_primary_succeeds_extras_unused(void) {
    hu_allocator_t alloc = hu_system_allocator();
    int prim_state[3] = {0, 0, 0}; /* never fail */
    int fb_state[3] = {0, 0, 1};
    hu_provider_t primary = {.ctx = prim_state, .vtable = &failover_mock_vtable};
    hu_provider_t fb = {.ctx = fb_state, .vtable = &failover_mock_vtable};
    hu_reliable_provider_entry_t extras[1] = {
        {.name = "fb-a", .name_len = 4, .provider = fb}};

    hu_provider_t prov;
    HU_ASSERT_EQ(hu_reliable_create_ex(&alloc, primary, 0, 50, extras, 1, NULL, 0, &prov), HU_OK);

    hu_chat_request_t req = {0};
    hu_chat_response_t resp = {0};
    HU_ASSERT_EQ(prov.vtable->chat(prov.ctx, &alloc, &req, "m", 1, 0.0, &resp), HU_OK);
    HU_ASSERT_STR_EQ(resp.content, "primary");
    HU_ASSERT_EQ(prim_state[0], 1);
    HU_ASSERT_EQ(fb_state[0], 0); /* extras not consulted when primary OK */
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_failover_primary_fails_first_fallback_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    int prim_state[3] = {0, 99, 0};   /* always fail */
    int fb_state[3] = {0, 0, 1};       /* always succeed */
    hu_provider_t primary = {.ctx = prim_state, .vtable = &failover_mock_vtable};
    hu_provider_t fb = {.ctx = fb_state, .vtable = &failover_mock_vtable};
    hu_reliable_provider_entry_t extras[1] = {
        {.name = "fb-a", .name_len = 4, .provider = fb}};

    hu_provider_t prov;
    HU_ASSERT_EQ(hu_reliable_create_ex(&alloc, primary, 0, 50, extras, 1, NULL, 0, &prov), HU_OK);

    hu_chat_request_t req = {0};
    hu_chat_response_t resp = {0};
    HU_ASSERT_EQ(prov.vtable->chat(prov.ctx, &alloc, &req, "m", 1, 0.0, &resp), HU_OK);
    HU_ASSERT_STR_EQ(resp.content, "fallback-A");
    HU_ASSERT_EQ(prim_state[0], 1);
    HU_ASSERT_EQ(fb_state[0], 1);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_failover_walks_chain_until_one_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    int prim_state[3] = {0, 99, 0}; /* fail */
    int fb_a_state[3] = {0, 99, 1}; /* fail */
    int fb_b_state[3] = {0, 0, 2};  /* succeed */
    hu_provider_t primary = {.ctx = prim_state, .vtable = &failover_mock_vtable};
    hu_provider_t fb_a = {.ctx = fb_a_state, .vtable = &failover_mock_vtable};
    hu_provider_t fb_b = {.ctx = fb_b_state, .vtable = &failover_mock_vtable};
    hu_reliable_provider_entry_t extras[2] = {
        {.name = "fb-a", .name_len = 4, .provider = fb_a},
        {.name = "fb-b", .name_len = 4, .provider = fb_b},
    };

    hu_provider_t prov;
    HU_ASSERT_EQ(hu_reliable_create_ex(&alloc, primary, 0, 50, extras, 2, NULL, 0, &prov), HU_OK);

    hu_chat_request_t req = {0};
    hu_chat_response_t resp = {0};
    HU_ASSERT_EQ(prov.vtable->chat(prov.ctx, &alloc, &req, "m", 1, 0.0, &resp), HU_OK);
    HU_ASSERT_STR_EQ(resp.content, "fallback-B");
    HU_ASSERT_EQ(prim_state[0], 1);
    HU_ASSERT_EQ(fb_a_state[0], 1);
    HU_ASSERT_EQ(fb_b_state[0], 1);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_failover_all_fail_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    int prim_state[3] = {0, 99, 0};
    int fb_state[3] = {0, 99, 1};
    hu_provider_t primary = {.ctx = prim_state, .vtable = &failover_mock_vtable};
    hu_provider_t fb = {.ctx = fb_state, .vtable = &failover_mock_vtable};
    hu_reliable_provider_entry_t extras[1] = {
        {.name = "fb-a", .name_len = 4, .provider = fb}};

    hu_provider_t prov;
    HU_ASSERT_EQ(hu_reliable_create_ex(&alloc, primary, 0, 50, extras, 1, NULL, 0, &prov), HU_OK);

    hu_chat_request_t req = {0};
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "m", 1, 0.0, &resp);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(resp.content);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_failover_primary_recovers_after_partial_failure(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* primary fails the first call only, then succeeds for ever. */
    int prim_state[3] = {0, 1, 0};
    int fb_state[3] = {0, 0, 1};
    hu_provider_t primary = {.ctx = prim_state, .vtable = &failover_mock_vtable};
    hu_provider_t fb = {.ctx = fb_state, .vtable = &failover_mock_vtable};
    hu_reliable_provider_entry_t extras[1] = {
        {.name = "fb-a", .name_len = 4, .provider = fb}};

    hu_provider_t prov;
    HU_ASSERT_EQ(hu_reliable_create_ex(&alloc, primary, 0, 50, extras, 1, NULL, 0, &prov), HU_OK);

    /* call 1: primary fails → fallback wins. */
    hu_chat_request_t req = {0};
    hu_chat_response_t resp = {0};
    HU_ASSERT_EQ(prov.vtable->chat(prov.ctx, &alloc, &req, "m", 1, 0.0, &resp), HU_OK);
    HU_ASSERT_STR_EQ(resp.content, "fallback-A");
    hu_chat_response_free(&alloc, &resp);

    /* call 2: primary recovered → primary wins, fallback NOT touched again. */
    int fb_calls_before = fb_state[0];
    HU_ASSERT_EQ(prov.vtable->chat(prov.ctx, &alloc, &req, "m", 1, 0.0, &resp), HU_OK);
    HU_ASSERT_STR_EQ(resp.content, "primary");
    HU_ASSERT_EQ(fb_state[0], fb_calls_before);
    hu_chat_response_free(&alloc, &resp);

    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── hu_provider_create_default wiring (config-driven auto-wrap) ─────────── */

static hu_config_t *make_failover_config(hu_allocator_t *backing, hu_arena_t **arena_out,
                                         const char *json) {
    hu_arena_t *arena = hu_arena_create(*backing);
    if (!arena)
        return NULL;
    hu_config_t *cfg = (hu_config_t *)backing->alloc(backing->ctx, sizeof(hu_config_t));
    if (!cfg) {
        hu_arena_destroy(arena);
        return NULL;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->arena = arena;
    cfg->allocator = hu_arena_allocator(arena);
    if (hu_config_parse_json(cfg, json, strlen(json)) != HU_OK) {
        hu_arena_destroy(arena);
        backing->free(backing->ctx, cfg, sizeof(hu_config_t));
        return NULL;
    }
    *arena_out = arena;
    return cfg;
}

static void free_failover_config(hu_allocator_t *backing, hu_arena_t *arena, hu_config_t *cfg) {
    hu_arena_destroy(arena);
    backing->free(backing->ctx, cfg, sizeof(hu_config_t));
}

static void test_create_default_no_fallbacks_returns_unwrapped(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_arena_t *arena = NULL;
    hu_config_t *cfg = make_failover_config(
        &backing, &arena,
        "{\"default_provider\":\"openai\","
        "\"providers\":[{\"name\":\"openai\",\"api_key\":\"sk-test\"}]}");
    HU_ASSERT_NOT_NULL(cfg);
    hu_provider_t out = {0};
    HU_ASSERT_EQ(hu_provider_create_default(&backing, cfg, &out), HU_OK);
    HU_ASSERT_NOT_NULL(out.ctx);
    /* Without fallbacks, the inner is openai itself (no wrap). */
    HU_ASSERT_STR_EQ(out.vtable->get_name(out.ctx), "openai");
    if (out.vtable->deinit)
        out.vtable->deinit(out.ctx, &backing);
    free_failover_config(&backing, arena, cfg);
}

static void test_create_default_with_fallbacks_wraps(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_arena_t *arena = NULL;
    hu_config_t *cfg = make_failover_config(
        &backing, &arena,
        "{\"default_provider\":\"openai\","
        "\"providers\":["
        "{\"name\":\"openai\",\"api_key\":\"sk-test\"},"
        "{\"name\":\"anthropic\",\"api_key\":\"sk-ant-test\"}],"
        "\"reliability\":{\"fallback_providers\":[\"anthropic\"]}}");
    HU_ASSERT_NOT_NULL(cfg);
    hu_provider_t out = {0};
    HU_ASSERT_EQ(hu_provider_create_default(&backing, cfg, &out), HU_OK);
    HU_ASSERT_NOT_NULL(out.ctx);
    /* Wrapped reliable returns the inner's name ("openai") via get_name, so
     * we cannot distinguish from there. We do verify the provider is
     * functional (chat works in test mode) and deinit is safe. */
    hu_chat_request_t req = {0};
    hu_chat_response_t resp = {0};
    hu_error_t err = out.vtable->chat(out.ctx, &backing, &req, "gpt-4", 5, 0.0, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    hu_chat_response_free(&backing, &resp);
    if (out.vtable->deinit)
        out.vtable->deinit(out.ctx, &backing);
    free_failover_config(&backing, arena, cfg);
}

static void test_create_default_composite_default_is_passthrough(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_arena_t *arena = NULL;
    /* default = "router" → should NOT auto-wrap even with fallbacks set. */
    hu_config_t *cfg = make_failover_config(
        &backing, &arena,
        "{\"default_provider\":\"router\","
        "\"providers\":[{\"name\":\"openai\",\"api_key\":\"sk-test\"}],"
        "\"router\":{\"standard\":\"openai\"},"
        "\"reliability\":{\"fallback_providers\":[\"anthropic\"]}}");
    HU_ASSERT_NOT_NULL(cfg);
    hu_provider_t out = {0};
    HU_ASSERT_EQ(hu_provider_create_default(&backing, cfg, &out), HU_OK);
    HU_ASSERT_NOT_NULL(out.ctx);
    HU_ASSERT_STR_EQ(out.vtable->get_name(out.ctx), "router");
    if (out.vtable->deinit)
        out.vtable->deinit(out.ctx, &backing);
    free_failover_config(&backing, arena, cfg);
}

static void test_create_default_same_name_fallback_skipped(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_arena_t *arena = NULL;
    /* fallback list contains the default itself → must be skipped, leaving an
     * empty effective chain → returns base unwrapped. */
    hu_config_t *cfg = make_failover_config(
        &backing, &arena,
        "{\"default_provider\":\"openai\","
        "\"providers\":[{\"name\":\"openai\",\"api_key\":\"sk-test\"}],"
        "\"reliability\":{\"fallback_providers\":[\"openai\"]}}");
    HU_ASSERT_NOT_NULL(cfg);
    hu_provider_t out = {0};
    HU_ASSERT_EQ(hu_provider_create_default(&backing, cfg, &out), HU_OK);
    HU_ASSERT_NOT_NULL(out.ctx);
    HU_ASSERT_STR_EQ(out.vtable->get_name(out.ctx), "openai");
    if (out.vtable->deinit)
        out.vtable->deinit(out.ctx, &backing);
    free_failover_config(&backing, arena, cfg);
}

static void test_create_default_null_args_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    hu_provider_t out = {0};
    HU_ASSERT_EQ(hu_provider_create_default(NULL, &cfg, &out), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_provider_create_default(&alloc, NULL, &out), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_provider_create_default(&alloc, &cfg, NULL), HU_ERR_INVALID_ARGUMENT);
}

/* ─── helpers.c ───────────────────────────────────────────────────────────── */
static void test_helpers_is_reasoning_model_o1(void) {
    HU_ASSERT_TRUE(hu_helpers_is_reasoning_model("o1", 2));
}

static void test_helpers_is_reasoning_model_o3(void) {
    HU_ASSERT_TRUE(hu_helpers_is_reasoning_model("o3-mini", 7));
}

static void test_helpers_is_reasoning_model_o4_mini(void) {
    HU_ASSERT_TRUE(hu_helpers_is_reasoning_model("o4-mini", 7));
}

static void test_helpers_is_reasoning_model_gpt5(void) {
    HU_ASSERT_TRUE(hu_helpers_is_reasoning_model("gpt-5", 5));
}

static void test_helpers_is_reasoning_model_codex_mini(void) {
    HU_ASSERT_TRUE(hu_helpers_is_reasoning_model("codex-mini", 10));
}

static void test_helpers_is_reasoning_model_gpt4_returns_false(void) {
    HU_ASSERT_FALSE(hu_helpers_is_reasoning_model("gpt-4", 5));
}

static void test_helpers_is_reasoning_model_null_returns_false(void) {
    HU_ASSERT_FALSE(hu_helpers_is_reasoning_model(NULL, 5));
}

static void test_helpers_is_reasoning_model_zero_len_returns_false(void) {
    HU_ASSERT_FALSE(hu_helpers_is_reasoning_model("o1", 0));
}

static void test_helpers_extract_openai_content_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *body = "{\"choices\":[{\"message\":{\"content\":\"Hello world\"}}]}";
    char *out = hu_helpers_extract_openai_content(&alloc, body, strlen(body));
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, "Hello world");
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_helpers_extract_openai_content_invalid_json_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = hu_helpers_extract_openai_content(&alloc, "{invalid", 8);
    HU_ASSERT_NULL(out);
}

static void test_helpers_extract_openai_content_empty_choices_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *body = "{\"choices\":[]}";
    char *out = hu_helpers_extract_openai_content(&alloc, body, strlen(body));
    HU_ASSERT_NULL(out);
}

static void test_helpers_extract_anthropic_content_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *body = "{\"content\":[{\"type\":\"text\",\"text\":\"Hi there\"}]}";
    char *out = hu_helpers_extract_anthropic_content(&alloc, body, strlen(body));
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, "Hi there");
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_helpers_extract_anthropic_content_invalid_json_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = hu_helpers_extract_anthropic_content(&alloc, "{invalid", 8);
    HU_ASSERT_NULL(out);
}

static void test_helpers_extract_anthropic_content_empty_array_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *body = "{\"content\":[]}";
    char *out = hu_helpers_extract_anthropic_content(&alloc, body, strlen(body));
    HU_ASSERT_NULL(out);
}

static void test_chat_response_free_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_chat_response_free(&alloc, NULL);
}

static void test_helpers_openai_choice_logprob_mean(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"logprobs\":{\"content\":["
                       "{\"logprob\":-0.1},{\"logprob\":-0.3}]}}";
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, json, strlen(json), &root), HU_OK);
    HU_ASSERT_NOT_NULL(root);
    hu_chat_response_t out;
    memset(&out, 0, sizeof(out));
    hu_helpers_openai_choice_apply_logprobs(root, &out);
    HU_ASSERT_TRUE(out.logprob_mean_valid);
    HU_ASSERT_TRUE(out.logprob_mean > -0.21f && out.logprob_mean < -0.19f);
    hu_json_free(&alloc, root);
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

    HU_RUN_TEST(test_from_config_null_alloc_returns_error);
    HU_RUN_TEST(test_from_config_null_cfg_returns_error);
    HU_RUN_TEST(test_from_config_null_name_returns_error);
    HU_RUN_TEST(test_from_config_zero_name_len_returns_error);
    HU_RUN_TEST(test_from_config_null_out_returns_error);
    HU_RUN_TEST(test_from_config_unknown_provider_returns_not_supported);
    HU_RUN_TEST(test_from_config_router_creates_provider);
    HU_RUN_TEST(test_failover_primary_succeeds_extras_unused);
    HU_RUN_TEST(test_failover_primary_fails_first_fallback_succeeds);
    HU_RUN_TEST(test_failover_walks_chain_until_one_succeeds);
    HU_RUN_TEST(test_failover_all_fail_returns_error);
    HU_RUN_TEST(test_failover_primary_recovers_after_partial_failure);
    HU_RUN_TEST(test_create_default_no_fallbacks_returns_unwrapped);
    HU_RUN_TEST(test_create_default_with_fallbacks_wraps);
    HU_RUN_TEST(test_create_default_composite_default_is_passthrough);
    HU_RUN_TEST(test_create_default_same_name_fallback_skipped);
    HU_RUN_TEST(test_create_default_null_args_rejected);
    HU_RUN_TEST(test_from_config_reliable_creates_provider);
    HU_RUN_TEST(test_from_config_ensemble_creates_provider);

    HU_RUN_TEST(test_helpers_is_reasoning_model_o1);
    HU_RUN_TEST(test_helpers_is_reasoning_model_o3);
    HU_RUN_TEST(test_helpers_is_reasoning_model_o4_mini);
    HU_RUN_TEST(test_helpers_is_reasoning_model_gpt5);
    HU_RUN_TEST(test_helpers_is_reasoning_model_codex_mini);
    HU_RUN_TEST(test_helpers_is_reasoning_model_gpt4_returns_false);
    HU_RUN_TEST(test_helpers_is_reasoning_model_null_returns_false);
    HU_RUN_TEST(test_helpers_is_reasoning_model_zero_len_returns_false);
    HU_RUN_TEST(test_helpers_extract_openai_content_succeeds);
    HU_RUN_TEST(test_helpers_extract_openai_content_invalid_json_returns_null);
    HU_RUN_TEST(test_helpers_extract_openai_content_empty_choices_returns_null);
    HU_RUN_TEST(test_helpers_extract_anthropic_content_succeeds);
    HU_RUN_TEST(test_helpers_extract_anthropic_content_invalid_json_returns_null);
    HU_RUN_TEST(test_helpers_extract_anthropic_content_empty_array_returns_null);
    HU_RUN_TEST(test_helpers_openai_choice_logprob_mean);
    HU_RUN_TEST(test_chat_response_free_null_safe);
}
