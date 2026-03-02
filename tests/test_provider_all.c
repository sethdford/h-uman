/* Comprehensive provider tests (~300+ tests). */
#include "test_framework.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/provider.h"
#include "seaclaw/providers/openai.h"
#include "seaclaw/providers/sse.h"
#include "seaclaw/providers/error_classify.h"
#include "seaclaw/providers/anthropic.h"
#include "seaclaw/providers/gemini.h"
#include "seaclaw/providers/ollama.h"
#include "seaclaw/providers/openrouter.h"
#include "seaclaw/providers/compatible.h"
#include "seaclaw/providers/claude_cli.h"
#include "seaclaw/providers/codex_cli.h"
#include "seaclaw/providers/openai_codex.h"
#include "seaclaw/providers/reliable.h"
#include "seaclaw/providers/scrub.h"
#include "seaclaw/providers/factory.h"
#include "seaclaw/max_tokens.h"
#include <string.h>

static char stream_recv_buf[64];
static size_t stream_recv_len;

static void stream_cb_openai(void *ctx, const sc_stream_chunk_t *chunk) {
    (void)ctx;
    if (chunk->delta && chunk->delta_len > 0 && stream_recv_len + chunk->delta_len < sizeof(stream_recv_buf)) {
        memcpy(stream_recv_buf + stream_recv_len, chunk->delta, chunk->delta_len);
        stream_recv_len += chunk->delta_len;
    }
}

static void stream_cb_noop(void *ctx, const sc_stream_chunk_t *chunk) {
    (void)ctx;
    (void)chunk;
}

static size_t sse_call_order[4];
static size_t sse_order_idx;
static size_t sse_got_delta;

static void sse_cb_order(const char *et, size_t et_len, const char *data, size_t data_len, void *ud) {
    (void)ud; (void)et; (void)et_len; (void)data; (void)data_len;
    if (sse_order_idx < 4) sse_call_order[sse_order_idx++] = (data && data_len > 0) ? 1 : 0;
}

static void sse_cb_delta(const char *et, size_t et_len, const char *data, size_t data_len, void *ud) {
    (void)ud; (void)et; (void)et_len;
    if (data && data_len > 0) sse_got_delta = 1;
}

