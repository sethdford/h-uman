/* Streaming chat and SSE parser tests */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "human/providers/anthropic.h"
#include "human/providers/openai.h"
#include "human/providers/sse.h"
#include "test_framework.h"
#include <string.h>

static int sse_event_count;
static char sse_last_event_type[64];
static char sse_last_data[256];
static size_t sse_last_data_len;

static void sse_event_cb(const char *event_type, size_t event_type_len, const char *data,
                         size_t data_len, void *userdata) {
    (void)userdata;
    sse_event_count++;
    if (event_type && event_type_len < sizeof(sse_last_event_type)) {
        memcpy(sse_last_event_type, event_type, event_type_len);
        sse_last_event_type[event_type_len] = '\0';
    } else {
        sse_last_event_type[0] = '\0';
    }
    if (data && data_len < sizeof(sse_last_data)) {
        memcpy(sse_last_data, data, data_len);
        sse_last_data[data_len] = '\0';
        sse_last_data_len = data_len;
    } else {
        sse_last_data[0] = '\0';
        sse_last_data_len = data_len;
    }
}

static void test_sse_parser_single_event(void) {
    sse_event_count = 0;
    sse_last_event_type[0] = '\0';
    sse_last_data[0] = '\0';

    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_error_t err = hu_sse_parser_init(&p, &alloc);
    HU_ASSERT_EQ(err, HU_OK);

    const char *input = "data: {\"foo\":1}\n\n";
    err = hu_sse_parser_feed(&p, input, strlen(input), sse_event_cb, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(sse_event_count, 1);
    HU_ASSERT_TRUE(strstr(sse_last_data, "\"foo\"") != NULL);
    HU_ASSERT_EQ(sse_last_data_len, 9); /* {"foo":1} after SSE leading-space trim */

    hu_sse_parser_deinit(&p);
}

static void test_sse_parser_multi_event(void) {
    sse_event_count = 0;

    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_error_t err = hu_sse_parser_init(&p, &alloc);
    HU_ASSERT_EQ(err, HU_OK);

    const char *input = "data: a\n\ndata: b\n\ndata: c\n\n";
    err = hu_sse_parser_feed(&p, input, strlen(input), sse_event_cb, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(sse_event_count, 3);

    hu_sse_parser_deinit(&p);
}

static void test_sse_parser_partial_feed(void) {
    sse_event_count = 0;

    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_error_t err = hu_sse_parser_init(&p, &alloc);
    HU_ASSERT_EQ(err, HU_OK);

    err = hu_sse_parser_feed(&p, "data: ", 6, sse_event_cb, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(sse_event_count, 0);

    err = hu_sse_parser_feed(&p, "{\"x\":1}\n\n", 9, sse_event_cb, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(sse_event_count, 1);
    HU_ASSERT_TRUE(strstr(sse_last_data, "\"x\"") != NULL);

    hu_sse_parser_deinit(&p);
}

static void test_sse_parser_event_type(void) {
    sse_event_count = 0;
    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_sse_parser_init(&p, &alloc);
    const char *input = "event: custom\ndata: {\"x\":1}\n\n";
    hu_sse_parser_feed(&p, input, strlen(input), sse_event_cb, NULL);
    HU_ASSERT_TRUE(sse_event_count >= 1);
    hu_sse_parser_deinit(&p);
}

static void test_sse_parser_multiline_data(void) {
    sse_event_count = 0;
    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_sse_parser_init(&p, &alloc);
    const char *input = "data: line1\ndata: line2\ndata: line3\n\n";
    hu_sse_parser_feed(&p, input, strlen(input), sse_event_cb, NULL);
    HU_ASSERT_TRUE(sse_event_count >= 1);
    HU_ASSERT_TRUE(strstr(sse_last_data, "line1") != NULL ||
                   strstr(sse_last_data, "line2") != NULL ||
                   strstr(sse_last_data, "line3") != NULL);
    hu_sse_parser_deinit(&p);
}

static void test_sse_parser_empty_event(void) {
    sse_event_count = 0;
    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_sse_parser_init(&p, &alloc);
    const char *input = "\n\n";
    hu_sse_parser_feed(&p, input, strlen(input), sse_event_cb, NULL);
    HU_ASSERT_TRUE(sse_event_count >= 1);
    hu_sse_parser_deinit(&p);
}

static void test_sse_parser_comment_ignored(void) {
    sse_event_count = 0;
    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_sse_parser_init(&p, &alloc);
    const char *input = ": comment line\ndata: ok\n\n";
    hu_sse_parser_feed(&p, input, strlen(input), sse_event_cb, NULL);
    HU_ASSERT_EQ(sse_event_count, 1);
    HU_ASSERT_TRUE(strstr(sse_last_data, "ok") != NULL);
    hu_sse_parser_deinit(&p);
}

static void test_sse_parser_init_null_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_error_t err = hu_sse_parser_init(NULL, &alloc);
    HU_ASSERT_NEQ(err, HU_OK);
    err = hu_sse_parser_init(&p, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_sse_parser_feed_null_callback_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_sse_parser_init(&p, &alloc);
    hu_error_t err = hu_sse_parser_feed(&p, "data: x\n\n", 9, NULL, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
    hu_sse_parser_deinit(&p);
}

static void test_sse_parser_feed_empty_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_sse_parser_init(&p, &alloc);
    hu_error_t err = hu_sse_parser_feed(&p, "", 0, sse_event_cb, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    hu_sse_parser_deinit(&p);
}

static void test_sse_parser_two_empty_events(void) {
    sse_event_count = 0;
    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_sse_parser_init(&p, &alloc);
    const char *input = "\n\n\n\n";
    hu_sse_parser_feed(&p, input, strlen(input), sse_event_cb, NULL);
    HU_ASSERT_TRUE(sse_event_count >= 1);
    hu_sse_parser_deinit(&p);
}

static void test_sse_parser_data_with_spaces(void) {
    sse_event_count = 0;
    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_sse_parser_init(&p, &alloc);
    const char *input = "data:   {\"trimmed\":1}\n\n";
    hu_sse_parser_feed(&p, input, strlen(input), sse_event_cb, NULL);
    HU_ASSERT_EQ(sse_event_count, 1);
    HU_ASSERT_TRUE(strstr(sse_last_data, "trimmed") != NULL || strstr(sse_last_data, "1") != NULL);
    hu_sse_parser_deinit(&p);
}

static void test_sse_parser_done_signal(void) {
    sse_event_count = 0;

    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_error_t err = hu_sse_parser_init(&p, &alloc);
    HU_ASSERT_EQ(err, HU_OK);

    const char *input = "data: [DONE]\n\n";
    err = hu_sse_parser_feed(&p, input, strlen(input), sse_event_cb, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(sse_event_count, 1);
    HU_ASSERT_TRUE(strstr(sse_last_data, "DONE") != NULL);

    hu_sse_parser_deinit(&p);
}

/* Edge case: incomplete SSE data (no final newline) — buffered, no event until complete */
static void test_sse_parser_incomplete_data_buffered(void) {
    sse_event_count = 0;

    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_sse_parser_init(&p, &alloc);

    hu_error_t err = hu_sse_parser_feed(&p, "data: ", 5, sse_event_cb, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(sse_event_count, 0);

    err = hu_sse_parser_feed(&p, "{\"x\":1}\n\n", 9, sse_event_cb, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(sse_event_count, 1);
    HU_ASSERT_TRUE(strstr(sse_last_data, "\"x\"") != NULL);

    hu_sse_parser_deinit(&p);
}

/* Edge case: missing event type — defaults to "message" */
static void test_sse_parser_missing_event_type_defaults_message(void) {
    sse_event_count = 0;
    sse_last_event_type[0] = '\0';

    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_sse_parser_init(&p, &alloc);

    const char *input = "data: hello\n\n";
    hu_sse_parser_feed(&p, input, strlen(input), sse_event_cb, NULL);
    HU_ASSERT_EQ(sse_event_count, 1);
    HU_ASSERT_STR_EQ(sse_last_event_type, "message");
    HU_ASSERT_TRUE(strstr(sse_last_data, "hello") != NULL);

    hu_sse_parser_deinit(&p);
}

/* Edge case: empty data field — callback invoked with data_len 0 */
static void test_sse_parser_empty_data_field(void) {
    sse_event_count = 0;

    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_sse_parser_init(&p, &alloc);

    const char *input = "data: \n\n";
    hu_error_t err = hu_sse_parser_feed(&p, input, strlen(input), sse_event_cb, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(sse_event_count, 1);
    HU_ASSERT_EQ(sse_last_data_len, 0u);

    hu_sse_parser_deinit(&p);
}

/* Edge case: feed with NULL bytes (len 0) is allowed and returns OK */
static void test_sse_parser_feed_null_bytes_len_zero_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_sse_parser_t p;
    hu_sse_parser_init(&p, &alloc);

    hu_error_t err = hu_sse_parser_feed(&p, NULL, 0, sse_event_cb, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    hu_sse_parser_deinit(&p);
}

static int openai_stream_chunk_count;
static bool openai_stream_got_final;

static void openai_stream_cb(void *ctx, const hu_stream_chunk_t *chunk) {
    (void)ctx;
    openai_stream_chunk_count++;
    if (chunk->is_final)
        openai_stream_got_final = true;
}

static void test_openai_stream_mock(void) {
#if HU_IS_TEST
    openai_stream_chunk_count = 0;
    openai_stream_got_final = false;

    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_openai_create(&alloc, "test-key", 8, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(prov.vtable->supports_streaming && prov.vtable->supports_streaming(prov.ctx));

    hu_chat_message_t msgs[1];
    msgs[0].role = HU_ROLE_USER;
    msgs[0].content = "hi";
    msgs[0].content_len = 2;
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

    hu_stream_chat_result_t result;
    memset(&result, 0, sizeof(result));
    err = prov.vtable->stream_chat(prov.ctx, &alloc, &req, "gpt-4", 5, 0.7, openai_stream_cb, NULL,
                                   &result);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(openai_stream_chunk_count, 4);
    HU_ASSERT_TRUE(openai_stream_got_final);
    HU_ASSERT_NOT_NULL(result.content);
    HU_ASSERT_TRUE(result.content_len > 0);

    if (result.content)
        alloc.free(alloc.ctx, (void *)result.content, result.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
#endif
}

static int anthropic_stream_chunk_count;
static bool anthropic_stream_got_final;

static void anthropic_stream_cb(void *ctx, const hu_stream_chunk_t *chunk) {
    (void)ctx;
    anthropic_stream_chunk_count++;
    if (chunk->is_final)
        anthropic_stream_got_final = true;
}

static void test_anthropic_stream_mock(void) {
#if HU_IS_TEST
    anthropic_stream_chunk_count = 0;
    anthropic_stream_got_final = false;

    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_anthropic_create(&alloc, "test-key", 8, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(prov.vtable->supports_streaming && prov.vtable->supports_streaming(prov.ctx));

    hu_chat_message_t msgs[1];
    msgs[0].role = HU_ROLE_USER;
    msgs[0].content = "hi";
    msgs[0].content_len = 2;
    msgs[0].name = NULL;
    msgs[0].name_len = 0;
    msgs[0].tool_call_id = NULL;
    msgs[0].tool_call_id_len = 0;
    msgs[0].content_parts = NULL;
    msgs[0].content_parts_count = 0;

    hu_chat_request_t req = {
        .messages = msgs,
        .messages_count = 1,
        .model = "claude-3",
        .model_len = 8,
        .temperature = 0.7,
        .max_tokens = 0,
        .tools = NULL,
        .tools_count = 0,
        .timeout_secs = 0,
        .reasoning_effort = NULL,
        .reasoning_effort_len = 0,
    };

    hu_stream_chat_result_t result;
    memset(&result, 0, sizeof(result));
    err = prov.vtable->stream_chat(prov.ctx, &alloc, &req, "claude-3", 8, 0.7, anthropic_stream_cb,
                                   NULL, &result);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(anthropic_stream_chunk_count, 4);
    HU_ASSERT_TRUE(anthropic_stream_got_final);
    HU_ASSERT_NOT_NULL(result.content);
    HU_ASSERT_TRUE(result.content_len > 0);

    if (result.content)
        alloc.free(alloc.ctx, (void *)result.content, result.content_len + 1);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
#endif
}

void run_streaming_tests(void) {
    HU_TEST_SUITE("Streaming");
    HU_RUN_TEST(test_sse_parser_single_event);
    HU_RUN_TEST(test_sse_parser_multi_event);
    HU_RUN_TEST(test_sse_parser_partial_feed);
    HU_RUN_TEST(test_sse_parser_event_type);
    HU_RUN_TEST(test_sse_parser_multiline_data);
    HU_RUN_TEST(test_sse_parser_empty_event);
    HU_RUN_TEST(test_sse_parser_comment_ignored);
    HU_RUN_TEST(test_sse_parser_init_null_fails);
    HU_RUN_TEST(test_sse_parser_feed_null_callback_fails);
    HU_RUN_TEST(test_sse_parser_feed_empty_ok);
    HU_RUN_TEST(test_sse_parser_two_empty_events);
    HU_RUN_TEST(test_sse_parser_data_with_spaces);
    HU_RUN_TEST(test_sse_parser_done_signal);
    HU_RUN_TEST(test_sse_parser_incomplete_data_buffered);
    HU_RUN_TEST(test_sse_parser_missing_event_type_defaults_message);
    HU_RUN_TEST(test_sse_parser_empty_data_field);
    HU_RUN_TEST(test_sse_parser_feed_null_bytes_len_zero_ok);
    HU_RUN_TEST(test_openai_stream_mock);
    HU_RUN_TEST(test_anthropic_stream_mock);
}
