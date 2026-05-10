#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/provider.h"
#include "test_framework.h"

#include <string.h>

typedef struct {
    int calls;
} retry_provider_ctx_t;

static const char *retry_provider_name(void *ctx) {
    (void)ctx;
    return "retry_guard_mock";
}

static bool retry_provider_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}

static bool retry_provider_supports_streaming(void *ctx) {
    (void)ctx;
    return true;
}

static void retry_provider_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static hu_error_t retry_provider_chat(void *ctx, hu_allocator_t *alloc,
                                      const hu_chat_request_t *request, const char *model,
                                      size_t model_len, double temperature,
                                      hu_chat_response_t *out) {
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    retry_provider_ctx_t *r = (retry_provider_ctx_t *)ctx;
    r->calls++;

    const char *text = NULL;
    if (r->calls == 1) {
        text =
            "Like <|channel>thoughtThe user said said \"Here! \" \" \" \" \" \" \" \" \" \" \" "
            "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
            "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
            "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
            "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" ";
    } else {
        text = "haha yeah, fair 😂";
    }

    out->content = hu_strndup(alloc, text, strlen(text));
    out->content_len = out->content ? strlen(text) : 0;
    out->tool_calls = NULL;
    out->tool_calls_count = 0;
    out->reasoning_content = NULL;
    out->reasoning_content_len = 0;
    return out->content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static hu_error_t retry_provider_stream_chat(void *ctx, hu_allocator_t *alloc,
                                             const hu_chat_request_t *request, const char *model,
                                             size_t model_len, double temperature,
                                             hu_stream_callback_t callback, void *callback_ctx,
                                             hu_stream_chat_result_t *out) {
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    retry_provider_ctx_t *r = (retry_provider_ctx_t *)ctx;
    r->calls++;

    const char *text =
        "Like <|channel>thoughtThe user said said \"Here! \" \" \" \" \" \" \" \" \" \" \" "
        "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
        "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" ";

    if (callback) {
        hu_stream_chunk_t chunk;
        memset(&chunk, 0, sizeof(chunk));
        chunk.type = HU_STREAM_CONTENT;
        chunk.delta = text;
        chunk.delta_len = strlen(text);
        callback(callback_ctx, &chunk);
    }

    out->content = hu_strndup(alloc, text, strlen(text));
    out->content_len = out->content ? strlen(text) : 0;
    out->tool_calls = NULL;
    out->tool_calls_count = 0;
    return out->content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static const hu_provider_vtable_t retry_provider_vtable = {
    .chat = retry_provider_chat,
    .supports_native_tools = retry_provider_supports_native_tools,
    .get_name = retry_provider_name,
    .deinit = retry_provider_deinit,
    .supports_streaming = retry_provider_supports_streaming,
    .stream_chat = retry_provider_stream_chat,
};

static hu_provider_t retry_provider_create(retry_provider_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    return (hu_provider_t){.ctx = ctx, .vtable = &retry_provider_vtable};
}

static void guard_reject_retry_produces_human_like_replacement(void) {
    hu_allocator_t alloc = hu_system_allocator();
    retry_provider_ctx_t provider_ctx;
    hu_provider_t provider = retry_provider_create(&provider_ctx);

    hu_agent_t agent;
    hu_error_t err = hu_agent_from_config(&agent, &alloc, provider, NULL, 0, NULL, NULL, NULL,
                                          NULL, "test-model", 10, "retry_guard_mock", 16, 0.7,
                                          "/tmp", 4, 5, 50, false, 1, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    agent.active_channel = "imessage";
    agent.active_channel_len = 8;

    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn(&agent, "haha i figured 😂", strlen("haha i figured 😂"), &response,
                        &response_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(response);
    HU_ASSERT_EQ(provider_ctx.calls, 2);
    HU_ASSERT(strstr(response, "<|") == NULL);
    HU_ASSERT(strstr(response, "\" \" \"") == NULL);
    HU_ASSERT_EQ(response_len, strlen("haha yeah, fair 😂"));
    HU_ASSERT_EQ(memcmp(response, "haha yeah, fair 😂", response_len), 0);

    alloc.free(alloc.ctx, response, response_len + 1);
    hu_agent_deinit(&agent);
}

static void stream_guard_reject_retry_produces_human_like_replacement(void) {
    hu_allocator_t alloc = hu_system_allocator();
    retry_provider_ctx_t provider_ctx;
    hu_provider_t provider = retry_provider_create(&provider_ctx);

    hu_agent_t agent;
    hu_error_t err = hu_agent_from_config(&agent, &alloc, provider, NULL, 0, NULL, NULL, NULL,
                                          NULL, "test-model", 10, "retry_guard_mock", 16, 0.7,
                                          "/tmp", 4, 5, 50, false, 1, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    agent.active_channel = "imessage";
    agent.active_channel_len = 8;

    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn_stream_v2(&agent, "haha i figured 😂", strlen("haha i figured 😂"), NULL,
                                  NULL, &response, &response_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(response);
    HU_ASSERT_EQ(provider_ctx.calls, 2);
    HU_ASSERT(strstr(response, "<|") == NULL);
    HU_ASSERT(strstr(response, "\" \" \"") == NULL);
    HU_ASSERT_EQ(response_len, strlen("haha yeah, fair 😂"));
    HU_ASSERT_EQ(memcmp(response, "haha yeah, fair 😂", response_len), 0);

    alloc.free(alloc.ctx, response, response_len + 1);
    hu_agent_deinit(&agent);
}

typedef struct {
    char seen[512];
    size_t seen_len;
} retry_stream_events_t;

static void retry_collect_stream_event(const hu_agent_stream_event_t *event, void *ctx) {
    retry_stream_events_t *events = (retry_stream_events_t *)ctx;
    if (!event || event->type != HU_AGENT_STREAM_TEXT || !event->data)
        return;
    size_t room = sizeof(events->seen) - events->seen_len - 1;
    size_t n = event->data_len < room ? event->data_len : room;
    if (n > 0) {
        memcpy(events->seen + events->seen_len, event->data, n);
        events->seen_len += n;
        events->seen[events->seen_len] = '\0';
    }
}

static void stream_guard_buffers_raw_output_until_retry_passes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    retry_provider_ctx_t provider_ctx;
    hu_provider_t provider = retry_provider_create(&provider_ctx);

    hu_agent_t agent;
    hu_error_t err = hu_agent_from_config(&agent, &alloc, provider, NULL, 0, NULL, NULL, NULL,
                                          NULL, "test-model", 10, "retry_guard_mock", 16, 0.7,
                                          "/tmp", 4, 5, 50, false, 1, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    agent.active_channel = "imessage";
    agent.active_channel_len = 8;

    retry_stream_events_t events;
    memset(&events, 0, sizeof(events));
    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn_stream_v2(&agent, "haha i figured 😂", strlen("haha i figured 😂"),
                                  retry_collect_stream_event, &events, &response, &response_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(response);
    HU_ASSERT_EQ(provider_ctx.calls, 2);
    HU_ASSERT(strstr(response, "<|") == NULL);
    HU_ASSERT(strstr(response, "\" \" \"") == NULL);
    HU_ASSERT(strstr(events.seen, "<|") == NULL);
    HU_ASSERT(strstr(events.seen, "\" \" \"") == NULL);
    HU_ASSERT_EQ(events.seen_len, strlen("haha yeah, fair 😂"));
    HU_ASSERT_EQ(memcmp(events.seen, "haha yeah, fair 😂", events.seen_len), 0);

    alloc.free(alloc.ctx, response, response_len + 1);
    hu_agent_deinit(&agent);
}

void run_response_guard_retry_tests(void) {
    HU_TEST_SUITE("Response Guard Retry");
    HU_RUN_TEST(guard_reject_retry_produces_human_like_replacement);
    HU_RUN_TEST(stream_guard_reject_retry_produces_human_like_replacement);
    HU_RUN_TEST(stream_guard_buffers_raw_output_until_retry_passes);
}
