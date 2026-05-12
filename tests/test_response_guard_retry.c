#include "human/agent.h"
#include "human/agent/response_guard.h"
#include "human/agent/response_guard_retry.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/persona.h"
#include "human/provider.h"
#include "test_framework.h"

#include <string.h>

typedef struct {
    int calls;
    /* Captured system prompt from the retry call (call #2) — lets tests
     * assert that the slim retry path actually splices the persona/style
     * hint into the system message. */
    char captured_retry_sys[4096];
    size_t captured_retry_sys_len;
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
    (void)model;
    (void)model_len;
    (void)temperature;
    retry_provider_ctx_t *r = (retry_provider_ctx_t *)ctx;
    r->calls++;

    /* Capture the SECOND call's system prompt — that's the slim retry. */
    if (r->calls == 2 && request && request->messages && request->messages_count > 0 &&
        request->messages[0].role == HU_ROLE_SYSTEM && request->messages[0].content) {
        size_t copy = request->messages[0].content_len;
        if (copy >= sizeof(r->captured_retry_sys))
            copy = sizeof(r->captured_retry_sys) - 1;
        memcpy(r->captured_retry_sys, request->messages[0].content, copy);
        r->captured_retry_sys[copy] = '\0';
        r->captured_retry_sys_len = copy;
    }

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

/* ── 2026-05-12 Jordan-iMessage style-collapse regression suite ──────────
 *
 * On 2026-05-12 the slim retry path lacked any persona/channel anchor. When
 * response_guard rejected Gemma-4's chain-of-thought first draft, the retry
 * collapsed onto a generic polite-assistant register and shipped "Yeah,
 * that works for me. See you then." to a real human contact via iMessage —
 * violating every rule in the persona's iMessage overlay (lowercase,
 * fragments, skip punctuation, react first).
 *
 * These tests pin the fix:
 *   1. hu_persona_build_retry_hint emits channel overlay style rules.
 *   2. hu_persona_build_retry_hint NULL-persona / no-overlay fallbacks.
 *   3. The slim retry's system prompt actually contains the style rules
 *      end-to-end (mock provider captures call #2's sys message). */

static void retry_hint_includes_imessage_overlay_style(void) {
    hu_allocator_t alloc = hu_system_allocator();

    char *style_notes_arr[] = {
        (char *)"lowercase by default",
        (char *)"fragments are fine",
        (char *)"React FIRST then add substance",
        (char *)"NEVER sound like an AI assistant",
    };
    hu_persona_overlay_t imessage_overlay = {0};
    imessage_overlay.channel = (char *)"imessage";
    imessage_overlay.formality = (char *)"Informal";
    imessage_overlay.avg_length = (char *)"5-15 words default";
    imessage_overlay.emoji_usage = (char *)"Rare";
    imessage_overlay.style_notes = style_notes_arr;
    imessage_overlay.style_notes_count = 4;

    hu_persona_t persona = {0};
    persona.name = (char *)"Seth";
    persona.core_anchor = (char *)"Seth Douglas Ford (51, Chief Architect)";
    persona.overlays = &imessage_overlay;
    persona.overlays_count = 1;

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_persona_build_retry_hint(&alloc, &persona, "imessage", 8, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT(out_len > 0);

    /* Identity comes through */
    HU_ASSERT(strstr(out, "Seth Douglas Ford") != NULL);
    /* Channel name is named in the STYLE clause */
    HU_ASSERT(strstr(out, "imessage") != NULL);
    /* Formality + avg_length + emoji are included */
    HU_ASSERT(strstr(out, "formality=Informal") != NULL);
    HU_ASSERT(strstr(out, "avg_length=") != NULL);
    HU_ASSERT(strstr(out, "emoji=") != NULL);
    /* Style rules — at minimum the lowercase + fragments rules */
    HU_ASSERT(strstr(out, "lowercase") != NULL);
    HU_ASSERT(strstr(out, "fragments") != NULL);
    /* Closes with the "Reply as Seth" anti-assistant directive */
    HU_ASSERT(strstr(out, "Reply as Seth") != NULL);
    /* Stays under the documented cap */
    HU_ASSERT(out_len <= HU_PERSONA_RETRY_HINT_MAX);

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void retry_hint_handles_null_persona_and_missing_overlay(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* (1) NULL persona — caller falls back to legacy bare-prompt retry. */
    char *out = NULL;
    size_t out_len = 99;
    hu_error_t err = hu_persona_build_retry_hint(&alloc, NULL, "imessage", 8, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(out == NULL);
    HU_ASSERT_EQ(out_len, 0u);

    /* (2) Persona with no matching overlay — still anchors identity + a
     * generic "match your usual voice on <channel>" so the retry doesn't
     * collapse onto polite-assistant. */
    hu_persona_t persona = {0};
    persona.name = (char *)"Seth";
    persona.core_anchor = (char *)"Seth Douglas Ford";

    out = NULL;
    out_len = 0;
    err = hu_persona_build_retry_hint(&alloc, &persona, "discord", 7, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT(strstr(out, "Seth Douglas Ford") != NULL);
    HU_ASSERT(strstr(out, "discord") != NULL);
    HU_ASSERT(strstr(out, "Reply as Seth") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);

    /* (3) NULL channel — no-overlay branch but no channel name to splice. */
    out = NULL;
    out_len = 0;
    err = hu_persona_build_retry_hint(&alloc, &persona, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT(strstr(out, "Seth Douglas Ford") != NULL);
    HU_ASSERT(strstr(out, "Reply as Seth") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void slim_retry_splices_imessage_overlay_into_sys_prompt(void) {
    /* End-to-end: rejected first draft -> slim retry path -> captured sys
     * prompt MUST contain the iMessage overlay's style rules. This is the
     * direct regression for the Jordan reply: if this test fails, the retry
     * is back to generic "Short, natural, channel-appropriate" and the model
     * will collapse to AI-formal output again. */
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

    /* Inject a minimal stack-allocated persona with an iMessage overlay.
     * Take care to clear agent.persona before deinit so we don't free a
     * stack pointer. */
    char *style_notes_arr[] = {
        (char *)"lowercase by default skip punctuation often",
        (char *)"fragments are fine yeah for sure lol nah",
        (char *)"NEVER sound like an AI assistant",
    };
    hu_persona_overlay_t imessage_overlay = {0};
    imessage_overlay.channel = (char *)"imessage";
    imessage_overlay.formality = (char *)"Informal";
    imessage_overlay.avg_length = (char *)"5-15 words";
    imessage_overlay.emoji_usage = (char *)"Rare";
    imessage_overlay.style_notes = style_notes_arr;
    imessage_overlay.style_notes_count = 3;

    hu_persona_t persona = {0};
    persona.name = (char *)"Seth";
    persona.core_anchor = (char *)"Seth Douglas Ford";
    persona.overlays = &imessage_overlay;
    persona.overlays_count = 1;

    hu_persona_t *prior_persona = agent.persona;
    agent.persona = &persona;

    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn(&agent, "haha i figured 😂", strlen("haha i figured 😂"), &response,
                        &response_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(provider_ctx.calls, 2);
    HU_ASSERT(provider_ctx.captured_retry_sys_len > 0);

    /* The retry sys prompt MUST contain the iMessage overlay style rules. */
    HU_ASSERT(strstr(provider_ctx.captured_retry_sys, "Seth Douglas Ford") != NULL);
    HU_ASSERT(strstr(provider_ctx.captured_retry_sys, "imessage") != NULL);
    HU_ASSERT(strstr(provider_ctx.captured_retry_sys, "lowercase") != NULL);
    HU_ASSERT(strstr(provider_ctx.captured_retry_sys, "fragments") != NULL);
    /* And it must NOT collapse back to the old "Short, natural,
     * channel-appropriate" prompt that caused the Jordan reply. */
    HU_ASSERT(strstr(provider_ctx.captured_retry_sys, "Short, natural, channel-appropriate") ==
              NULL);

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);

    agent.persona = prior_persona;
    hu_agent_deinit(&agent);
}

void run_response_guard_retry_tests(void) {
    HU_TEST_SUITE("Response Guard Retry");
    HU_RUN_TEST(guard_reject_retry_produces_human_like_replacement);
    HU_RUN_TEST(stream_guard_reject_retry_produces_human_like_replacement);
    HU_RUN_TEST(stream_guard_buffers_raw_output_until_retry_passes);
    HU_RUN_TEST(retry_hint_includes_imessage_overlay_style);
    HU_RUN_TEST(retry_hint_handles_null_persona_and_missing_overlay);
    HU_RUN_TEST(slim_retry_splices_imessage_overlay_into_sys_prompt);
}