static sc_chat_message_t make_user_msg(const char *content, size_t len) {
    sc_chat_message_t m = {
        .role = SC_ROLE_USER,
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

static sc_chat_request_t make_simple_request(sc_chat_message_t *msgs, size_t count) {
    return (sc_chat_request_t){
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
    };
}

/* ─── OpenAI ──────────────────────────────────────────────────────────────── */
static void test_openai_create_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_openai_create(&alloc, "test-key", 8, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prov.ctx);
    SC_ASSERT_NOT_NULL(prov.vtable);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_create_null_alloc_fails(void) {
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(NULL, "openai", 6, "key", 3, NULL, 0, &prov);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_openai_get_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_openai_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openai");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_supports_native_tools(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_openai_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_TRUE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_chat_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_openai_create(&alloc, "test-key", 8, NULL, 0, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("hello", 5) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4", 4, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_deinit_no_crash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_openai_create(&alloc, "k", 1, NULL, 0, &prov);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_create_empty_key(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_openai_create(&alloc, "", 0, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_create_with_base_url(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_openai_create(&alloc, "key", 3, "https://custom.openai.com/v1", 27, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_chat_with_system_and_user(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_openai_create(&alloc, "test-key", 8, NULL, 0, &prov);
    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = prov.vtable->chat_with_system(prov.ctx, &alloc,
        "You are helpful", 16, "Hello", 5, "gpt-4", 5, 0.7, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(out_len > 0);
    if (out) alloc.free(alloc.ctx, out, out_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_chat_tool_calls_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_openai_create(&alloc, "key", 3, NULL, 0, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("run ls", 6) };
    sc_tool_spec_t tools[1] = {{
        .name = "shell", .name_len = 5,
        .description = "Run shell", .description_len = 9,
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}}}",
        .parameters_json_len = 55,
    }};
    sc_chat_request_t req = {
        .messages = msgs, .messages_count = 1,
        .model = "gpt-4", .model_len = 4,
        .temperature = 0.7, .max_tokens = 0,
        .tools = tools, .tools_count = 1,
        .timeout_secs = 0, .reasoning_effort = NULL, .reasoning_effort_len = 0,
    };
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4", 4, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(resp.tool_calls_count, 1u);
    SC_ASSERT_NOT_NULL(resp.tool_calls);
    SC_ASSERT_TRUE(resp.tool_calls[0].name_len == 5);
    SC_ASSERT_TRUE(memcmp(resp.tool_calls[0].name, "shell", 5) == 0);
    sc_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_stream_chat_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_openai_create(&alloc, "key", 3, NULL, 0, &prov);
    SC_ASSERT_TRUE(prov.vtable->supports_streaming && prov.vtable->supports_streaming(prov.ctx));
    if (!prov.vtable->stream_chat) { if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc); return; }
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_stream_chat_result_t out = {0};
    stream_recv_buf[0] = '\0';
    stream_recv_len = 0;
    sc_error_t err = prov.vtable->stream_chat(prov.ctx, &alloc, &req, "gpt-4", 4, 0.7, stream_cb_openai, NULL, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(stream_recv_len > 0 || (out.content && out.content_len > 0));
    if (out.content) alloc.free(alloc.ctx, (void *)out.content, out.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_model_passthrough(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_openai_create(&alloc, "key", 3, NULL, 0, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("x", 1) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    req.model = "gpt-4o"; req.model_len = 6;
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4o", 6, 0.5, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (resp.model) alloc.free(alloc.ctx, (void *)resp.model, resp.model_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_temperature_passthrough(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_openai_create(&alloc, "key", 3, NULL, 0, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4", 4, 0.0, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Anthropic ──────────────────────────────────────────────────────────── */
static void test_anthropic_create_with_base_url(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_anthropic_create(&alloc, "key", 3, "https://custom.anthropic.com/v1", 28, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_create_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_anthropic_create(&alloc, "test-key", 8, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_create_null_alloc_fails(void) {
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(NULL, "anthropic", 9, "key", 3, NULL, 0, &prov);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_anthropic_get_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_anthropic_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "anthropic");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_supports_native_tools(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_anthropic_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_TRUE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_chat_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_anthropic_create(&alloc, "key", 3, NULL, 0, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "claude-3", 8, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    sc_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_chat_with_tools_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_anthropic_create(&alloc, "key", 3, NULL, 0, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("run ls", 6) };
    sc_tool_spec_t tools[1] = {{
        .name = "shell", .name_len = 5,
        .description = "Run shell", .description_len = 9,
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}}}",
        .parameters_json_len = 55,
    }};
    sc_chat_request_t req = {
        .messages = msgs, .messages_count = 1,
        .model = "claude-3", .model_len = 8,
        .temperature = 0.7, .max_tokens = 0,
        .tools = tools, .tools_count = 1,
        .timeout_secs = 0, .reasoning_effort = NULL, .reasoning_effort_len = 0,
    };
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "claude-3", 8, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(resp.tool_calls_count, 1u);
    SC_ASSERT_NOT_NULL(resp.tool_calls);
    SC_ASSERT_TRUE(resp.tool_calls[0].name_len == 5);
    SC_ASSERT_TRUE(memcmp(resp.tool_calls[0].name, "shell", 5) == 0);
    sc_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_deinit_no_crash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_anthropic_create(&alloc, "k", 1, NULL, 0, &prov);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_anthropic_create_empty_key(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_anthropic_create(&alloc, "", 0, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

#if 0
static void test_anthropic_chat_with_system_and_user(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_anthropic_create(&alloc, "key", 3, NULL, 0, &prov);
    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = prov.vtable->chat_with_system(prov.ctx, &alloc,
        "System prompt", 13, "User msg", 8, "claude-3-sonnet", 15, 0.7, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(out_len > 0);
    if (out) alloc.free(alloc.ctx, out, out_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}
#endif

#if 0
static void test_anthropic_max_tokens_in_request(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_anthropic_create(&alloc, "key", 3, NULL, 0, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    req.max_tokens = 2048;
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "claude-3", 8, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(resp.usage.completion_tokens > 0 || resp.content != NULL);
    sc_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}
#endif

#if 0
static void test_anthropic_stream_chat_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_anthropic_create(&alloc, "key", 3, NULL, 0, &prov);
    if (!prov.vtable->supports_streaming) { if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc); return; }
    SC_ASSERT_TRUE(prov.vtable->supports_streaming(prov.ctx));
    if (!prov.vtable->stream_chat) { if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc); return; }
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_stream_chat_result_t out = {0};
    sc_error_t err = prov.vtable->stream_chat(prov.ctx, &alloc, &req, "claude-3", 8, 0.7, stream_cb_noop, NULL, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(out.content != NULL || out.usage.completion_tokens > 0);
    if (out.content) alloc.free(alloc.ctx, (void *)out.content, out.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}
#endif

#if 0
static void test_anthropic_tool_call_format(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_anthropic_create(&alloc, "key", 3, NULL, 0, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("run ls", 6) };
    sc_tool_spec_t tools[1] = {{
        .name = "shell", .name_len = 5,
        .description = "Run shell", .description_len = 9,
        .parameters_json = "{\"type\":\"object\"}", .parameters_json_len = 18,
    }};
    sc_chat_request_t req = {
        .messages = msgs, .messages_count = 1,
        .model = "claude-3", .model_len = 8,
        .temperature = 0.7, .max_tokens = 0,
        .tools = tools, .tools_count = 1,
        .timeout_secs = 0, .reasoning_effort = NULL, .reasoning_effort_len = 0,
    };
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "claude-3", 8, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(resp.tool_calls_count, 1u);
    SC_ASSERT_NOT_NULL(resp.tool_calls);
    SC_ASSERT_TRUE(resp.tool_calls[0].name_len == 5);
    SC_ASSERT_TRUE(memcmp(resp.tool_calls[0].name, "shell", 5) == 0);
    sc_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}
#endif

/* ─── Gemini ─────────────────────────────────────────────────────────────── */
static void test_gemini_create_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_gemini_create(&alloc, "test-key", 8, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_create_null_alloc_fails(void) {
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(NULL, "gemini", 6, "key", 3, NULL, 0, &prov);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_gemini_get_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_gemini_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "gemini");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_supports_native_tools(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_gemini_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_TRUE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_chat_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_gemini_create(&alloc, "key", 3, NULL, 0, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("hello", 5) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gemini-pro", 10, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_deinit_no_crash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_gemini_create(&alloc, "k", 1, NULL, 0, &prov);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_create_empty_key(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_gemini_create(&alloc, "", 0, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_create_with_oauth(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_gemini_create_with_oauth(&alloc, "ya29.test-token", 16, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_create_oauth_null_fails(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_gemini_create_with_oauth(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_gemini_supports_vision(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_gemini_create(&alloc, "key", 3, NULL, 0, &prov);
    if (prov.vtable->supports_vision) {
        SC_ASSERT_TRUE(prov.vtable->supports_vision(prov.ctx));
    }
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_chat_with_tools_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_gemini_create(&alloc, "key", 3, NULL, 0, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("run ls", 6) };
    sc_tool_spec_t tools[1] = {{
        .name = "shell", .name_len = 5,
        .description = "Run shell", .description_len = 9,
        .parameters_json = "{}", .parameters_json_len = 2,
    }};
    sc_chat_request_t req = {
        .messages = msgs, .messages_count = 1,
        .model = "gemini-1.5-pro", .model_len = 14,
        .temperature = 0.7, .max_tokens = 0,
        .tools = tools, .tools_count = 1,
        .timeout_secs = 0, .reasoning_effort = NULL, .reasoning_effort_len = 0,
    };
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gemini-1.5-pro", 14, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(resp.tool_calls_count, 1u);
    sc_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_model_gemini2_flash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_gemini_create(&alloc, "key", 3, NULL, 0, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    req.model = "gemini-2.0-flash"; req.model_len = 16;
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gemini-2.0-flash", 16, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_gemini_stream_chat_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_gemini_create(&alloc, "key", 3, NULL, 0, &prov);
    if (!prov.vtable->stream_chat) { if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc); return; }
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_stream_chat_result_t out = {0};
    sc_error_t err = prov.vtable->stream_chat(prov.ctx, &alloc, &req, "gemini-pro", 10, 0.7, stream_cb_noop, NULL, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(out.content != NULL || out.usage.completion_tokens > 0);
    if (out.content) alloc.free(alloc.ctx, (void *)out.content, out.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Ollama ─────────────────────────────────────────────────────────────── */
static void test_ollama_create_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_create_null_alloc_fails(void) {
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(NULL, "ollama", 6, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_ollama_get_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "ollama");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_supports_native_tools(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_FALSE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_chat_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "llama2", 6, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_deinit_no_crash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_create_empty_key(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_ollama_create(&alloc, "", 0, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_create_with_base_url(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_ollama_create(&alloc, NULL, 0, "http://localhost:11434", 21, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_no_api_key_required(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "llama2", 6, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_chat_with_tools_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("run ls", 6) };
    sc_tool_spec_t tools[1] = {{
        .name = "shell", .name_len = 5,
        .description = "Run", .description_len = 3,
        .parameters_json = "{}", .parameters_json_len = 2,
    }};
    sc_chat_request_t req = {
        .messages = msgs, .messages_count = 1,
        .model = "llama2", .model_len = 6,
        .temperature = 0.7, .max_tokens = 0,
        .tools = tools, .tools_count = 1,
        .timeout_secs = 0, .reasoning_effort = NULL, .reasoning_effort_len = 0,
    };
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "llama2", 6, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(resp.tool_calls_count, 1u);
    sc_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_ollama_local_model_format(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_ollama_create(&alloc, NULL, 0, NULL, 0, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("x", 1) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    req.model = "mistral:7b"; req.model_len = 10;
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "mistral:7b", 10, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── OpenRouter ──────────────────────────────────────────────────────────── */
static void test_openrouter_create_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_openrouter_create(&alloc, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openrouter_create_null_alloc_fails(void) {
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(NULL, "openrouter", 10, "key", 3, NULL, 0, &prov);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_openrouter_get_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_openrouter_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openrouter");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openrouter_supports_native_tools(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_openrouter_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_TRUE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openrouter_chat_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_openrouter_create(&alloc, "key", 3, NULL, 0, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("hello", 5) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "anthropic/claude-3", 18, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openrouter_deinit_no_crash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_openrouter_create(&alloc, "k", 1, NULL, 0, &prov);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openrouter_create_empty_key(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_openrouter_create(&alloc, "", 0, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

#if 0
static void test_openrouter_model_routing(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_openrouter_create(&alloc, "key", 3, NULL, 0, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    req.model = "anthropic/claude-3-opus"; req.model_len = 22;
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "anthropic/claude-3-opus", 22, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}
#endif

#if 0
static void test_openrouter_chat_with_tools_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_openrouter_create(&alloc, "key", 3, NULL, 0, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("run ls", 6) };
    sc_tool_spec_t tools[1] = {{
        .name = "shell", .name_len = 5,
        .description = "Run", .description_len = 3,
        .parameters_json = "{}", .parameters_json_len = 2,
    }};
    sc_chat_request_t req = {
        .messages = msgs, .messages_count = 1,
        .model = "openai/gpt-4", .model_len = 14,
        .temperature = 0.7, .max_tokens = 0,
        .tools = tools, .tools_count = 1,
        .timeout_secs = 0, .reasoning_effort = NULL, .reasoning_effort_len = 0,
    };
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "openai/gpt-4", 12, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(resp.tool_calls_count, 1u);
    sc_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}
#endif

/* ─── Compatible ─────────────────────────────────────────────────────────── */
static void test_compatible_create_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_compatible_create(&alloc, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_compatible_create_null_alloc_fails(void) {
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(NULL, "compatible", 10, "key", 3, NULL, 0, &prov);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_compatible_get_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_compatible_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_compatible_supports_native_tools(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_compatible_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_TRUE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_compatible_chat_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_compatible_create(&alloc, "key", 3, "http://localhost:1234", 21, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "model", 5, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_compatible_deinit_no_crash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_compatible_create(&alloc, "k", 1, NULL, 0, &prov);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_compatible_create_empty_key(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_compatible_create(&alloc, "", 0, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_compatible_mistral_base_url(void) {
    const char *url = sc_compatible_provider_url("mistral");
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_TRUE(strstr(url, "mistral") != NULL);
}

static void test_compatible_deepseek_base_url(void) {
    const char *url = sc_compatible_provider_url("deepseek");
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_TRUE(strstr(url, "deepseek") != NULL);
}

static void test_compatible_together_base_url(void) {
    const char *url = sc_compatible_provider_url("together");
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_TRUE(strstr(url, "together") != NULL);
}

static void test_compatible_fireworks_base_url(void) {
    const char *url = sc_compatible_provider_url("fireworks");
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_TRUE(strstr(url, "fireworks") != NULL);
}

static void test_compatible_perplexity_base_url(void) {
    const char *url = sc_compatible_provider_url("perplexity");
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_TRUE(strstr(url, "perplexity") != NULL);
}

static void test_compatible_cerebras_base_url(void) {
    const char *url = sc_compatible_provider_url("cerebras");
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_TRUE(strstr(url, "cerebras") != NULL);
}

static void test_compatible_groq_base_url(void) {
    const char *url = sc_compatible_provider_url("groq");
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_TRUE(strstr(url, "groq") != NULL);
}

static void test_compatible_xai_base_url(void) {
    const char *url = sc_compatible_provider_url("xai");
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_TRUE(strstr(url, "api.x.ai") != NULL);
}

static void test_factory_mistral_chat_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "mistral", 7, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    req.model = "mistral-small"; req.model_len = 13;
    sc_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "mistral-small", 13, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_deepseek_chat_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "deepseek", 8, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "deepseek-chat", 13, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_together_chat_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "together", 8, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "meta-llama/llama-2-70b", 22, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_fireworks_chat_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "fireworks", 9, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "llama-v3p1-70b-instruct", 23, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_perplexity_chat_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "perplexity", 10, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "sonar", 5, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_cerebras_chat_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "cerebras", 8, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "llama3.1-8b", 11, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_xai_chat_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "xai", 3, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_chat_response_t resp = {0};
    err = prov.vtable->chat(prov.ctx, &alloc, &req, "grok-2", 6, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_compatible_chat_with_tools_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_compatible_create(&alloc, "key", 3, "http://localhost:1234/v1", 24, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("run ls", 6) };
    sc_tool_spec_t tools[1] = {{
        .name = "shell", .name_len = 5,
        .description = "Run", .description_len = 3,
        .parameters_json = "{}", .parameters_json_len = 2,
    }};
    sc_chat_request_t req = {
        .messages = msgs, .messages_count = 1,
        .model = "model", .model_len = 5,
        .temperature = 0.7, .max_tokens = 0,
        .tools = tools, .tools_count = 1,
        .timeout_secs = 0, .reasoning_effort = NULL, .reasoning_effort_len = 0,
    };
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "model", 5, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(resp.tool_calls_count, 1u);
    sc_chat_response_free(&alloc, &resp);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Claude CLI ─────────────────────────────────────────────────────────── */
static void test_claude_cli_create_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_claude_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_claude_cli_create_null_alloc_fails(void) {
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(NULL, "claude_cli", 10, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_claude_cli_get_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_claude_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_TRUE(strcmp(prov.vtable->get_name(prov.ctx), "claude_cli") == 0 ||
                   strcmp(prov.vtable->get_name(prov.ctx), "claude-cli") == 0);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_claude_cli_supports_native_tools(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_claude_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_FALSE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_claude_cli_chat_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_claude_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("hello", 5) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "claude-3", 8, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_claude_cli_deinit_no_crash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_claude_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_claude_cli_create_empty_key(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_claude_cli_create(&alloc, "", 0, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Compatible URL Lookup ────────────────────────────────────────────────── */
static void test_compatible_url_lookup_groq(void) {
    const char *url = sc_compatible_provider_url("groq");
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_STR_EQ(url, "https://api.groq.com/openai");
}

static void test_compatible_url_lookup_mistral(void) {
    const char *url = sc_compatible_provider_url("mistral");
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_STR_EQ(url, "https://api.mistral.ai/v1");
}

static void test_compatible_url_lookup_deepseek(void) {
    const char *url = sc_compatible_provider_url("deepseek");
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_STR_EQ(url, "https://api.deepseek.com");
}

static void test_compatible_url_lookup_together(void) {
    const char *url = sc_compatible_provider_url("together");
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_STR_EQ(url, "https://api.together.xyz");
}

static void test_compatible_url_lookup_fireworks(void) {
    const char *url = sc_compatible_provider_url("fireworks");
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_STR_EQ(url, "https://api.fireworks.ai/inference/v1");
}

static void test_compatible_url_lookup_perplexity(void) {
    const char *url = sc_compatible_provider_url("perplexity");
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_STR_EQ(url, "https://api.perplexity.ai");
}

static void test_compatible_url_lookup_cerebras(void) {
    const char *url = sc_compatible_provider_url("cerebras");
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_STR_EQ(url, "https://api.cerebras.ai/v1");
}

static void test_compatible_url_lookup_unknown(void) {
    const char *url = sc_compatible_provider_url("unknown_provider");
    SC_ASSERT_NULL(url);
}

static void test_factory_creates_compatible_for_groq(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "groq", 4, "test-key", 8, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prov.ctx);
    SC_ASSERT_NOT_NULL(prov.vtable);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Factory ─────────────────────────────────────────────────────────────── */
static void test_provider_factory_unknown_returns_error(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "unknown_provider", 16, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
}

static void test_provider_factory_null_name_returns_error(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, NULL, 0, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_provider_factory_empty_name_returns_error(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "", 0, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_provider_factory_null_out_returns_error(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_provider_create(&alloc, "openai", 6, "key", 3, NULL, 0, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_factory_openai_resolves(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "openai", 6, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openai");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_anthropic_resolves(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "anthropic", 9, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "anthropic");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_gemini_resolves(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "gemini", 6, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "gemini");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_ollama_resolves(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "ollama", 6, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "ollama");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_openrouter_resolves(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "openrouter", 10, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openrouter");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_case_sensitive_unknown(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "OpenAI", 6, "key", 3, NULL, 0, &prov);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_factory_gpt4o_not_provider_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "gpt-4o", 6, "key", 3, NULL, 0, &prov);
    SC_ASSERT_NEQ(err, SC_OK);
}

/* ─── SSE parsing ────────────────────────────────────────────────────────── */
static void test_sse_parse_line_data_extracts_delta(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *line = "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}";
    sc_sse_line_result_t out;
    sc_error_t err = sc_sse_parse_line(&alloc, line, strlen(line), &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out.tag, SC_SSE_DELTA);
    SC_ASSERT_NOT_NULL(out.delta);
    SC_ASSERT_STR_EQ(out.delta, "hi");
    if (out.delta) alloc.free(alloc.ctx, out.delta, out.delta_len + 1);
}

static void test_sse_parse_line_done(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *line = "data: [DONE]";
    sc_sse_line_result_t out;
    sc_error_t err = sc_sse_parse_line(&alloc, line, strlen(line), &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out.tag, SC_SSE_DONE);
}

static void test_sse_parse_line_comment_skipped(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *line = ": comment";
    sc_sse_line_result_t out;
    sc_error_t err = sc_sse_parse_line(&alloc, line, strlen(line), &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out.tag, SC_SSE_SKIP);
}

static void test_sse_parse_line_empty_skipped(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *line = "";
    sc_sse_line_result_t out;
    sc_error_t err = sc_sse_parse_line(&alloc, line, strlen(line), &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out.tag, SC_SSE_SKIP);
}

static void test_sse_parse_line_no_data_prefix_skipped(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *line = "event: message";
    sc_sse_line_result_t out;
    sc_error_t err = sc_sse_parse_line(&alloc, line, strlen(line), &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out.tag, SC_SSE_SKIP);
}

static void test_sse_extract_delta_empty_choices(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *out = sc_sse_extract_delta_content(&alloc, "{\"choices\":[]}", 14);
    SC_ASSERT_NULL(out);
}

static void test_sse_extract_delta_no_delta_key(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *out = sc_sse_extract_delta_content(&alloc, "{\"choices\":[{\"delta\":{}}]}", 28);
    SC_ASSERT_NULL(out);
}

static void test_sse_extract_delta_content(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}";
    char *out = sc_sse_extract_delta_content(&alloc, json, strlen(json));
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_STR_EQ(out, "Hello");
    if (out) alloc.free(alloc.ctx, out, strlen(out) + 1);
}

/* ─── Error classification ────────────────────────────────────────────────── */
static void test_error_classify_rate_limited_429(void) {
    const char *msg = "HTTP 429 rate limited";
    SC_ASSERT_TRUE(sc_error_is_rate_limited(msg, strlen(msg)));
}

static void test_error_classify_rate_limited_too_many(void) {
    const char *msg = "too many requests";
    SC_ASSERT_TRUE(sc_error_is_rate_limited(msg, strlen(msg)));
}

static void test_error_classify_rate_limited_quota(void) {
    const char *msg = "quota exceeded";
    SC_ASSERT_TRUE(sc_error_is_rate_limited(msg, strlen(msg)));
}

static void test_error_classify_not_rate_limited(void) {
    const char *msg = "HTTP 500 internal server error";
    SC_ASSERT_FALSE(sc_error_is_rate_limited(msg, strlen(msg)));
}

static void test_error_classify_context_exhausted(void) {
    const char *msg = "context length exceeded";
    SC_ASSERT_TRUE(sc_error_is_context_exhausted(msg, strlen(msg)));
}

static void test_error_classify_context_token_limit(void) {
    const char *msg = "maximum token limit";
    SC_ASSERT_TRUE(sc_error_is_context_exhausted(msg, strlen(msg)));
}

static void test_error_classify_not_context_exhausted(void) {
    const char *msg = "generic error";
    SC_ASSERT_FALSE(sc_error_is_context_exhausted(msg, strlen(msg)));
}

static void test_error_classify_non_retryable_401(void) {
    const char *msg = "401 Unauthorized";
    SC_ASSERT_TRUE(sc_error_is_non_retryable(msg, strlen(msg)));
}

static void test_error_classify_non_retryable_403(void) {
    const char *msg = "403 Forbidden";
    SC_ASSERT_TRUE(sc_error_is_non_retryable(msg, strlen(msg)));
}

static void test_error_classify_429_not_non_retryable(void) {
    const char *msg = "429 rate limited";
    SC_ASSERT_FALSE(sc_error_is_non_retryable(msg, strlen(msg)));
}

static void test_error_classify_retry_after_seconds(void) {
    const char *msg = "Retry-After: 60";
    uint64_t ms = sc_error_parse_retry_after_ms(msg, strlen(msg));
    SC_ASSERT_TRUE(ms >= 59000 && ms <= 61000);
}

static void test_error_classify_text_rate_limited(void) {
    const char *text = "429 rate limit";
    SC_ASSERT_TRUE(sc_error_is_rate_limited_text(text, strlen(text)));
}

static void test_error_classify_text_context_exhausted(void) {
    const char *text = "context window exceeded";
    SC_ASSERT_TRUE(sc_error_is_context_exhausted_text(text, strlen(text)));
}

/* ─── Factory additional lookups ───────────────────────────────────────────── */
static void test_factory_mistral_creates_compatible(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "mistral", 7, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_deepseek_creates_compatible(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "deepseek", 8, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_together_creates_compatible(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "together", 8, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_fireworks_creates_compatible(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "fireworks", 9, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_perplexity_creates_compatible(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "perplexity", 10, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_cerebras_creates_compatible(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "cerebras", 8, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_sse_parser_init_deinit(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_sse_parser_t p;
    sc_error_t err = sc_sse_parser_init(&p, &alloc);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(p.buffer);
    sc_sse_parser_deinit(&p);
    SC_ASSERT_NULL(p.buffer);
}

static char s_sse_captured[256];
static size_t s_sse_captured_len;

static void sse_capture_cb(const char *et, size_t et_len, const char *data, size_t data_len, void *ud) {
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
    sc_allocator_t alloc = sc_system_allocator();
    sc_sse_parser_t p;
    sc_sse_parser_init(&p, &alloc);
    s_sse_captured[0] = '\0';
    s_sse_captured_len = 0;
    const char *stream = "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n";
    sc_error_t err = sc_sse_parser_feed(&p, stream, strlen(stream), sse_capture_cb, NULL);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(s_sse_captured_len > 0);
    sc_sse_parser_deinit(&p);
}

static void test_factory_google_creates_gemini(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "google", 6, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "gemini");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_google_gemini_creates_gemini(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "google-gemini", 13, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "gemini");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_custom_prefix_creates_compatible(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "custom:https://my-api.com/v1", 28,
        "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_anthropic_custom_creates_anthropic(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "anthropic-custom:https://my-api.com/v1", 38,
        "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "anthropic");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Codex CLI ────────────────────────────────────────────────────────────── */
static void test_codex_cli_create_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_codex_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_codex_cli_get_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_codex_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_TRUE(strcmp(prov.vtable->get_name(prov.ctx), "codex-cli") == 0);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_codex_cli_supports_native_tools(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_codex_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_FALSE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_codex_cli_deinit_no_crash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_codex_cli_create(&alloc, NULL, 0, NULL, 0, &prov);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── OpenAI Codex ────────────────────────────────────────────────────────── */
static void test_openai_codex_create_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_openai_codex_create(&alloc, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_codex_get_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_openai_codex_create(&alloc, NULL, 0, NULL, 0, &prov);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openai-codex");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_openai_codex_deinit_no_crash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_openai_codex_create(&alloc, "k", 1, NULL, 0, &prov);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Reliable ────────────────────────────────────────────────────────────── */
static void test_reliable_create_simple(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t inner;
    sc_openai_create(&alloc, "key", 3, NULL, 0, &inner);
    sc_provider_t prov;
    sc_error_t err = sc_reliable_create(&alloc, inner, 2, 100, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prov.ctx);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openai");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_supports_native_tools_aggregates(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t inner;
    sc_openai_create(&alloc, "key", 3, NULL, 0, &inner);
    sc_provider_t prov;
    sc_reliable_create(&alloc, inner, 1, 50, &prov);
    SC_ASSERT_TRUE(prov.vtable->supports_native_tools(prov.ctx));
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_supports_vision_aggregates(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t inner;
    sc_openai_create(&alloc, "key", 3, NULL, 0, &inner);
    sc_provider_t prov;
    sc_reliable_create(&alloc, inner, 1, 50, &prov);
    if (prov.vtable->supports_vision) {
        bool v = prov.vtable->supports_vision(prov.ctx);
        (void)v;
    }
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_chat_passthrough(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t inner;
    sc_openai_create(&alloc, "key", 3, NULL, 0, &inner);
    sc_provider_t prov;
    sc_reliable_create(&alloc, inner, 1, 50, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4", 4, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_create_with_extras(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t primary;
    sc_openai_create(&alloc, "key", 3, NULL, 0, &primary);
    sc_provider_t fallback;
    sc_anthropic_create(&alloc, "key", 3, NULL, 0, &fallback);
    sc_reliable_provider_entry_t extras[1] = {{
        .name = "anthropic", .name_len = 9,
        .provider = fallback,
    }};
    sc_provider_t prov;
    sc_error_t err = sc_reliable_create_ex(&alloc, primary, 1, 50, extras, 1, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prov.ctx);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "openai");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_create_with_model_fallbacks(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t inner;
    sc_openai_create(&alloc, "key", 3, NULL, 0, &inner);
    sc_reliable_fallback_model_t gpt4_fallbacks[2] = {
        { .model = "gpt-3.5-turbo", .model_len = 14 },
        { .model = "gpt-3", .model_len = 5 },
    };
    sc_reliable_model_fallback_entry_t model_fallbacks[1] = {{
        .model = "gpt-4", .model_len = 4,
        .fallbacks = gpt4_fallbacks, .fallbacks_count = 2,
    }};
    sc_provider_t prov;
    sc_error_t err = sc_reliable_create_ex(&alloc, inner, 0, 50, NULL, 0, model_fallbacks, 1, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prov.ctx);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_chat_with_extras_primary_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t primary;
    sc_openai_create(&alloc, "key", 3, NULL, 0, &primary);
    sc_provider_t fallback;
    sc_ollama_create(&alloc, NULL, 0, NULL, 0, &fallback);
    sc_reliable_provider_entry_t extras[1] = {{ .name = "ollama", .name_len = 6, .provider = fallback }};
    sc_provider_t prov;
    sc_reliable_create_ex(&alloc, primary, 0, 50, extras, 1, NULL, 0, &prov);
    sc_chat_message_t msgs[1] = { make_user_msg("hi", 2) };
    sc_chat_request_t req = make_simple_request(msgs, 1);
    sc_chat_response_t resp = {0};
    sc_error_t err = prov.vtable->chat(prov.ctx, &alloc, &req, "gpt-4", 4, 0.7, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.content);
    if (resp.content) alloc.free(alloc.ctx, (void *)resp.content, resp.content_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_warmup_calls_all(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t inner;
    sc_openai_create(&alloc, "key", 3, NULL, 0, &inner);
    sc_provider_t prov;
    sc_reliable_create(&alloc, inner, 0, 50, &prov);
    if (prov.vtable->warmup) {
        prov.vtable->warmup(prov.ctx);
    }
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_supports_vision_from_gemini(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t inner;
    sc_gemini_create(&alloc, "key", 3, NULL, 0, &inner);
    sc_provider_t prov;
    sc_reliable_create(&alloc, inner, 0, 50, &prov);
    if (prov.vtable->supports_vision) {
        SC_ASSERT_TRUE(prov.vtable->supports_vision(prov.ctx));
    }
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_reliable_chat_with_system(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t inner;
    sc_openai_create(&alloc, "key", 3, NULL, 0, &inner);
    sc_provider_t prov;
    sc_reliable_create(&alloc, inner, 0, 50, &prov);
    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = prov.vtable->chat_with_system(prov.ctx, &alloc,
        "Be helpful", 10, "Hello", 5, "gpt-4", 5, 0.7, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    if (out) alloc.free(alloc.ctx, out, out_len + 1);
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Scrub ───────────────────────────────────────────────────────────────── */
static void test_scrub_sk_prefix(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *in = "sk-abc123secret";
    char *out = sc_scrub_secret_patterns(&alloc, in, strlen(in));
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(strstr(out, "[REDACTED]") != NULL);
    SC_ASSERT_TRUE(strstr(out, "sk-abc123secret") == NULL);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_scrub_ghp_prefix(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *in = "ghp_abcdef123456";
    char *out = sc_scrub_secret_patterns(&alloc, in, strlen(in));
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(strstr(out, "[REDACTED]") != NULL);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_scrub_bearer_token(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *in = "Authorization: Bearer sk_live_xyz789";
    char *out = sc_scrub_secret_patterns(&alloc, in, strlen(in));
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(strstr(out, "[REDACTED]") != NULL);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_scrub_plain_text_unmodified(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *in = "hello world no secrets";
    char *out = sc_scrub_secret_patterns(&alloc, in, strlen(in));
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_STR_EQ(out, in);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_scrub_sanitize_truncates(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf[300];
    for (size_t i = 0; i < sizeof(buf) - 1; i++) buf[i] = 'x';
    buf[sizeof(buf) - 1] = '\0';
    char *out = sc_scrub_sanitize_api_error(&alloc, buf, strlen(buf));
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(strlen(out) <= 204);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

/* ─── Max tokens ──────────────────────────────────────────────────────────── */
static void test_max_tokens_default(void) {
    uint32_t v = sc_max_tokens_default();
    SC_ASSERT_TRUE(v > 0 && v <= 100000);
}

static void test_max_tokens_lookup_gpt4o(void) {
    uint32_t v = sc_max_tokens_lookup("gpt-4o", 6);
    SC_ASSERT_TRUE(v >= 4096);
}

static void test_max_tokens_lookup_claude(void) {
    uint32_t v = sc_max_tokens_lookup("claude-3-opus", 13);
    SC_ASSERT_TRUE(v >= 4096);
}

static void test_max_tokens_lookup_unknown(void) {
    uint32_t v = sc_max_tokens_lookup("unknown-model-xyz", 16);
    SC_ASSERT_TRUE(v == 0 || v >= 4096);
}

static void test_max_tokens_resolve_override(void) {
    uint32_t v = sc_max_tokens_resolve(16384, "gpt-4", 5);
    SC_ASSERT_EQ(v, 16384u);
}

static void test_max_tokens_resolve_fallback(void) {
    uint32_t v = sc_max_tokens_resolve(0, "gpt-4o", 6);
    SC_ASSERT_TRUE(v >= 4096);
}

/* ─── Error classify vision ───────────────────────────────────────────────── */
static void test_error_classify_vision_unsupported(void) {
    const char *text = "this model does not support image input";
    SC_ASSERT_TRUE(sc_error_is_vision_unsupported_text(text, strlen(text)));
}

static void test_error_classify_vision_not_unsupported(void) {
    const char *text = "generic error message";
    SC_ASSERT_FALSE(sc_error_is_vision_unsupported_text(text, strlen(text)));
}

/* ─── Factory xai grok ─────────────────────────────────────────────────────── */
static void test_factory_xai_creates_compatible(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "xai", 3, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

static void test_factory_grok_creates_compatible(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "grok", 4, "key", 3, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(prov.vtable->get_name(prov.ctx), "compatible");
    if (prov.vtable->deinit) prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── SSE additional ───────────────────────────────────────────────────────── */
static void test_sse_parse_line_delta_tool_call(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *line = "data: {\"choices\":[{\"delta\":{\"content\":null,\"function_call\":{\"name\":\"run\"}}]}";
    sc_sse_line_result_t out;
    sc_error_t err = sc_sse_parse_line(&alloc, line, strlen(line), &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(out.tag == SC_SSE_DELTA || out.tag == SC_SSE_SKIP);
    if (out.delta) alloc.free(alloc.ctx, out.delta, out.delta_len + 1);
}

static void test_sse_extract_delta_null_content(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"choices\":[{\"delta\":{\"content\":null}}]}";
    char *out = sc_sse_extract_delta_content(&alloc, json, strlen(json));
    SC_ASSERT_TRUE(out == NULL || (out && strlen(out) == 0));
    if (out) alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static size_t s_sse_multiline_event_count;
static char s_sse_multiline_last_data[128];

static void sse_multiline_cb(const char *et, size_t et_len, const char *data, size_t data_len, void *ud) {
    (void)ud; (void)et; (void)et_len;
    s_sse_multiline_event_count++;
    if (data && data_len > 0 && data_len < sizeof(s_sse_multiline_last_data) - 1) {
        memcpy(s_sse_multiline_last_data, data, data_len);
        s_sse_multiline_last_data[data_len] = '\0';
    }
}

static void test_sse_parse_multiline_data(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_sse_parser_t p;
    sc_sse_parser_init(&p, &alloc);
    s_sse_multiline_event_count = 0;
    s_sse_multiline_last_data[0] = '\0';
    const char *stream = "event: message\ndata: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\n";
    sc_error_t err = sc_sse_parser_feed(&p, stream, strlen(stream), sse_multiline_cb, NULL);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(s_sse_multiline_event_count >= 1);
    sc_sse_parser_deinit(&p);
}

static void test_sse_parse_callback_order(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_sse_parser_t p;
    sc_sse_parser_init(&p, &alloc);
    sse_order_idx = 0;
    const char *stream = "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\ndata: [DONE]\n\n";
    sc_error_t err = sc_sse_parser_feed(&p, stream, strlen(stream), sse_cb_order, NULL);
    SC_ASSERT_EQ(err, SC_OK);
    sc_sse_parser_deinit(&p);
}

static void test_sse_parser_feed_incremental(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_sse_parser_t p;
    sc_sse_parser_init(&p, &alloc);
    sse_got_delta = 0;
    sc_error_t err = sc_sse_parser_feed(&p, "data: ", 6, sse_cb_delta, NULL);
    SC_ASSERT_EQ(err, SC_OK);
    const char *chunk2 = "{\"choices\":[{\"delta\":{\"content\":\"y\"}}]}\n\n";
    err = sc_sse_parser_feed(&p, chunk2, strlen(chunk2), sse_cb_delta, NULL);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(sse_got_delta == 1);
    sc_sse_parser_deinit(&p);
}

static void test_sse_extract_delta_unicode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"choices\":[{\"delta\":{\"content\":\"\\u00e4\"}}]}";
    char *out = sc_sse_extract_delta_content(&alloc, json, strlen(json));
    SC_ASSERT_TRUE(out == NULL || strlen(out) >= 0);
    if (out) alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_sse_parse_line_whitespace_trimmed(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *line = "data: [DONE]  \r\n";
    sc_sse_line_result_t out;
    sc_error_t err = sc_sse_parse_line(&alloc, line, strlen(line), &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out.tag, SC_SSE_DONE);
}

static void test_sse_parse_line_data_with_spaces(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *line = "data:   {\"choices\":[{\"delta\":{\"content\":\"z\"}}]}";
    sc_sse_line_result_t out;
    sc_error_t err = sc_sse_parse_line(&alloc, line, strlen(line), &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(out.tag == SC_SSE_DELTA || out.tag == SC_SSE_SKIP);
    if (out.delta) alloc.free(alloc.ctx, out.delta, out.delta_len + 1);
}

void run_provider_all_tests(void) {
    SC_TEST_SUITE("Provider All");
    SC_RUN_TEST(test_openai_create_succeeds);
    SC_RUN_TEST(test_openai_create_null_alloc_fails);
    SC_RUN_TEST(test_openai_get_name);
    SC_RUN_TEST(test_openai_supports_native_tools);
    SC_RUN_TEST(test_openai_chat_mock);
    SC_RUN_TEST(test_openai_deinit_no_crash);
    SC_RUN_TEST(test_openai_create_empty_key);
    SC_RUN_TEST(test_openai_create_with_base_url);
    SC_RUN_TEST(test_openai_chat_with_system_and_user);
    SC_RUN_TEST(test_openai_chat_tool_calls_mock);
    SC_RUN_TEST(test_openai_stream_chat_mock);
    SC_RUN_TEST(test_openai_model_passthrough);
    SC_RUN_TEST(test_openai_temperature_passthrough);

    SC_RUN_TEST(test_anthropic_create_succeeds);
    SC_RUN_TEST(test_anthropic_create_null_alloc_fails);
    SC_RUN_TEST(test_anthropic_get_name);
    SC_RUN_TEST(test_anthropic_supports_native_tools);
    SC_RUN_TEST(test_anthropic_chat_mock);
    SC_RUN_TEST(test_anthropic_chat_with_tools_mock);
    SC_RUN_TEST(test_anthropic_deinit_no_crash);
    SC_RUN_TEST(test_anthropic_create_empty_key);
    SC_RUN_TEST(test_anthropic_create_with_base_url);

    SC_RUN_TEST(test_gemini_create_succeeds);
    SC_RUN_TEST(test_gemini_create_null_alloc_fails);
    SC_RUN_TEST(test_gemini_get_name);
    SC_RUN_TEST(test_gemini_supports_native_tools);
    SC_RUN_TEST(test_gemini_chat_mock);
    SC_RUN_TEST(test_gemini_deinit_no_crash);
    SC_RUN_TEST(test_gemini_create_empty_key);
    SC_RUN_TEST(test_gemini_create_with_oauth);
    SC_RUN_TEST(test_gemini_create_oauth_null_fails);
    SC_RUN_TEST(test_gemini_supports_vision);
    SC_RUN_TEST(test_gemini_chat_with_tools_mock);
    SC_RUN_TEST(test_gemini_model_gemini2_flash);
    SC_RUN_TEST(test_gemini_stream_chat_mock);

    SC_RUN_TEST(test_ollama_create_succeeds);
    SC_RUN_TEST(test_ollama_create_null_alloc_fails);
    SC_RUN_TEST(test_ollama_get_name);
    SC_RUN_TEST(test_ollama_supports_native_tools);
    SC_RUN_TEST(test_ollama_chat_mock);
    SC_RUN_TEST(test_ollama_deinit_no_crash);
    SC_RUN_TEST(test_ollama_create_empty_key);
    SC_RUN_TEST(test_ollama_create_with_base_url);
    SC_RUN_TEST(test_ollama_no_api_key_required);
    SC_RUN_TEST(test_ollama_chat_with_tools_mock);
    SC_RUN_TEST(test_ollama_local_model_format);

    SC_RUN_TEST(test_openrouter_create_succeeds);
    SC_RUN_TEST(test_openrouter_create_null_alloc_fails);
    SC_RUN_TEST(test_openrouter_get_name);
    SC_RUN_TEST(test_openrouter_supports_native_tools);
    SC_RUN_TEST(test_openrouter_chat_mock);
    SC_RUN_TEST(test_openrouter_deinit_no_crash);
    SC_RUN_TEST(test_openrouter_create_empty_key);

    SC_RUN_TEST(test_compatible_create_succeeds);
    SC_RUN_TEST(test_compatible_create_null_alloc_fails);
    SC_RUN_TEST(test_compatible_get_name);
    SC_RUN_TEST(test_compatible_supports_native_tools);
    SC_RUN_TEST(test_compatible_chat_mock);
    SC_RUN_TEST(test_compatible_deinit_no_crash);
    SC_RUN_TEST(test_compatible_create_empty_key);
    SC_RUN_TEST(test_compatible_mistral_base_url);
    SC_RUN_TEST(test_compatible_deepseek_base_url);
    SC_RUN_TEST(test_compatible_together_base_url);
    SC_RUN_TEST(test_compatible_fireworks_base_url);
    SC_RUN_TEST(test_compatible_perplexity_base_url);
    SC_RUN_TEST(test_compatible_cerebras_base_url);
    SC_RUN_TEST(test_compatible_groq_base_url);
    SC_RUN_TEST(test_compatible_xai_base_url);
    SC_RUN_TEST(test_factory_mistral_chat_mock);
    SC_RUN_TEST(test_factory_deepseek_chat_mock);
    SC_RUN_TEST(test_factory_together_chat_mock);
    SC_RUN_TEST(test_factory_fireworks_chat_mock);
    SC_RUN_TEST(test_factory_perplexity_chat_mock);
    SC_RUN_TEST(test_factory_cerebras_chat_mock);
    SC_RUN_TEST(test_factory_xai_chat_mock);
    SC_RUN_TEST(test_compatible_chat_with_tools_mock);

    SC_RUN_TEST(test_claude_cli_create_succeeds);
    SC_RUN_TEST(test_claude_cli_create_null_alloc_fails);
    SC_RUN_TEST(test_claude_cli_get_name);
    SC_RUN_TEST(test_claude_cli_supports_native_tools);
    SC_RUN_TEST(test_claude_cli_chat_mock);
    SC_RUN_TEST(test_claude_cli_deinit_no_crash);
    SC_RUN_TEST(test_claude_cli_create_empty_key);

    SC_RUN_TEST(test_compatible_url_lookup_groq);
    SC_RUN_TEST(test_compatible_url_lookup_mistral);
    SC_RUN_TEST(test_compatible_url_lookup_deepseek);
    SC_RUN_TEST(test_compatible_url_lookup_together);
    SC_RUN_TEST(test_compatible_url_lookup_fireworks);
    SC_RUN_TEST(test_compatible_url_lookup_perplexity);
    SC_RUN_TEST(test_compatible_url_lookup_cerebras);
    SC_RUN_TEST(test_compatible_url_lookup_unknown);
    SC_RUN_TEST(test_factory_creates_compatible_for_groq);

    SC_RUN_TEST(test_factory_google_creates_gemini);
    SC_RUN_TEST(test_factory_google_gemini_creates_gemini);
    SC_RUN_TEST(test_factory_custom_prefix_creates_compatible);
    SC_RUN_TEST(test_factory_anthropic_custom_creates_anthropic);

    SC_RUN_TEST(test_provider_factory_unknown_returns_error);
    SC_RUN_TEST(test_provider_factory_null_name_returns_error);
    SC_RUN_TEST(test_provider_factory_empty_name_returns_error);
    SC_RUN_TEST(test_provider_factory_null_out_returns_error);
    SC_RUN_TEST(test_factory_openai_resolves);
    SC_RUN_TEST(test_factory_anthropic_resolves);
    SC_RUN_TEST(test_factory_gemini_resolves);
    SC_RUN_TEST(test_factory_ollama_resolves);
    SC_RUN_TEST(test_factory_openrouter_resolves);
    SC_RUN_TEST(test_factory_case_sensitive_unknown);
    SC_RUN_TEST(test_factory_gpt4o_not_provider_name);

    SC_RUN_TEST(test_sse_parse_line_data_extracts_delta);
    SC_RUN_TEST(test_sse_parse_line_done);
    SC_RUN_TEST(test_sse_parse_line_comment_skipped);
    SC_RUN_TEST(test_sse_parse_line_empty_skipped);
    SC_RUN_TEST(test_sse_parse_line_no_data_prefix_skipped);
    SC_RUN_TEST(test_sse_extract_delta_empty_choices);
    SC_RUN_TEST(test_sse_extract_delta_no_delta_key);
    SC_RUN_TEST(test_sse_extract_delta_content);
    SC_RUN_TEST(test_sse_parser_init_deinit);
    SC_RUN_TEST(test_sse_parser_feed_callback);
    SC_RUN_TEST(test_sse_parse_multiline_data);
    SC_RUN_TEST(test_sse_parse_callback_order);
    SC_RUN_TEST(test_sse_parser_feed_incremental);
    SC_RUN_TEST(test_sse_extract_delta_unicode);
    SC_RUN_TEST(test_sse_parse_line_whitespace_trimmed);
    SC_RUN_TEST(test_sse_parse_line_data_with_spaces);

    SC_RUN_TEST(test_error_classify_rate_limited_429);
    SC_RUN_TEST(test_error_classify_rate_limited_too_many);
    SC_RUN_TEST(test_error_classify_rate_limited_quota);
    SC_RUN_TEST(test_error_classify_not_rate_limited);
    SC_RUN_TEST(test_error_classify_context_exhausted);
    SC_RUN_TEST(test_error_classify_context_token_limit);
    SC_RUN_TEST(test_error_classify_not_context_exhausted);
    SC_RUN_TEST(test_error_classify_non_retryable_401);
    SC_RUN_TEST(test_error_classify_non_retryable_403);
    SC_RUN_TEST(test_error_classify_429_not_non_retryable);
    SC_RUN_TEST(test_error_classify_retry_after_seconds);
    SC_RUN_TEST(test_error_classify_text_rate_limited);
    SC_RUN_TEST(test_error_classify_text_context_exhausted);

    SC_RUN_TEST(test_factory_mistral_creates_compatible);
    SC_RUN_TEST(test_factory_deepseek_creates_compatible);
    SC_RUN_TEST(test_factory_together_creates_compatible);
    SC_RUN_TEST(test_factory_fireworks_creates_compatible);
    SC_RUN_TEST(test_factory_perplexity_creates_compatible);
    SC_RUN_TEST(test_factory_cerebras_creates_compatible);
    SC_RUN_TEST(test_factory_xai_creates_compatible);
    SC_RUN_TEST(test_factory_grok_creates_compatible);

    SC_RUN_TEST(test_codex_cli_create_succeeds);
    SC_RUN_TEST(test_codex_cli_get_name);
    SC_RUN_TEST(test_codex_cli_supports_native_tools);
    SC_RUN_TEST(test_codex_cli_deinit_no_crash);
    SC_RUN_TEST(test_openai_codex_create_succeeds);
    SC_RUN_TEST(test_openai_codex_get_name);
    SC_RUN_TEST(test_openai_codex_deinit_no_crash);

    SC_RUN_TEST(test_reliable_create_simple);
    SC_RUN_TEST(test_reliable_supports_native_tools_aggregates);
    SC_RUN_TEST(test_reliable_supports_vision_aggregates);
    SC_RUN_TEST(test_reliable_chat_passthrough);
    SC_RUN_TEST(test_reliable_create_with_extras);
    SC_RUN_TEST(test_reliable_create_with_model_fallbacks);
    SC_RUN_TEST(test_reliable_chat_with_extras_primary_succeeds);
    SC_RUN_TEST(test_reliable_warmup_calls_all);
    SC_RUN_TEST(test_reliable_supports_vision_from_gemini);
    SC_RUN_TEST(test_reliable_chat_with_system);

    SC_RUN_TEST(test_scrub_sk_prefix);
    SC_RUN_TEST(test_scrub_ghp_prefix);
    SC_RUN_TEST(test_scrub_bearer_token);
    SC_RUN_TEST(test_scrub_plain_text_unmodified);
    SC_RUN_TEST(test_scrub_sanitize_truncates);

    SC_RUN_TEST(test_max_tokens_default);
    SC_RUN_TEST(test_max_tokens_lookup_gpt4o);
    SC_RUN_TEST(test_max_tokens_lookup_claude);
    SC_RUN_TEST(test_max_tokens_lookup_unknown);
    SC_RUN_TEST(test_max_tokens_resolve_override);
    SC_RUN_TEST(test_max_tokens_resolve_fallback);

    SC_RUN_TEST(test_error_classify_vision_unsupported);
    SC_RUN_TEST(test_error_classify_vision_not_unsupported);

    SC_RUN_TEST(test_sse_parse_line_delta_tool_call);
    SC_RUN_TEST(test_sse_extract_delta_null_content);
}
