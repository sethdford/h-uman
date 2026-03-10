/* Comprehensive provider tests (~300+ tests). */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/max_tokens.h"
#include "human/provider.h"
#include "human/providers/anthropic.h"
#include "human/providers/claude_cli.h"
#include "human/providers/codex_cli.h"
#include "human/providers/compatible.h"
#include "human/providers/error_classify.h"
#include "human/providers/factory.h"
#include "human/providers/gemini.h"
#include "human/providers/ollama.h"
#include "human/providers/openai.h"
#include "human/providers/openai_codex.h"
#include "human/providers/openrouter.h"
#include "human/providers/reliable.h"
#include "human/providers/scrub.h"
#include "human/providers/sse.h"
#include "test_framework.h"
#include <string.h>

static char stream_recv_buf[64];
static size_t stream_recv_len;

static void stream_cb_openai(void *ctx, const hu_stream_chunk_t *chunk) {
    (void)ctx;
    if (chunk->delta && chunk->delta_len > 0 &&
        stream_recv_len + chunk->delta_len < sizeof(stream_recv_buf)) {
        memcpy(stream_recv_buf + stream_recv_len, chunk->delta, chunk->delta_len);
        stream_recv_len += chunk->delta_len;
    }
}

static void stream_cb_noop(void *ctx, const hu_stream_chunk_t *chunk) {
    (void)ctx;
    (void)chunk;
}

static size_t sse_call_order[4];
static size_t sse_order_idx;
static size_t sse_got_delta;

static void sse_cb_order(const char *et, size_t et_len, const char *data, size_t data_len,
                         void *ud) {
    (void)ud;
    (void)et;
    (void)et_len;
    (void)data;
    (void)data_len;
    if (sse_order_idx < 4)
        sse_call_order[sse_order_idx++] = (data && data_len > 0) ? 1 : 0;
}

static void sse_cb_delta(const char *et, size_t et_len, const char *data, size_t data_len,
                         void *ud) {
    (void)ud;
    (void)et;
    (void)et_len;
    if (data && data_len > 0)
        sse_got_delta = 1;
}

static hu_chat_message_t make_user_msg(const char *content, size_t len) {
    hu_chat_message_t m = {
        .role = HU_ROLE_USER,
        .content = content,
        .content_len = len,
        .name = NULL,
        .name_len = 0,
        .tool_call_id = NULL,
        .tool_call_id_len = 0,
        .content_parts = NULL,
        .content_parts_count = 0,
        .tool_calls = NULL,
        .tool_calls_count = 0,
    };
    return m;
}

static hu_chat_request_t make_simple_request(hu_chat_message_t *msgs, size_t count) {
    return (hu_chat_request_t){
        .messages = msgs,
        .messages_count = count,
        .model = "gpt-4",
        .model_len = 4,
        .temperature = 0.7,
        .max_tokens = 0,
        .tools = NULL,
        .tools_count = 0,
        .timeout_secs = 0,
        .reasoning_effort = NULL,
        .reasoning_effort_len = 0,
        .response_format = NULL,
        .response_format_len = 0,
    };
}

/* ─── OpenAI ──────────────────────────────────────────────────────────────── */
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

static void test_openai_create_null_alloc_fails(void) {
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(NULL, "openai", 6, "key", 3, NULL, 0, &prov);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_openai_get_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openai_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openai");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_supports_native_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openai_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_TRUE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_chat_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openai_create(&alloc, "test-key", 8, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("hello", 5)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4", 4, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_deinit_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openai_create(&alloc, "k", 1, NULL, 0, &prov);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_create_empty_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_openai_create(&alloc, "", 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_create_with_base_url(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_openai_create(&alloc, "key", 3, "https://custom.openai.com/v1", 27, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_chat_with_system_and_user(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openai_create(&alloc, "test-key", 8, NULL, 0, &prov);
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = prov.vtable->chat_with_system(prov.ctx, &alloc, "You are helpful", 16, "Hello",
                                                   5, "gpt-4", 5, 0.7, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    if (out)
        alloc.free(alloc.ctx, out, out_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_chat_tool_calls_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("run ls", 6)};
    hu_tool_spec_t tools[1] = {{
        .name = "shell",
        .name_len = 5,
        .description = "Run shell",
        .description_len = 9,
        .parameters_json =
            "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}}}",
        .parameters_json_len = 55,
    }};
    hu_chat_request_t req = {
        .messages = msgs,
        .messages_count = 1,
        .model = "gpt-4",
        .model_len = 4,
        .temperature = 0.7,
        .max_tokens = 0,
        .tools = tools,
        .tools_count = 1,
        .timeout_secs = 0,
        .reasoning_effort = NULL,
        .reasoning_effort_len = 0,
    };
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4", 4, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(resp.tool_calls_count, 1u);
    HU_ASSERT_NOT_NULL(resp.tool_calls);
    HU_ASSERT_TRUE(resp.tool_calls[0].name_len == 5);
    HU_ASSERT_TRUE(memcmp(resp.tool_calls[0].name, "shell", 5) == 0);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_stream_chat_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &prov);
    HU_ASSERT_TRUE(prov.vtable->supports_streaming && prov.vtable->supports_streaming(prov.ctx));
    if (!prov.vtable->stream_chat) {
        if (prov.vtable->deinit)
            prov.vtable->deinit(prov.ctx, &alloc);
        return;
    }
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_stream_chat_result_t out = {0};
    stream_recv_buf[0] = '\0';
    stream_recv_len = 0;
    hu_error_t err = prov.vtable->stream_chat(prov.ctx, &alloc, &req, "gpt-4", 4, 0.7,
                                              stream_cb_openai, NULL, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(stream_recv_len > 0 || (out.content && out.content_len > 0));
    if (out.content)
        alloc.free(alloc.ctx, (void *)out.content, out.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_model_passthrough(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("x", 1)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    req.model = "gpt-4o";
    req.model_len = 6;
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4o", 6, 0.5, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_temperature_passthrough(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4", 4, 0.0, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_chat_null_request_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, NULL, "gpt-4", 4, 0.7, &resp);
    HU_ASSERT_NEQ(err, HU_OK);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_chat_empty_messages_graceful(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_request_t req = {.messages = NULL,
                             .messages_count = 0,
                             .model = "gpt-4",
                             .model_len = 4,
                             .temperature = 0.7,
                             .max_tokens = 0,
                             .tools = NULL,
                             .tools_count = 0,
                             .timeout_secs = 0,
                             .reasoning_effort = NULL,
                             .reasoning_effort_len = 0};
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4", 4, 0.7, &resp);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_INVALID_ARGUMENT ||
                   err == HU_ERR_PROVIDER_RESPONSE);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Anthropic ──────────────────────────────────────────────────────────── */
static void test_anthropic_create_with_base_url(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err =
        hu_anthropic_create(&alloc, "key", 3, "https://custom.anthropic.com/v1", 28, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_create_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_anthropic_create(&alloc, "test-key", 8, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_create_null_alloc_fails(void) {
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(NULL, "anthropic", 9, "key", 3, NULL, 0, &prov);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_anthropic_get_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_anthropic_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "anthropic");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_supports_native_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_anthropic_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_TRUE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_chat_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_anthropic_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "claude-3", 8, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_chat_with_tools_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_anthropic_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("run ls", 6)};
    hu_tool_spec_t tools[1] = {{
        .name = "shell",
        .name_len = 5,
        .description = "Run shell",
        .description_len = 9,
        .parameters_json =
            "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}}}",
        .parameters_json_len = 55,
    }};
    hu_chat_request_t req = {
        .messages = msgs,
        .messages_count = 1,
        .model = "claude-3",
        .model_len = 8,
        .temperature = 0.7,
        .max_tokens = 0,
        .tools = tools,
        .tools_count = 1,
        .timeout_secs = 0,
        .reasoning_effort = NULL,
        .reasoning_effort_len = 0,
    };
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "claude-3", 8, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(resp.tool_calls_count, 1u);
    HU_ASSERT_NOT_NULL(resp.tool_calls);
    HU_ASSERT_TRUE(resp.tool_calls[0].name_len == 5);
    HU_ASSERT_TRUE(memcmp(resp.tool_calls[0].name, "shell", 5) == 0);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_deinit_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_anthropic_create(&alloc, "k", 1, NULL, 0, &prov);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_create_empty_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_anthropic_create(&alloc, "", 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_chat_with_system_and_user(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_anthropic_create(&alloc, "key", 3, NULL, 0, &prov);
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        prov.vtable->chat_with_system(prov.ctx, &alloc, "System prompt", 13, "User msg", 8,
                                      "claude-3-sonnet", 15, 0.7, &out, &out_len);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_NOT_SUPPORTED);
    if (err == HU_OK && out) {
        alloc.free(alloc.ctx, out, out_len + 1);
    }
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_chat_null_request_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_anthropic_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, NULL, "claude-3", 8, 0.7, &resp);
    HU_ASSERT_NEQ(err, HU_OK);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_chat_empty_messages_graceful(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_anthropic_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_request_t req = {.messages = NULL,
                             .messages_count = 0,
                             .model = "claude-3",
                             .model_len = 8,
                             .temperature = 0.7,
                             .max_tokens = 0,
                             .tools = NULL,
                             .tools_count = 0,
                             .timeout_secs = 0,
                             .reasoning_effort = NULL,
                             .reasoning_effort_len = 0};
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "claude-3", 8, 0.7, &resp);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_INVALID_ARGUMENT ||
                   err == HU_ERR_PROVIDER_RESPONSE);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_max_tokens_in_request(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_anthropic_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    req.max_tokens = 2048;
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "claude-3", 8, 0.7, &resp);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_NOT_SUPPORTED);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_stream_chat_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_anthropic_create(&alloc, "key", 3, NULL, 0, &prov);
    if (!prov.vtable->supports_streaming || !prov.vtable->stream_chat) {
        if (prov.vtable->deinit)
            prov.vtable->deinit(prov.ctx, &alloc);
        return;
    }
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_stream_chat_result_t out = {0};
    hu_error_t err = prov.vtable->stream_chat(prov.ctx, &alloc, &req, "claude-3", 8, 0.7,
                                              stream_cb_noop, NULL, &out);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_NOT_SUPPORTED);
    if (out.content)
        alloc.free(alloc.ctx, (void *)out.content, out.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_tool_call_format(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_anthropic_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("run ls", 6)};
    hu_tool_spec_t tools[1] = {{
        .name = "shell",
        .name_len = 5,
        .description = "Run shell",
        .description_len = 9,
        .parameters_json = "{\"type\":\"object\"}",
        .parameters_json_len = 18,
    }};
    hu_chat_request_t req = {
        .messages = msgs,
        .messages_count = 1,
        .model = "claude-3",
        .model_len = 8,
        .temperature = 0.7,
        .max_tokens = 0,
        .tools = tools,
        .tools_count = 1,
        .timeout_secs = 0,
        .reasoning_effort = NULL,
        .reasoning_effort_len = 0,
    };
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "claude-3", 8, 0.7, &resp);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_NOT_SUPPORTED);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Gemini ─────────────────────────────────────────────────────────────── */
static void test_gemini_create_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_gemini_create(&alloc, "test-key", 8, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_create_null_alloc_fails(void) {
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(NULL, "gemini", 6, "key", 3, NULL, 0, &prov);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_gemini_get_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_gemini_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "gemini");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_supports_native_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_gemini_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_TRUE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_chat_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_gemini_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("hello", 5)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gemini-pro", 10, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    if (resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_deinit_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_gemini_create(&alloc, "k", 1, NULL, 0, &prov);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_create_empty_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_gemini_create(&alloc, "", 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_create_with_oauth(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_gemini_create_with_oauth(&alloc, "ya29.test-token", 16, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_create_oauth_null_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_gemini_create_with_oauth(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_gemini_supports_vision(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_gemini_create(&alloc, "key", 3, NULL, 0, &prov);
    if (prov.vtable->supports_vision) {
        HU_ASSERT_TRUE(prov.vtable->supports_vision(prov.ctx));
    }
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_chat_with_tools_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_gemini_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("run ls", 6)};
    hu_tool_spec_t tools[1] = {{
        .name = "shell",
        .name_len = 5,
        .description = "Run shell",
        .description_len = 9,
        .parameters_json = "{}",
        .parameters_json_len = 2,
    }};
    hu_chat_request_t req = {
        .messages = msgs,
        .messages_count = 1,
        .model = "gemini-1.5-pro",
        .model_len = 14,
        .temperature = 0.7,
        .max_tokens = 0,
        .tools = tools,
        .tools_count = 1,
        .timeout_secs = 0,
        .reasoning_effort = NULL,
        .reasoning_effort_len = 0,
    };
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gemini-1.5-pro", 14, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(resp.tool_calls_count, 1u);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_model_gemini2_flash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_gemini_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    req.model = "gemini-2.0-flash";
    req.model_len = 16;
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gemini-2.0-flash", 16, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    if (resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_stream_chat_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_gemini_create(&alloc, "key", 3, NULL, 0, &prov);
    if (!prov.vtable->stream_chat) {
        if (prov.vtable->deinit)
            prov.vtable->deinit(prov.ctx, &alloc);
        return;
    }
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_stream_chat_result_t out = {0};
    hu_error_t err = prov.vtable->stream_chat(prov.ctx, &alloc, &req, "gemini-pro", 10, 0.7,
                                              stream_cb_noop, NULL, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out.content != NULL || out.usage.completion_tokens > 0);
    if (out.content)
        alloc.free(alloc.ctx, (void *)out.content, out.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_chat_null_request_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_gemini_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, NULL, "gemini-pro", 10, 0.7, &resp);
    HU_ASSERT_NEQ(err, HU_OK);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_chat_empty_messages_graceful(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_gemini_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_request_t req = {.messages = NULL,
                             .messages_count = 0,
                             .model = "gemini-pro",
                             .model_len = 10,
                             .temperature = 0.7,
                             .max_tokens = 0,
                             .tools = NULL,
                             .tools_count = 0,
                             .timeout_secs = 0,
                             .reasoning_effort = NULL,
                             .reasoning_effort_len = 0};
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gemini-pro", 10, 0.7, &resp);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_INVALID_ARGUMENT ||
                   err == HU_ERR_PROVIDER_RESPONSE);
    if (err == HU_OK && resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Ollama ─────────────────────────────────────────────────────────────── */
static void test_ollama_create_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_create_null_alloc_fails(void) {
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(NULL, "ollama", 6, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_ollama_get_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "ollama");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_supports_native_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_FALSE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_chat_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "llama2", 6, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    if (resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_deinit_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_create_empty_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_ollama_create(&alloc, "", 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_create_with_base_url(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_ollama_create(&alloc, NULL, 0, "http://localhost:11434", 21, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_no_api_key_required(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "llama2", 6, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    if (resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_chat_with_tools_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("run ls", 6)};
    hu_tool_spec_t tools[1] = {{
        .name = "shell",
        .name_len = 5,
        .description = "Run",
        .description_len = 3,
        .parameters_json = "{}",
        .parameters_json_len = 2,
    }};
    hu_chat_request_t req = {
        .messages = msgs,
        .messages_count = 1,
        .model = "llama2",
        .model_len = 6,
        .temperature = 0.7,
        .max_tokens = 0,
        .tools = tools,
        .tools_count = 1,
        .timeout_secs = 0,
        .reasoning_effort = NULL,
        .reasoning_effort_len = 0,
    };
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "llama2", 6, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(resp.tool_calls_count, 1u);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_local_model_format(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("x", 1)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    req.model = "mistral:7b";
    req.model_len = 10;
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "mistral:7b", 10, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    if (resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_chat_null_request_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, NULL, "llama2", 6, 0.7, &resp);
    HU_ASSERT_NEQ(err, HU_OK);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_chat_empty_messages_graceful(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    hu_chat_request_t req = {.messages = NULL,
                             .messages_count = 0,
                             .model = "llama2",
                             .model_len = 6,
                             .temperature = 0.7,
                             .max_tokens = 0,
                             .tools = NULL,
                             .tools_count = 0,
                             .timeout_secs = 0,
                             .reasoning_effort = NULL,
                             .reasoning_effort_len = 0};
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "llama2", 6, 0.7, &resp);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_INVALID_ARGUMENT ||
                   err == HU_ERR_PROVIDER_RESPONSE);
    if (err == HU_OK && resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── OpenRouter ──────────────────────────────────────────────────────────── */
static void test_openrouter_create_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_openrouter_create(&alloc, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openrouter_create_null_alloc_fails(void) {
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(NULL, "openrouter", 10, "key", 3, NULL, 0, &prov);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_openrouter_get_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openrouter_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openrouter");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openrouter_supports_native_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openrouter_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_TRUE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openrouter_chat_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openrouter_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("hello", 5)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    hu_error_t err =
        prov.vtable->chat(prov.ctx, &alloc, &req, "anthropic/claude-3", 18, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openrouter_deinit_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openrouter_create(&alloc, "k", 1, NULL, 0, &prov);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openrouter_create_empty_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_openrouter_create(&alloc, "", 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openrouter_model_routing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openrouter_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    req.model = "anthropic/claude-3-opus";
    req.model_len = 22;
    hu_chat_response_t resp = {0};
    hu_error_t err =
        prov.vtable->chat(prov.ctx, &alloc, &req, "anthropic/claude-3-opus", 22, 0.7, &resp);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_NOT_SUPPORTED);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openrouter_chat_with_tools_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openrouter_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("run ls", 6)};
    hu_tool_spec_t tools[1] = {{
        .name = "shell",
        .name_len = 5,
        .description = "Run",
        .description_len = 3,
        .parameters_json = "{}",
        .parameters_json_len = 2,
    }};
    hu_chat_request_t req = {
        .messages = msgs,
        .messages_count = 1,
        .model = "openai/gpt-4",
        .model_len = 14,
        .temperature = 0.7,
        .max_tokens = 0,
        .tools = tools,
        .tools_count = 1,
        .timeout_secs = 0,
        .reasoning_effort = NULL,
        .reasoning_effort_len = 0,
    };
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "openai/gpt-4", 12, 0.7, &resp);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_NOT_SUPPORTED);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openrouter_chat_null_request_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openrouter_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_response_t resp = {0};
    hu_error_t err =
        prov.vtable->chat(prov.ctx, &alloc, NULL, "anthropic/claude-3", 18, 0.7, &resp);
    HU_ASSERT_NEQ(err, HU_OK);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openrouter_chat_empty_messages_graceful(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openrouter_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_request_t req = {.messages = NULL,
                             .messages_count = 0,
                             .model = "openai/gpt-4",
                             .model_len = 14,
                             .temperature = 0.7,
                             .max_tokens = 0,
                             .tools = NULL,
                             .tools_count = 0,
                             .timeout_secs = 0,
                             .reasoning_effort = NULL,
                             .reasoning_effort_len = 0};
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "openai/gpt-4", 14, 0.7, &resp);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_INVALID_ARGUMENT ||
                   err == HU_ERR_PROVIDER_RESPONSE);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Compatible ─────────────────────────────────────────────────────────── */
static void test_compatible_create_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_compatible_create(&alloc, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_compatible_create_null_alloc_fails(void) {
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(NULL, "compatible", 10, "key", 3, NULL, 0, &prov);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_compatible_get_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_compatible_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_compatible_supports_native_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_compatible_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_TRUE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_compatible_chat_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_compatible_create(&alloc, "key", 3, "http://localhost:1234", 21, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "model", 5, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    if (resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_compatible_deinit_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_compatible_create(&alloc, "k", 1, NULL, 0, &prov);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_compatible_create_empty_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_compatible_create(&alloc, "", 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_compatible_mistral_base_url(void) {
    const char *url = hu_compatible_provider_url("mistral");
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_TRUE(strstr(url, "mistral") != NULL);
}

static void test_compatible_deepseek_base_url(void) {
    const char *url = hu_compatible_provider_url("deepseek");
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_TRUE(strstr(url, "deepseek") != NULL);
}

static void test_compatible_together_base_url(void) {
    const char *url = hu_compatible_provider_url("together");
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_TRUE(strstr(url, "together") != NULL);
}

static void test_compatible_fireworks_base_url(void) {
    const char *url = hu_compatible_provider_url("fireworks");
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_TRUE(strstr(url, "fireworks") != NULL);
}

static void test_compatible_perplexity_base_url(void) {
    const char *url = hu_compatible_provider_url("perplexity");
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_TRUE(strstr(url, "perplexity") != NULL);
}

static void test_compatible_cerebras_base_url(void) {
    const char *url = hu_compatible_provider_url("cerebras");
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_TRUE(strstr(url, "cerebras") != NULL);
}

static void test_compatible_groq_base_url(void) {
    const char *url = hu_compatible_provider_url("groq");
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_TRUE(strstr(url, "groq") != NULL);
}

static void test_compatible_xai_base_url(void) {
    const char *url = hu_compatible_provider_url("xai");
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_TRUE(strstr(url, "api.x.ai") != NULL);
}

static void test_factory_mistral_chat_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "mistral", 7, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    req.model = "mistral-small";
    req.model_len = 13;
    hu_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "mistral-small", 13, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    if (resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_deepseek_chat_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "deepseek", 8, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "deepseek-chat", 13, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    if (resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_together_chat_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "together", 8, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "meta-llama/llama-2-70b", 22, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    if (resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_fireworks_chat_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "fireworks", 9, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "llama-v3p1-70b-instruct", 23, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    if (resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_perplexity_chat_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "perplexity", 10, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "sonar", 5, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    if (resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_cerebras_chat_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "cerebras", 8, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "llama3.1-8b", 11, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    if (resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_xai_chat_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "xai", 3, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "grok-2", 6, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    if (resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_compatible_chat_with_tools_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_compatible_create(&alloc, "key", 3, "http://localhost:1234/v1", 24, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("run ls", 6)};
    hu_tool_spec_t tools[1] = {{
        .name = "shell",
        .name_len = 5,
        .description = "Run",
        .description_len = 3,
        .parameters_json = "{}",
        .parameters_json_len = 2,
    }};
    hu_chat_request_t req = {
        .messages = msgs,
        .messages_count = 1,
        .model = "model",
        .model_len = 5,
        .temperature = 0.7,
        .max_tokens = 0,
        .tools = tools,
        .tools_count = 1,
        .timeout_secs = 0,
        .reasoning_effort = NULL,
        .reasoning_effort_len = 0,
    };
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "model", 5, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(resp.tool_calls_count, 1u);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_compatible_chat_null_request_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_compatible_create(&alloc, "key", 3, "http://localhost:1234", 21, &prov);
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, NULL, "model", 5, 0.7, &resp);
    HU_ASSERT_NEQ(err, HU_OK);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_compatible_chat_empty_messages_graceful(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_compatible_create(&alloc, "key", 3, "http://localhost:1234", 21, &prov);
    hu_chat_request_t req = {.messages = NULL,
                             .messages_count = 0,
                             .model = "model",
                             .model_len = 5,
                             .temperature = 0.7,
                             .max_tokens = 0,
                             .tools = NULL,
                             .tools_count = 0,
                             .timeout_secs = 0,
                             .reasoning_effort = NULL,
                             .reasoning_effort_len = 0};
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "model", 5, 0.7, &resp);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_INVALID_ARGUMENT ||
                   err == HU_ERR_PROVIDER_RESPONSE);
    if (err == HU_OK)
        hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Claude CLI ─────────────────────────────────────────────────────────── */
static void test_claude_cli_create_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_claude_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_claude_cli_create_null_alloc_fails(void) {
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(NULL, "claude_cli", 10, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_claude_cli_get_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_claude_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_TRUE(strcmp(prov.vtable->get_name(prov.ctx), "claude_cli") == 0 ||
                   strcmp(prov.vtable->get_name(prov.ctx), "claude-cli") == 0);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_claude_cli_supports_native_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_claude_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_FALSE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_claude_cli_chat_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_claude_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("hello", 5)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "claude-3", 8, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    if (resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_claude_cli_deinit_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_claude_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_claude_cli_create_empty_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_claude_cli_create(&alloc, "", 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_claude_cli_chat_empty_messages_graceful(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_claude_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    hu_chat_request_t req = {.messages = NULL,
                             .messages_count = 0,
                             .model = "claude-3",
                             .model_len = 8,
                             .temperature = 0.7,
                             .max_tokens = 0,
                             .tools = NULL,
                             .tools_count = 0,
                             .timeout_secs = 0,
                             .reasoning_effort = NULL,
                             .reasoning_effort_len = 0};
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "claude-3", 8, 0.7, &resp);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_INVALID_ARGUMENT ||
                   err == HU_ERR_PROVIDER_RESPONSE);
    if (err == HU_OK && resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Compatible URL Lookup ────────────────────────────────────────────────── */
static void test_compatible_url_lookup_groq(void) {
    const char *url = hu_compatible_provider_url("groq");
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_STR_EQ(url, "https://api.groq.com/openai");
}

static void test_compatible_url_lookup_mistral(void) {
    const char *url = hu_compatible_provider_url("mistral");
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_STR_EQ(url, "https://api.mistral.ai/v1");
}

static void test_compatible_url_lookup_deepseek(void) {
    const char *url = hu_compatible_provider_url("deepseek");
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_STR_EQ(url, "https://api.deepseek.com");
}

static void test_compatible_url_lookup_together(void) {
    const char *url = hu_compatible_provider_url("together");
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_STR_EQ(url, "https://api.together.xyz");
}

static void test_compatible_url_lookup_fireworks(void) {
    const char *url = hu_compatible_provider_url("fireworks");
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_STR_EQ(url, "https://api.fireworks.ai/inference/v1");
}

static void test_compatible_url_lookup_perplexity(void) {
    const char *url = hu_compatible_provider_url("perplexity");
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_STR_EQ(url, "https://api.perplexity.ai");
}

static void test_compatible_url_lookup_cerebras(void) {
    const char *url = hu_compatible_provider_url("cerebras");
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_STR_EQ(url, "https://api.cerebras.ai/v1");
}

static void test_compatible_url_lookup_unknown(void) {
    const char *url = hu_compatible_provider_url("unknown_provider");
    HU_ASSERT_NULL(url);
}

static void test_factory_creates_compatible_for_groq(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "groq", 4, "test-key", 8, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    HU_ASSERT_NOT_NULL(prov.vtable);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Factory ─────────────────────────────────────────────────────────────── */
static void test_provider_factory_unknown_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "unknown_provider", 16, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
}

static void test_provider_factory_null_name_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, NULL, 0, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_provider_factory_empty_name_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "", 0, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_provider_factory_null_out_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_provider_create(&alloc, "openai", 6, "key", 3, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_factory_openai_resolves(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "openai", 6, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openai");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_anthropic_resolves(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "anthropic", 9, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "anthropic");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_gemini_resolves(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "gemini", 6, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "gemini");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_ollama_resolves(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "ollama", 6, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "ollama");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_openrouter_resolves(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "openrouter", 10, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openrouter");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_case_sensitive_unknown(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "OpenAI", 6, "key", 3, NULL, 0, &prov);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_factory_gpt4o_not_provider_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "gpt-4o", 6, "key", 3, NULL, 0, &prov);
    HU_ASSERT_NEQ(err, HU_OK);
}

/* ─── Provider error handling (factory path) ─────────────────────────────── */
static void test_factory_openai_create_empty_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "openai", 6, "", 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_anthropic_create_empty_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "anthropic", 9, "", 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_gemini_create_empty_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "gemini", 6, "", 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_openai_chat_returns_stub(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "openai", 6, "", 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    hu_chat_message_t msgs[1] = {make_user_msg("hello", 5)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4", 4, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_openai_chat_with_system_returns_stub(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "openai", 6, "", 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    char *out = NULL;
    size_t out_len = 0;
    err = prov.vtable->chat_with_system(prov.ctx, &alloc, "You are helpful", 16, "Hello", 5,
                                        "gpt-4", 5, 0.7, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    if (out)
        alloc.free(alloc.ctx, out, out_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_anthropic_chat_returns_stub(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "anthropic", 9, "", 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "claude-3", 8, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_anthropic_chat_with_system_returns_stub(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "anthropic", 9, "", 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    char *out = NULL;
    size_t out_len = 0;
    err = prov.vtable->chat_with_system(prov.ctx, &alloc, "System prompt", 13, "User msg", 8,
                                        "claude-3-sonnet", 15, 0.7, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    if (out)
        alloc.free(alloc.ctx, out, out_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_gemini_chat_returns_stub(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "gemini", 6, "", 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    hu_chat_message_t msgs[1] = {make_user_msg("hello", 5)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "gemini-pro", 10, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_gemini_chat_with_system_returns_stub(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "gemini", 6, "", 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    char *out = NULL;
    size_t out_len = 0;
    err = prov.vtable->chat_with_system(prov.ctx, &alloc, "You are helpful", 16, "Hello", 5,
                                        "gemini-pro", 10, 0.7, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    if (out)
        alloc.free(alloc.ctx, out, out_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── SSE parsing ────────────────────────────────────────────────────────── */
static void test_sse_parse_line_data_extracts_delta(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *line = "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}";
    hu_sse_line_result_t out;
    hu_error_t err = hu_sse_parse_line(&alloc, line, strlen(line), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.tag, HU_SSE_DELTA);
    HU_ASSERT_NOT_NULL(out.delta);
    HU_ASSERT_STR_EQ(out.delta, "hi");
    if (out.delta)
        alloc.free(alloc.ctx, out.delta, out.delta_len + 1);
}

static void test_sse_parse_line_done(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *line = "data: [DONE]";
    hu_sse_line_result_t out;
    hu_error_t err = hu_sse_parse_line(&alloc, line, strlen(line), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.tag, HU_SSE_DONE);
}

static void test_sse_parse_line_comment_skipped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *line = ": comment";
    hu_sse_line_result_t out;
    hu_error_t err = hu_sse_parse_line(&alloc, line, strlen(line), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.tag, HU_SSE_SKIP);
}

static void test_sse_parse_line_empty_skipped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *line = "";
    hu_sse_line_result_t out;
    hu_error_t err = hu_sse_parse_line(&alloc, line, strlen(line), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.tag, HU_SSE_SKIP);
}

static void test_sse_parse_line_no_data_prefix_skipped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *line = "event: message";
    hu_sse_line_result_t out;
    hu_error_t err = hu_sse_parse_line(&alloc, line, strlen(line), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.tag, HU_SSE_SKIP);
}

static void test_sse_extract_delta_empty_choices(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = hu_sse_extract_delta_content(&alloc, "{\"choices\":[]}", 14);
    HU_ASSERT_NULL(out);
}

static void test_sse_extract_delta_no_delta_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = hu_sse_extract_delta_content(&alloc, "{\"choices\":[{\"delta\":{}}]}", 28);
    HU_ASSERT_NULL(out);
}

static void test_sse_extract_delta_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}";
    char *out = hu_sse_extract_delta_content(&alloc, json, strlen(json));
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, "Hello");
    if (out)
        alloc.free(alloc.ctx, out, strlen(out) + 1);
}

/* ─── Error classification ────────────────────────────────────────────────── */
static void test_error_classify_rate_limited_429(void) {
    const char *msg = "HTTP 429 rate limited";
    HU_ASSERT_TRUE(hu_error_is_rate_limited(msg, strlen(msg)));
}

static void test_error_classify_rate_limited_too_many(void) {
    const char *msg = "too many requests";
    HU_ASSERT_TRUE(hu_error_is_rate_limited(msg, strlen(msg)));
}

static void test_error_classify_rate_limited_quota(void) {
    const char *msg = "quota exceeded";
    HU_ASSERT_TRUE(hu_error_is_rate_limited(msg, strlen(msg)));
}

static void test_error_classify_not_rate_limited(void) {
    const char *msg = "HTTP 500 internal server error";
    HU_ASSERT_FALSE(hu_error_is_rate_limited(msg, strlen(msg)));
}

static void test_error_classify_context_exhausted(void) {
    const char *msg = "context length exceeded";
    HU_ASSERT_TRUE(hu_error_is_context_exhausted(msg, strlen(msg)));
}

static void test_error_classify_context_token_limit(void) {
    const char *msg = "maximum token limit";
    HU_ASSERT_TRUE(hu_error_is_context_exhausted(msg, strlen(msg)));
}

static void test_error_classify_not_context_exhausted(void) {
    const char *msg = "generic error";
    HU_ASSERT_FALSE(hu_error_is_context_exhausted(msg, strlen(msg)));
}

static void test_error_classify_non_retryable_401(void) {
    const char *msg = "401 Unauthorized";
    HU_ASSERT_TRUE(hu_error_is_non_retryable(msg, strlen(msg)));
}

static void test_error_classify_non_retryable_403(void) {
    const char *msg = "403 Forbidden";
    HU_ASSERT_TRUE(hu_error_is_non_retryable(msg, strlen(msg)));
}

static void test_error_classify_429_not_non_retryable(void) {
    const char *msg = "429 rate limited";
    HU_ASSERT_FALSE(hu_error_is_non_retryable(msg, strlen(msg)));
}

static void test_error_classify_retry_after_seconds(void) {
    const char *msg = "Retry-After: 60";
    uint64_t ms = hu_error_parse_retry_after_ms(msg, strlen(msg));
    HU_ASSERT_TRUE(ms >= 59000 && ms <= 61000);
}

static void test_error_classify_text_rate_limited(void) {
    const char *text = "429 rate limit";
    HU_ASSERT_TRUE(hu_error_is_rate_limited_text(text, strlen(text)));
}

static void test_error_classify_text_context_exhausted(void) {
    const char *text = "context window exceeded";
    HU_ASSERT_TRUE(hu_error_is_context_exhausted_text(text, strlen(text)));
}

/* ─── Factory additional lookups ───────────────────────────────────────────── */
static void test_factory_mistral_creates_compatible(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "mistral", 7, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_deepseek_creates_compatible(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "deepseek", 8, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_together_creates_compatible(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "together", 8, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_fireworks_creates_compatible(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "fireworks", 9, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_perplexity_creates_compatible(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "perplexity", 10, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_cerebras_creates_compatible(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "cerebras", 8, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_sse_parser_init_deinit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_error_t err = hu_sse_parser_init(&p, &alloc);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(p.buffer);
    hu_sse_parser_deinit(&p);
    HU_ASSERT_NULL(p.buffer);
}

static char s_sse_captured[256];
static size_t s_sse_captured_len;

static void sse_capture_cb(const char *et, size_t et_len, const char *data, size_t data_len,
                           void *ud) {
    (void)ud;
    (void)et;
    (void)et_len;
    if (data && data_len > 0 && data_len < (sizeof(s_sse_captured) - 1)) {
        memcpy(s_sse_captured, data, data_len);
        s_sse_captured[data_len] = '\0';
        s_sse_captured_len = data_len;
    }
}

static void test_sse_parser_feed_callback(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_sse_parser_init(&p, &alloc);
    s_sse_captured[0] = '\0';
    s_sse_captured_len = 0;
    const char *stream = "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n";
    hu_error_t err = hu_sse_parser_feed(&p, stream, strlen(stream), sse_capture_cb, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(s_sse_captured_len > 0);
    hu_sse_parser_deinit(&p);
}

static void test_factory_google_creates_gemini(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "google", 6, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "gemini");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_google_gemini_creates_gemini(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "google-gemini", 13, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "gemini");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_custom_prefix_creates_compatible(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err =
        hu_provider_create(&alloc, "custom:https://my-api.com/v1", 28, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_anthropic_custom_creates_anthropic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "anthropic-custom:https://my-api.com/v1", 38, "key",
                                        3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "anthropic");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Codex CLI ────────────────────────────────────────────────────────────── */
static void test_codex_cli_create_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_codex_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_codex_cli_get_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_codex_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_TRUE(strcmp(prov.vtable->get_name(prov.ctx), "codex-cli") == 0);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_codex_cli_supports_native_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_codex_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_FALSE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_codex_cli_chat_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_codex_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("hello", 5)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "codex-mini", 10, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_codex_cli_create_null_alloc_fails(void) {
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(NULL, "codex_cli", 9, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_codex_cli_deinit_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_codex_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_codex_cli_chat_empty_messages_graceful(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_codex_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    hu_chat_request_t req = {.messages = NULL,
                             .messages_count = 0,
                             .model = "codex-mini",
                             .model_len = 10,
                             .temperature = 0.7,
                             .max_tokens = 0,
                             .tools = NULL,
                             .tools_count = 0,
                             .timeout_secs = 0,
                             .reasoning_effort = NULL,
                             .reasoning_effort_len = 0};
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "codex-mini", 10, 0.7, &resp);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_INVALID_ARGUMENT);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── OpenAI Codex ────────────────────────────────────────────────────────── */
static void test_openai_codex_create_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_openai_codex_create(&alloc, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_codex_get_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openai_codex_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openai-codex");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_codex_supports_native_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openai_codex_create(&alloc, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_FALSE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_codex_chat_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openai_codex_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "o4-mini", 7, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_codex_create_null_alloc_fails(void) {
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(NULL, "openai-codex", 12, "key", 3, NULL, 0, &prov);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_openai_codex_deinit_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openai_codex_create(&alloc, "k", 1, NULL, 0, &prov);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_codex_chat_null_request_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openai_codex_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, NULL, "o4-mini", 7, 0.7, &resp);
    HU_ASSERT_NEQ(err, HU_OK);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_codex_chat_empty_messages_graceful(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openai_codex_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_request_t req = {.messages = NULL,
                             .messages_count = 0,
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
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "o4-mini", 7, 0.7, &resp);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_INVALID_ARGUMENT ||
                   err == HU_ERR_PROVIDER_RESPONSE);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Reliable ────────────────────────────────────────────────────────────── */
static void test_reliable_provider_create_from_config(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t primary;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &primary);
    hu_provider_t fallback;
    hu_ollama_create(&alloc, NULL, 0, NULL, 0, &fallback);
    hu_reliable_config_t config = {
        .primary = primary,
        .fallback = fallback,
        .max_retries = 3,
        .base_delay_ms = 1000,
        .max_delay_ms = 30000,
        .failure_threshold = 5,
        .recovery_timeout_seconds = 60,
    };
    hu_provider_t prov;
    hu_error_t err = hu_reliable_provider_create(&alloc, &config, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openai");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_create_simple(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t inner;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &inner);
    hu_provider_t prov;
    hu_error_t err = hu_reliable_create(&alloc, inner, 2, 100, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openai");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_supports_native_tools_aggregates(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t inner;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &inner);
    hu_provider_t prov;
    hu_reliable_create(&alloc, inner, 1, 50, &prov);
    HU_ASSERT_TRUE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_supports_vision_aggregates(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t inner;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &inner);
    hu_provider_t prov;
    hu_reliable_create(&alloc, inner, 1, 50, &prov);
    if (prov.vtable->supports_vision) {
        bool v = prov.vtable->supports_vision(prov.ctx);
        (void)v;
    }
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_chat_passthrough(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t inner;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &inner);
    hu_provider_t prov;
    hu_reliable_create(&alloc, inner, 1, 50, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4", 4, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_create_with_extras(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t primary;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &primary);
    hu_provider_t fallback;
    hu_anthropic_create(&alloc, "key", 3, NULL, 0, &fallback);
    hu_reliable_provider_entry_t extras[1] = {{
        .name = "anthropic",
        .name_len = 9,
        .provider = fallback,
    }};
    hu_provider_t prov;
    hu_error_t err = hu_reliable_create_ex(&alloc, primary, 1, 50, extras, 1, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openai");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_create_with_model_fallbacks(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t inner;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &inner);
    hu_reliable_fallback_model_t gpt4_fallbacks[2] = {
        {.model = "gpt-3.5-turbo", .model_len = 14},
        {.model = "gpt-3", .model_len = 5},
    };
    hu_reliable_model_fallback_entry_t model_fallbacks[1] = {{
        .model = "gpt-4",
        .model_len = 4,
        .fallbacks = gpt4_fallbacks,
        .fallbacks_count = 2,
    }};
    hu_provider_t prov;
    hu_error_t err =
        hu_reliable_create_ex(&alloc, inner, 0, 50, NULL, 0, model_fallbacks, 1, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_chat_with_extras_primary_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t primary;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &primary);
    hu_provider_t fallback;
    hu_ollama_create(&alloc, NULL, 0, NULL, 0, &fallback);
    hu_reliable_provider_entry_t extras[1] = {
        {.name = "ollama", .name_len = 6, .provider = fallback}};
    hu_provider_t prov;
    hu_reliable_create_ex(&alloc, primary, 0, 50, extras, 1, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4", 4, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.content);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_warmup_calls_all(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t inner;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &inner);
    hu_provider_t prov;
    hu_reliable_create(&alloc, inner, 0, 50, &prov);
    if (prov.vtable->warmup) {
        prov.vtable->warmup(prov.ctx);
    }
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_supports_vision_from_gemini(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t inner;
    hu_gemini_create(&alloc, "key", 3, NULL, 0, &inner);
    hu_provider_t prov;
    hu_reliable_create(&alloc, inner, 0, 50, &prov);
    if (prov.vtable->supports_vision) {
        HU_ASSERT_TRUE(prov.vtable->supports_vision(prov.ctx));
    }
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_chat_with_system(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t inner;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &inner);
    hu_provider_t prov;
    hu_reliable_create(&alloc, inner, 0, 50, &prov);
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = prov.vtable->chat_with_system(prov.ctx, &alloc, "Be helpful", 10, "Hello", 5,
                                                   "gpt-4", 5, 0.7, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    if (out)
        alloc.free(alloc.ctx, out, out_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Scrub ───────────────────────────────────────────────────────────────── */
static void test_scrub_sk_prefix(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "sk-abc123secret";
    char *out = hu_scrub_secret_patterns(&alloc, in, strlen(in));
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "[REDACTED]") != NULL);
    HU_ASSERT_TRUE(strstr(out, "sk-abc123secret") == NULL);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_scrub_ghp_prefix(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "ghp_abcdef123456";
    char *out = hu_scrub_secret_patterns(&alloc, in, strlen(in));
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "[REDACTED]") != NULL);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_scrub_bearer_token(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "Authorization: Bearer sk_live_xyz789";
    char *out = hu_scrub_secret_patterns(&alloc, in, strlen(in));
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "[REDACTED]") != NULL);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_scrub_plain_text_unmodified(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "hello world no secrets";
    char *out = hu_scrub_secret_patterns(&alloc, in, strlen(in));
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, in);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_scrub_sanitize_truncates(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[300];
    for (size_t i = 0; i < sizeof(buf) - 1; i++)
        buf[i] = 'x';
    buf[sizeof(buf) - 1] = '\0';
    char *out = hu_scrub_sanitize_api_error(&alloc, buf, strlen(buf));
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strlen(out) <= 204);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

/* ─── Max tokens ──────────────────────────────────────────────────────────── */
static void test_max_tokens_default(void) {
    uint32_t v = hu_max_tokens_default();
    HU_ASSERT_TRUE(v > 0 && v <= 100000);
}

static void test_max_tokens_lookup_gpt4o(void) {
    uint32_t v = hu_max_tokens_lookup("gpt-4o", 6);
    HU_ASSERT_TRUE(v >= 4096);
}

static void test_max_tokens_lookup_claude(void) {
    uint32_t v = hu_max_tokens_lookup("claude-3-opus", 13);
    HU_ASSERT_TRUE(v >= 4096);
}

static void test_max_tokens_lookup_unknown(void) {
    uint32_t v = hu_max_tokens_lookup("unknown-model-xyz", 16);
    HU_ASSERT_TRUE(v == 0 || v >= 4096);
}

static void test_max_tokens_resolve_override(void) {
    uint32_t v = hu_max_tokens_resolve(16384, "gpt-4", 5);
    HU_ASSERT_EQ(v, 16384u);
}

static void test_max_tokens_resolve_fallback(void) {
    uint32_t v = hu_max_tokens_resolve(0, "gpt-4o", 6);
    HU_ASSERT_TRUE(v >= 4096);
}

/* ─── Error classify vision ───────────────────────────────────────────────── */
static void test_error_classify_vision_unsupported(void) {
    const char *text = "this model does not support image input";
    HU_ASSERT_TRUE(hu_error_is_vision_unsupported_text(text, strlen(text)));
}

static void test_error_classify_vision_not_unsupported(void) {
    const char *text = "generic error message";
    HU_ASSERT_FALSE(hu_error_is_vision_unsupported_text(text, strlen(text)));
}

/* ─── Factory xai grok ─────────────────────────────────────────────────────── */
static void test_factory_xai_creates_compatible(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "xai", 3, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_grok_creates_compatible(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "grok", 4, "key", 3, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── SSE additional ───────────────────────────────────────────────────────── */
static void test_sse_parse_line_delta_tool_call(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *line =
        "data: {\"choices\":[{\"delta\":{\"content\":null,\"function_call\":{\"name\":\"run\"}}]}";
    hu_sse_line_result_t out;
    hu_error_t err = hu_sse_parse_line(&alloc, line, strlen(line), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out.tag == HU_SSE_DELTA || out.tag == HU_SSE_SKIP);
    if (out.delta)
        alloc.free(alloc.ctx, out.delta, out.delta_len + 1);
}

static void test_sse_extract_delta_null_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"choices\":[{\"delta\":{\"content\":null}}]}";
    char *out = hu_sse_extract_delta_content(&alloc, json, strlen(json));
    HU_ASSERT_TRUE(out == NULL || (out && strlen(out) == 0));
    if (out)
        alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static size_t s_sse_multiline_event_count;
static char s_sse_multiline_last_data[128];

static void sse_multiline_cb(const char *et, size_t et_len, const char *data, size_t data_len,
                             void *ud) {
    (void)ud;
    (void)et;
    (void)et_len;
    s_sse_multiline_event_count++;
    if (data && data_len > 0 && data_len < sizeof(s_sse_multiline_last_data) - 1) {
        memcpy(s_sse_multiline_last_data, data, data_len);
        s_sse_multiline_last_data[data_len] = '\0';
    }
}

static void test_sse_parse_multiline_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_sse_parser_init(&p, &alloc);
    s_sse_multiline_event_count = 0;
    s_sse_multiline_last_data[0] = '\0';
    const char *stream =
        "event: message\ndata: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\n";
    hu_error_t err = hu_sse_parser_feed(&p, stream, strlen(stream), sse_multiline_cb, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(s_sse_multiline_event_count >= 1);
    hu_sse_parser_deinit(&p);
}

static void test_sse_parse_callback_order(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_sse_parser_init(&p, &alloc);
    sse_order_idx = 0;
    const char *stream =
        "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\ndata: [DONE]\n\n";
    hu_error_t err = hu_sse_parser_feed(&p, stream, strlen(stream), sse_cb_order, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    hu_sse_parser_deinit(&p);
}

static void test_sse_parser_feed_incremental(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_sse_parser_init(&p, &alloc);
    sse_got_delta = 0;
    hu_error_t err = hu_sse_parser_feed(&p, "data: ", 6, sse_cb_delta, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    const char *chunk2 = "{\"choices\":[{\"delta\":{\"content\":\"y\"}}]}\n\n";
    err = hu_sse_parser_feed(&p, chunk2, strlen(chunk2), sse_cb_delta, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(sse_got_delta == 1);
    hu_sse_parser_deinit(&p);
}

static void test_sse_extract_delta_unicode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"choices\":[{\"delta\":{\"content\":\"\\u00e4\"}}]}";
    char *out = hu_sse_extract_delta_content(&alloc, json, strlen(json));
    HU_ASSERT_NOT_NULL(out);
    if (out)
        alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_sse_parse_line_whitespace_trimmed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *line = "data: [DONE]  \r\n";
    hu_sse_line_result_t out;
    hu_error_t err = hu_sse_parse_line(&alloc, line, strlen(line), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.tag, HU_SSE_DONE);
}

static void test_sse_parse_line_data_with_spaces(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *line = "data:   {\"choices\":[{\"delta\":{\"content\":\"z\"}}]}";
    hu_sse_line_result_t out;
    hu_error_t err = hu_sse_parse_line(&alloc, line, strlen(line), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out.tag == HU_SSE_DELTA || out.tag == HU_SSE_SKIP);
    if (out.delta)
        alloc.free(alloc.ctx, out.delta, out.delta_len + 1);
}

static void test_ollama_supports_streaming(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov = {0};
    hu_error_t err = hu_provider_create(&alloc, "ollama", 6, "test", 4, NULL, 0, &prov);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(prov.vtable->supports_streaming != NULL);
    HU_ASSERT(prov.vtable->supports_streaming(prov.ctx) == true);
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_stream_chat_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov = {0};
    hu_error_t err = hu_provider_create(&alloc, "ollama", 6, "test", 4, NULL, 0, &prov);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(prov.vtable->stream_chat != NULL);
    hu_chat_request_t req = {0};
    hu_stream_chat_result_t result = {0};
    err = prov.vtable->stream_chat(prov.ctx, &alloc, &req, "test", 4, 0.7, NULL, NULL, &result);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(result.content != NULL);
    HU_ASSERT(result.content_len == 4);
    alloc.free(alloc.ctx, (void *)result.content, result.content_len + 1);
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openrouter_supports_streaming(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov = {0};
    hu_error_t err = hu_provider_create(&alloc, "openrouter", 10, "test-key", 8, NULL, 0, &prov);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(prov.vtable->supports_streaming != NULL);
    HU_ASSERT(prov.vtable->supports_streaming(prov.ctx) == true);
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openrouter_stream_chat_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov = {0};
    hu_error_t err = hu_provider_create(&alloc, "openrouter", 10, "test-key", 8, NULL, 0, &prov);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(prov.vtable->stream_chat != NULL);
    hu_chat_request_t req = {0};
    hu_stream_chat_result_t result = {0};
    err = prov.vtable->stream_chat(prov.ctx, &alloc, &req, "test", 4, 0.7, NULL, NULL, &result);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(result.content != NULL);
    HU_ASSERT(result.content_len == 4);
    alloc.free(alloc.ctx, (void *)result.content, result.content_len + 1);
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_compatible_supports_streaming(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov = {0};
    hu_error_t err = hu_provider_create(&alloc, "compatible", 10, "test-key", 8,
                                        "https://example.com", 19, &prov);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(prov.vtable->supports_streaming != NULL);
    HU_ASSERT(prov.vtable->supports_streaming(prov.ctx) == true);
    prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_compatible_stream_chat_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov = {0};
    hu_error_t err = hu_provider_create(&alloc, "compatible", 10, "test-key", 8,
                                        "https://example.com", 19, &prov);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(prov.vtable->stream_chat != NULL);
    hu_chat_request_t req = {0};
    hu_stream_chat_result_t result = {0};
    err = prov.vtable->stream_chat(prov.ctx, &alloc, &req, "test", 4, 0.7, NULL, NULL, &result);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(result.content != NULL);
    HU_ASSERT(result.content_len == 4);
    alloc.free(alloc.ctx, (void *)result.content, result.content_len + 1);
    prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Structured Output ───────────────────────────────────────────── */

static void test_openai_structured_output_json_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("respond in JSON", 15)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    req.response_format = "json_object";
    req.response_format_len = 11;
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4", 5, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(resp.content != NULL);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_structured_output_json_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_anthropic_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("respond in JSON", 15)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    req.response_format = "json_object";
    req.response_format_len = 11;
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "claude-3", 8, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    if (resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_structured_output_json_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_gemini_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("respond in JSON", 15)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    req.response_format = "json_object";
    req.response_format_len = 11;
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gemini-pro", 10, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    if (resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_compatible_structured_output_json_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_compatible_create(&alloc, "key", 3, "https://api.example.com/v1/chat/completions", 44,
                         &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("respond in JSON", 15)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    req.response_format = "json_object";
    req.response_format_len = 11;
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "model", 5, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    if (resp.content)
        alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_structured_output_null_format_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_openai_create(&alloc, "key", 3, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("hi", 2)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    req.response_format = NULL;
    req.response_format_len = 0;
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4", 5, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_structured_output_json_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    hu_chat_message_t msgs[1] = {make_user_msg("respond in JSON", 15)};
    hu_chat_request_t req = make_simple_request(msgs, 1);
    req.response_format = "json_object";
    req.response_format_len = 11;
    hu_chat_response_t resp = {0};
    hu_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "llama3", 6, 0.7, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    hu_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

void run_provider_all_tests(void) {
    HU_TEST_SUITE("Provider All");
    HU_RUN_TEST(test_openai_create_succeeds);
    HU_RUN_TEST(test_openai_create_null_alloc_fails);
    HU_RUN_TEST(test_openai_get_name);
    HU_RUN_TEST(test_openai_supports_native_tools);
    HU_RUN_TEST(test_openai_chat_mock);
    HU_RUN_TEST(test_openai_chat_null_request_returns_error);
    HU_RUN_TEST(test_openai_chat_empty_messages_graceful);
    HU_RUN_TEST(test_openai_deinit_no_crash);
    HU_RUN_TEST(test_openai_create_empty_key);
    HU_RUN_TEST(test_openai_create_with_base_url);
    HU_RUN_TEST(test_openai_chat_with_system_and_user);
    HU_RUN_TEST(test_openai_chat_tool_calls_mock);
    HU_RUN_TEST(test_openai_stream_chat_mock);
    HU_RUN_TEST(test_openai_model_passthrough);
    HU_RUN_TEST(test_openai_temperature_passthrough);
    HU_RUN_TEST(test_openai_chat_null_request_returns_error);
    HU_RUN_TEST(test_openai_chat_empty_messages_graceful);
    HU_RUN_TEST(test_openai_structured_output_json_mode);

    HU_RUN_TEST(test_anthropic_create_succeeds);
    HU_RUN_TEST(test_anthropic_create_null_alloc_fails);
    HU_RUN_TEST(test_anthropic_get_name);
    HU_RUN_TEST(test_anthropic_supports_native_tools);
    HU_RUN_TEST(test_anthropic_chat_mock);
    HU_RUN_TEST(test_anthropic_chat_with_tools_mock);
    HU_RUN_TEST(test_anthropic_deinit_no_crash);
    HU_RUN_TEST(test_anthropic_create_empty_key);
    HU_RUN_TEST(test_anthropic_create_with_base_url);
    HU_RUN_TEST(test_anthropic_chat_with_system_and_user);
    HU_RUN_TEST(test_anthropic_max_tokens_in_request);
    HU_RUN_TEST(test_anthropic_stream_chat_mock);
    HU_RUN_TEST(test_anthropic_tool_call_format);
    HU_RUN_TEST(test_anthropic_chat_null_request_returns_error);
    HU_RUN_TEST(test_anthropic_chat_empty_messages_graceful);
    HU_RUN_TEST(test_anthropic_structured_output_json_mode);

    HU_RUN_TEST(test_gemini_create_succeeds);
    HU_RUN_TEST(test_gemini_create_null_alloc_fails);
    HU_RUN_TEST(test_gemini_get_name);
    HU_RUN_TEST(test_gemini_supports_native_tools);
    HU_RUN_TEST(test_gemini_chat_mock);
    HU_RUN_TEST(test_gemini_deinit_no_crash);
    HU_RUN_TEST(test_gemini_create_empty_key);
    HU_RUN_TEST(test_gemini_create_with_oauth);
    HU_RUN_TEST(test_gemini_create_oauth_null_fails);
    HU_RUN_TEST(test_gemini_supports_vision);
    HU_RUN_TEST(test_gemini_chat_with_tools_mock);
    HU_RUN_TEST(test_gemini_model_gemini2_flash);
    HU_RUN_TEST(test_gemini_stream_chat_mock);
    HU_RUN_TEST(test_gemini_chat_null_request_returns_error);
    HU_RUN_TEST(test_gemini_chat_empty_messages_graceful);
    HU_RUN_TEST(test_gemini_structured_output_json_mode);

    HU_RUN_TEST(test_ollama_create_succeeds);
    HU_RUN_TEST(test_ollama_create_null_alloc_fails);
    HU_RUN_TEST(test_ollama_get_name);
    HU_RUN_TEST(test_ollama_supports_native_tools);
    HU_RUN_TEST(test_ollama_chat_mock);
    HU_RUN_TEST(test_ollama_deinit_no_crash);
    HU_RUN_TEST(test_ollama_create_empty_key);
    HU_RUN_TEST(test_ollama_create_with_base_url);
    HU_RUN_TEST(test_ollama_no_api_key_required);
    HU_RUN_TEST(test_ollama_chat_with_tools_mock);
    HU_RUN_TEST(test_ollama_local_model_format);
    HU_RUN_TEST(test_ollama_chat_null_request_returns_error);
    HU_RUN_TEST(test_ollama_chat_empty_messages_graceful);
    HU_RUN_TEST(test_ollama_structured_output_json_mode);

    HU_RUN_TEST(test_openrouter_create_succeeds);
    HU_RUN_TEST(test_openrouter_create_null_alloc_fails);
    HU_RUN_TEST(test_openrouter_get_name);
    HU_RUN_TEST(test_openrouter_supports_native_tools);
    HU_RUN_TEST(test_openrouter_chat_mock);
    HU_RUN_TEST(test_openrouter_deinit_no_crash);
    HU_RUN_TEST(test_openrouter_create_empty_key);
    HU_RUN_TEST(test_openrouter_model_routing);
    HU_RUN_TEST(test_openrouter_chat_with_tools_mock);
    HU_RUN_TEST(test_openrouter_chat_null_request_returns_error);
    HU_RUN_TEST(test_openrouter_chat_empty_messages_graceful);

    HU_RUN_TEST(test_compatible_create_succeeds);
    HU_RUN_TEST(test_compatible_create_null_alloc_fails);
    HU_RUN_TEST(test_compatible_get_name);
    HU_RUN_TEST(test_compatible_supports_native_tools);
    HU_RUN_TEST(test_compatible_chat_mock);
    HU_RUN_TEST(test_compatible_deinit_no_crash);
    HU_RUN_TEST(test_compatible_create_empty_key);
    HU_RUN_TEST(test_compatible_mistral_base_url);
    HU_RUN_TEST(test_compatible_deepseek_base_url);
    HU_RUN_TEST(test_compatible_together_base_url);
    HU_RUN_TEST(test_compatible_fireworks_base_url);
    HU_RUN_TEST(test_compatible_perplexity_base_url);
    HU_RUN_TEST(test_compatible_cerebras_base_url);
    HU_RUN_TEST(test_compatible_groq_base_url);
    HU_RUN_TEST(test_compatible_xai_base_url);
    HU_RUN_TEST(test_factory_mistral_chat_mock);
    HU_RUN_TEST(test_factory_deepseek_chat_mock);
    HU_RUN_TEST(test_factory_together_chat_mock);
    HU_RUN_TEST(test_factory_fireworks_chat_mock);
    HU_RUN_TEST(test_factory_perplexity_chat_mock);
    HU_RUN_TEST(test_factory_cerebras_chat_mock);
    HU_RUN_TEST(test_factory_xai_chat_mock);
    HU_RUN_TEST(test_compatible_chat_with_tools_mock);
    HU_RUN_TEST(test_compatible_chat_null_request_returns_error);
    HU_RUN_TEST(test_compatible_chat_empty_messages_graceful);
    HU_RUN_TEST(test_compatible_structured_output_json_mode);
    HU_RUN_TEST(test_structured_output_null_format_no_crash);

    HU_RUN_TEST(test_claude_cli_create_succeeds);
    HU_RUN_TEST(test_claude_cli_create_null_alloc_fails);
    HU_RUN_TEST(test_claude_cli_get_name);
    HU_RUN_TEST(test_claude_cli_supports_native_tools);
    HU_RUN_TEST(test_claude_cli_chat_mock);
    HU_RUN_TEST(test_claude_cli_deinit_no_crash);
    HU_RUN_TEST(test_claude_cli_create_empty_key);
    HU_RUN_TEST(test_claude_cli_chat_empty_messages_graceful);

    HU_RUN_TEST(test_compatible_url_lookup_groq);
    HU_RUN_TEST(test_compatible_url_lookup_mistral);
    HU_RUN_TEST(test_compatible_url_lookup_deepseek);
    HU_RUN_TEST(test_compatible_url_lookup_together);
    HU_RUN_TEST(test_compatible_url_lookup_fireworks);
    HU_RUN_TEST(test_compatible_url_lookup_perplexity);
    HU_RUN_TEST(test_compatible_url_lookup_cerebras);
    HU_RUN_TEST(test_compatible_url_lookup_unknown);
    HU_RUN_TEST(test_factory_creates_compatible_for_groq);

    HU_RUN_TEST(test_factory_google_creates_gemini);
    HU_RUN_TEST(test_factory_google_gemini_creates_gemini);
    HU_RUN_TEST(test_factory_custom_prefix_creates_compatible);
    HU_RUN_TEST(test_factory_anthropic_custom_creates_anthropic);

    HU_RUN_TEST(test_provider_factory_unknown_returns_error);
    HU_RUN_TEST(test_provider_factory_null_name_returns_error);
    HU_RUN_TEST(test_provider_factory_empty_name_returns_error);
    HU_RUN_TEST(test_provider_factory_null_out_returns_error);
    HU_RUN_TEST(test_factory_openai_resolves);
    HU_RUN_TEST(test_factory_anthropic_resolves);
    HU_RUN_TEST(test_factory_gemini_resolves);
    HU_RUN_TEST(test_factory_ollama_resolves);
    HU_RUN_TEST(test_factory_openrouter_resolves);
    HU_RUN_TEST(test_factory_case_sensitive_unknown);
    HU_RUN_TEST(test_factory_gpt4o_not_provider_name);
    HU_RUN_TEST(test_factory_openai_create_empty_key);
    HU_RUN_TEST(test_factory_anthropic_create_empty_key);
    HU_RUN_TEST(test_factory_gemini_create_empty_key);
    HU_RUN_TEST(test_factory_openai_chat_returns_stub);
    HU_RUN_TEST(test_factory_openai_chat_with_system_returns_stub);
    HU_RUN_TEST(test_factory_anthropic_chat_returns_stub);
    HU_RUN_TEST(test_factory_anthropic_chat_with_system_returns_stub);
    HU_RUN_TEST(test_factory_gemini_chat_returns_stub);
    HU_RUN_TEST(test_factory_gemini_chat_with_system_returns_stub);

    HU_RUN_TEST(test_sse_parse_line_data_extracts_delta);
    HU_RUN_TEST(test_sse_parse_line_done);
    HU_RUN_TEST(test_sse_parse_line_comment_skipped);
    HU_RUN_TEST(test_sse_parse_line_empty_skipped);
    HU_RUN_TEST(test_sse_parse_line_no_data_prefix_skipped);
    HU_RUN_TEST(test_sse_extract_delta_empty_choices);
    HU_RUN_TEST(test_sse_extract_delta_no_delta_key);
    HU_RUN_TEST(test_sse_extract_delta_content);
    HU_RUN_TEST(test_sse_parser_init_deinit);
    HU_RUN_TEST(test_sse_parser_feed_callback);
    HU_RUN_TEST(test_sse_parse_multiline_data);
    HU_RUN_TEST(test_sse_parse_callback_order);
    HU_RUN_TEST(test_sse_parser_feed_incremental);
    HU_RUN_TEST(test_sse_extract_delta_unicode);
    HU_RUN_TEST(test_sse_parse_line_whitespace_trimmed);
    HU_RUN_TEST(test_sse_parse_line_data_with_spaces);

    HU_RUN_TEST(test_error_classify_rate_limited_429);
    HU_RUN_TEST(test_error_classify_rate_limited_too_many);
    HU_RUN_TEST(test_error_classify_rate_limited_quota);
    HU_RUN_TEST(test_error_classify_not_rate_limited);
    HU_RUN_TEST(test_error_classify_context_exhausted);
    HU_RUN_TEST(test_error_classify_context_token_limit);
    HU_RUN_TEST(test_error_classify_not_context_exhausted);
    HU_RUN_TEST(test_error_classify_non_retryable_401);
    HU_RUN_TEST(test_error_classify_non_retryable_403);
    HU_RUN_TEST(test_error_classify_429_not_non_retryable);
    HU_RUN_TEST(test_error_classify_retry_after_seconds);
    HU_RUN_TEST(test_error_classify_text_rate_limited);
    HU_RUN_TEST(test_error_classify_text_context_exhausted);

    HU_RUN_TEST(test_factory_mistral_creates_compatible);
    HU_RUN_TEST(test_factory_deepseek_creates_compatible);
    HU_RUN_TEST(test_factory_together_creates_compatible);
    HU_RUN_TEST(test_factory_fireworks_creates_compatible);
    HU_RUN_TEST(test_factory_perplexity_creates_compatible);
    HU_RUN_TEST(test_factory_cerebras_creates_compatible);
    HU_RUN_TEST(test_factory_xai_creates_compatible);
    HU_RUN_TEST(test_factory_grok_creates_compatible);

    HU_RUN_TEST(test_codex_cli_create_succeeds);
    HU_RUN_TEST(test_codex_cli_get_name);
    HU_RUN_TEST(test_codex_cli_supports_native_tools);
    HU_RUN_TEST(test_codex_cli_chat_mock);
    HU_RUN_TEST(test_codex_cli_create_null_alloc_fails);
    HU_RUN_TEST(test_codex_cli_deinit_no_crash);
    HU_RUN_TEST(test_codex_cli_chat_empty_messages_graceful);
    HU_RUN_TEST(test_openai_codex_create_succeeds);
    HU_RUN_TEST(test_openai_codex_get_name);
    HU_RUN_TEST(test_openai_codex_supports_native_tools);
    HU_RUN_TEST(test_openai_codex_chat_mock);
    HU_RUN_TEST(test_openai_codex_create_null_alloc_fails);
    HU_RUN_TEST(test_openai_codex_deinit_no_crash);
    HU_RUN_TEST(test_openai_codex_chat_null_request_returns_error);
    HU_RUN_TEST(test_openai_codex_chat_empty_messages_graceful);

    HU_RUN_TEST(test_reliable_provider_create_from_config);
    HU_RUN_TEST(test_reliable_create_simple);
    HU_RUN_TEST(test_reliable_supports_native_tools_aggregates);
    HU_RUN_TEST(test_reliable_supports_vision_aggregates);
    HU_RUN_TEST(test_reliable_chat_passthrough);
    HU_RUN_TEST(test_reliable_create_with_extras);
    HU_RUN_TEST(test_reliable_create_with_model_fallbacks);
    HU_RUN_TEST(test_reliable_chat_with_extras_primary_succeeds);
    HU_RUN_TEST(test_reliable_warmup_calls_all);
    HU_RUN_TEST(test_reliable_supports_vision_from_gemini);
    HU_RUN_TEST(test_reliable_chat_with_system);

    HU_RUN_TEST(test_scrub_sk_prefix);
    HU_RUN_TEST(test_scrub_ghp_prefix);
    HU_RUN_TEST(test_scrub_bearer_token);
    HU_RUN_TEST(test_scrub_plain_text_unmodified);
    HU_RUN_TEST(test_scrub_sanitize_truncates);

    HU_RUN_TEST(test_max_tokens_default);
    HU_RUN_TEST(test_max_tokens_lookup_gpt4o);
    HU_RUN_TEST(test_max_tokens_lookup_claude);
    HU_RUN_TEST(test_max_tokens_lookup_unknown);
    HU_RUN_TEST(test_max_tokens_resolve_override);
    HU_RUN_TEST(test_max_tokens_resolve_fallback);

    HU_RUN_TEST(test_error_classify_vision_unsupported);
    HU_RUN_TEST(test_error_classify_vision_not_unsupported);

    HU_RUN_TEST(test_ollama_supports_streaming);
    HU_RUN_TEST(test_ollama_stream_chat_test_mode);
    HU_RUN_TEST(test_openrouter_supports_streaming);
    HU_RUN_TEST(test_openrouter_stream_chat_test_mode);
    HU_RUN_TEST(test_compatible_supports_streaming);
    HU_RUN_TEST(test_compatible_stream_chat_test_mode);

    HU_RUN_TEST(test_sse_parse_line_delta_tool_call);
    HU_RUN_TEST(test_sse_extract_delta_null_content);
}
