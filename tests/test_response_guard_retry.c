#include "human/agent.h"
#include "human/agent/response_guard_retry.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/persona.h"
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

/* ── Sprint 34 — end-to-end G5 (length anomaly) + G6 (director echo) ──
 *
 * These exercise the production wiring that lives in agent_stream.c
 * and agent_turn.c (Sprint 33 added recent_avg_len, Sprint 34 added
 * scene_direction_text). Each test runs a real agent turn through a
 * mock provider and asserts the agent surfaces a clean reply, not the
 * leaky one — proof the guard fires at runtime, not just in unit tests. */

typedef struct {
    int calls;
    /* Per-call text. NULL = use defaults below. */
    const char *call_text[8];
    size_t call_text_len[8];
} length_provider_ctx_t;

static const char *length_provider_name(void *ctx) {
    (void)ctx;
    return "length_anomaly_mock";
}

static bool length_provider_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}

static bool length_provider_supports_streaming(void *ctx) {
    (void)ctx;
    return false;
}

static void length_provider_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static hu_error_t length_provider_chat(void *ctx, hu_allocator_t *alloc,
                                       const hu_chat_request_t *request, const char *model,
                                       size_t model_len, double temperature,
                                       hu_chat_response_t *out) {
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    length_provider_ctx_t *r = (length_provider_ctx_t *)ctx;
    int idx = r->calls;
    if (idx < 0 || idx >= 8 || !r->call_text[idx]) {
        out->content = hu_strndup(alloc, "ok", 2);
        out->content_len = out->content ? 2 : 0;
    } else {
        out->content = hu_strndup(alloc, r->call_text[idx], r->call_text_len[idx]);
        out->content_len = out->content ? r->call_text_len[idx] : 0;
    }
    r->calls++;
    out->tool_calls = NULL;
    out->tool_calls_count = 0;
    out->reasoning_content = NULL;
    out->reasoning_content_len = 0;
    return out->content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static const hu_provider_vtable_t length_provider_vtable = {
    .chat = length_provider_chat,
    .supports_native_tools = length_provider_supports_native_tools,
    .get_name = length_provider_name,
    .deinit = length_provider_deinit,
    .supports_streaming = length_provider_supports_streaming,
    .stream_chat = NULL,
};

static hu_provider_t length_provider_create(length_provider_ctx_t *ctx) {
    return (hu_provider_t){.ctx = ctx, .vtable = &length_provider_vtable};
}

/* G5 — length anomaly through agent_turn.
 *
 * Seeds the agent's history with two short replies (~25 bytes each),
 * then a third response that is 1500 bytes of *varied* prose (trips
 * neither degenerate repetition nor any semantic marker). The wired
 * Sprint 33 helper computes recent_avg_len ≈ 25; G5 fires at 8×; the
 * 1500-byte response is REJECTed and the retry returns a normal reply. */
static void agent_g5_length_anomaly_rejects_and_retries(void) {
    hu_allocator_t alloc = hu_system_allocator();
    length_provider_ctx_t pctx;
    memset(&pctx, 0, sizeof(pctx));

    /* Build a benign 1500-byte reply with varied content (mirrors the
     * fixture pattern in test_response_guard.c: long enough to trip G5
     * but varied enough not to trip Phase 2 / Phase 3). */
    static char long_reply[1600];
    static const char phrase[] =
        "Sure thing, that all sounds reasonable to me right now. "
        "Maybe we can grab coffee tomorrow if you have free time then. ";
    size_t plen = sizeof(phrase) - 1;
    size_t target = 1500;
    size_t i = 0;
    while (i + plen < target) {
        memcpy(long_reply + i, phrase, plen);
        i += plen;
    }
    long_reply[i] = '\0';
    size_t long_len = i;

    static const char short1[] = "yeah, sounds good lol";       /* 21 */
    static const char short2[] = "hahaha ok, fair enough";      /* 22 */
    static const char retry_ok[] = "ok cool, sounds good";      /* 20 */
    pctx.call_text[0] = short1;
    pctx.call_text_len[0] = sizeof(short1) - 1;
    pctx.call_text[1] = short2;
    pctx.call_text_len[1] = sizeof(short2) - 1;
    pctx.call_text[2] = long_reply;
    pctx.call_text_len[2] = long_len;
    /* Call 3 is the slim retry triggered by the REJECT. */
    pctx.call_text[3] = retry_ok;
    pctx.call_text_len[3] = sizeof(retry_ok) - 1;

    hu_provider_t provider = length_provider_create(&pctx);
    hu_agent_t agent;
    hu_error_t err = hu_agent_from_config(&agent, &alloc, provider, NULL, 0, NULL, NULL, NULL, NULL,
                                          "test-model", 10, "length_anomaly_mock", 19, 0.7,
                                          "/tmp", 4, 5, 50, false, 3, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    agent.active_channel = "imessage";
    agent.active_channel_len = 8;

    /* Turn 1: short reply, history empty → recent_avg = 0, no G5. */
    char *r1 = NULL;
    size_t r1_len = 0;
    HU_ASSERT_EQ(hu_agent_turn(&agent, "hey", 3, &r1, &r1_len), HU_OK);
    HU_ASSERT_EQ(r1_len, sizeof(short1) - 1);
    alloc.free(alloc.ctx, r1, r1_len + 1);

    /* Turn 2: short reply, history has 1 entry → recent_avg = 21,
     * 22 bytes is well within tolerance, no G5. */
    char *r2 = NULL;
    size_t r2_len = 0;
    HU_ASSERT_EQ(hu_agent_turn(&agent, "hi", 2, &r2, &r2_len), HU_OK);
    HU_ASSERT_EQ(r2_len, sizeof(short2) - 1);
    alloc.free(alloc.ctx, r2, r2_len + 1);

    /* Turn 3: provider returns 1500 bytes. recent_avg ≈ 21 → ratio
     * ~71×, trips G5 at the 8× threshold. Guard REJECTs, slim retry
     * fires (call #4 in the provider), agent surfaces the retry text. */
    char *r3 = NULL;
    size_t r3_len = 0;
    HU_ASSERT_EQ(hu_agent_turn(&agent, "what's up", 9, &r3, &r3_len), HU_OK);
    /* Provider was called at least 4 times — original + slim retry. */
    HU_ASSERT(pctx.calls >= 4);
    /* Final response must NOT be the 1500-byte dump. */
    HU_ASSERT(r3_len < 200);
    /* Retry path returned the clean text. */
    HU_ASSERT_NOT_NULL(r3);
    alloc.free(alloc.ctx, r3, r3_len + 1);

    hu_agent_deinit(&agent);
}

/* G6 — director-string echo through agent_turn.
 *
 * Sets the agent's scene_direction_text to the same string the mock
 * provider verbatim-quotes back. The wiring (Sprint 34) propagates
 * this into hu_guard_context_t.director_text; the response_guard
 * matches the >=30-byte verbatim run and REJECTs. The slim retry
 * returns a clean reply.
 *
 * Pattern reproduces the 2026-05-12 leak signature where the model
 * echoed "per scene direction" + the rest of the scene-direction
 * paragraph back to a real human contact. */
static void agent_g6_director_echo_rejects_and_retries(void) {
    hu_allocator_t alloc = hu_system_allocator();
    length_provider_ctx_t pctx;
    memset(&pctx, 0, sizeof(pctx));

    static const char director[] =
        "Slightly skeptical but intrigued, acknowledge the observation about the AI";
    /* Mock first reply: model leaks the director text verbatim back
     * (>= 30 bytes). Guard's G6 must REJECT this. */
    static char leaked[256];
    int n = snprintf(leaked, sizeof(leaked),
                     "got it - %s. yeah lol", director);
    HU_ASSERT(n > 0 && (size_t)n < sizeof(leaked));
    static const char retry_ok[] = "got it lol";
    pctx.call_text[0] = leaked;
    pctx.call_text_len[0] = (size_t)n;
    /* Slim retry returns a clean reply with no director quote. */
    pctx.call_text[1] = retry_ok;
    pctx.call_text_len[1] = sizeof(retry_ok) - 1;

    hu_provider_t provider = length_provider_create(&pctx);
    hu_agent_t agent;
    hu_error_t err = hu_agent_from_config(&agent, &alloc, provider, NULL, 0, NULL, NULL, NULL, NULL,
                                          "test-model", 10, "length_anomaly_mock", 19, 0.7,
                                          "/tmp", 4, 5, 50, false, 1, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    agent.active_channel = "imessage";
    agent.active_channel_len = 8;
    /* Mirror the daemon-side wire: scene_direction_text is set before
     * the turn, cleared after. Here we just set it; deinit will not
     * free the static string. */
    agent.scene_direction_text = director;
    agent.scene_direction_text_len = sizeof(director) - 1;

    char *r = NULL;
    size_t rlen = 0;
    HU_ASSERT_EQ(hu_agent_turn(&agent, "interesting", 11, &r, &rlen), HU_OK);

    /* The leaked content must NOT be in the surfaced reply. */
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT(strstr(r, director) == NULL);
    /* Provider was called at least twice — original + retry. */
    HU_ASSERT(pctx.calls >= 2);

    /* Mirror the daemon clear-on-exit. */
    agent.scene_direction_text = NULL;
    agent.scene_direction_text_len = 0;

    alloc.free(alloc.ctx, r, rlen + 1);
    hu_agent_deinit(&agent);
}

/* G7 — persona-PII echo through agent_turn (Sprint 35).
 *
 * Loads the agent with a tiny synthetic persona (name="testname", all
 * other fields zero/empty), then drives a turn where the mock provider
 * leaks "testname is a software developer". The wired G7 must reject;
 * the slim retry must surface a clean reply. */
static void agent_g7_persona_pii_echo_rejects_and_retries(void) {
    hu_allocator_t alloc = hu_system_allocator();
    length_provider_ctx_t pctx;
    memset(&pctx, 0, sizeof(pctx));

    static const char leaked[] =
        "yeah testname is a software developer who loves dry humor";
    static const char retry_ok[] = "yeah lol";
    pctx.call_text[0] = leaked;
    pctx.call_text_len[0] = sizeof(leaked) - 1;
    pctx.call_text[1] = retry_ok;
    pctx.call_text_len[1] = sizeof(retry_ok) - 1;

    hu_provider_t provider = length_provider_create(&pctx);
    hu_agent_t agent;
    hu_error_t err = hu_agent_from_config(&agent, &alloc, provider, NULL, 0, NULL, NULL, NULL, NULL,
                                          "test-model", 10, "length_anomaly_mock", 19, 0.7,
                                          "/tmp", 4, 5, 50, false, 1, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    agent.active_channel = "imessage";
    agent.active_channel_len = 8;

    /* Build a heap-allocated minimal persona shim. hu_persona_deinit
     * frees `persona->name` with `name_len + 1` and skips other fields
     * if they are NULL. So we can safely set just `name` + `name_len`
     * and zero everything else — agent_deinit will clean up properly. */
    hu_persona_t *p = (hu_persona_t *)alloc.alloc(alloc.ctx, sizeof(*p));
    HU_ASSERT_NOT_NULL(p);
    memset(p, 0, sizeof(*p));
    p->name = hu_strndup(&alloc, "testname", 8);
    p->name_len = 8;
    HU_ASSERT_NOT_NULL(p->name);
    /* Replace any persona the agent allocated. */
    if (agent.persona) {
        hu_persona_deinit(&alloc, agent.persona);
        alloc.free(alloc.ctx, agent.persona, sizeof(hu_persona_t));
    }
    agent.persona = p;

    char *r = NULL;
    size_t rlen = 0;
    HU_ASSERT_EQ(hu_agent_turn(&agent, "tell me about you", 17, &r, &rlen), HU_OK);

    /* The leaked "testname is a software developer" must NOT have made
     * it to the surfaced response. */
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT(strstr(r, "is a software developer") == NULL);
    /* Provider was called at least twice — original + retry. */
    HU_ASSERT(pctx.calls >= 2);

    alloc.free(alloc.ctx, r, rlen + 1);
    hu_agent_deinit(&agent);
}

/* G8 — persona identity / core-anchor echo through agent_turn (Sprint 36).
 *
 * Loads the agent with a synthetic persona shim (name + identity).
 * Mock provider leaks a verbatim 30+ byte chunk of the identity string
 * (no name in the response — pure first-person identity echo). G8
 * fires; slim retry returns clean reply. Confirms identity wiring at
 * the production call site. */
static void agent_g8_persona_identity_echo_rejects_and_retries(void) {
    hu_allocator_t alloc = hu_system_allocator();
    length_provider_ctx_t pctx;
    memset(&pctx, 0, sizeof(pctx));

    /* identity string is 40 bytes; leaked response quotes 40 bytes
     * verbatim (first-person, no name — would slip past G7). */
    static const char leaked[] =
        "yeah i'm a Chief Architect at Pure Health Solutions, busy day";
    static const char retry_ok[] = "yeah lol";
    pctx.call_text[0] = leaked;
    pctx.call_text_len[0] = sizeof(leaked) - 1;
    pctx.call_text[1] = retry_ok;
    pctx.call_text_len[1] = sizeof(retry_ok) - 1;

    hu_provider_t provider = length_provider_create(&pctx);
    hu_agent_t agent;
    hu_error_t err = hu_agent_from_config(&agent, &alloc, provider, NULL, 0, NULL, NULL, NULL, NULL,
                                          "test-model", 10, "identity_echo_mock", 18, 0.7,
                                          "/tmp", 4, 5, 50, false, 1, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    agent.active_channel = "imessage";
    agent.active_channel_len = 8;

    /* Persona shim with name + identity. hu_persona_deinit will free
     * both fields since they're heap-allocated via hu_strndup. */
    hu_persona_t *p = (hu_persona_t *)alloc.alloc(alloc.ctx, sizeof(*p));
    HU_ASSERT_NOT_NULL(p);
    memset(p, 0, sizeof(*p));
    p->name = hu_strndup(&alloc, "testname", 8);
    p->name_len = 8;
    HU_ASSERT_NOT_NULL(p->name);
    static const char identity_str[] = "Chief Architect at Pure Health Solutions";
    p->identity = hu_strndup(&alloc, identity_str, sizeof(identity_str) - 1);
    HU_ASSERT_NOT_NULL(p->identity);
    if (agent.persona) {
        hu_persona_deinit(&alloc, agent.persona);
        alloc.free(alloc.ctx, agent.persona, sizeof(hu_persona_t));
    }
    agent.persona = p;

    char *r = NULL;
    size_t rlen = 0;
    HU_ASSERT_EQ(hu_agent_turn(&agent, "what do you do for work", 23, &r, &rlen), HU_OK);

    /* The leaked identity quote must NOT have made it to the
     * surfaced response. */
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT(strstr(r, "Chief Architect at Pure Health") == NULL);
    HU_ASSERT(pctx.calls >= 2);

    alloc.free(alloc.ctx, r, rlen + 1);
    hu_agent_deinit(&agent);
}

/* Forward declaration — director history helpers (Sprint 37). The
 * test binary links against agent.c so the symbols resolve. */
void hu_agent_internal_push_director_history(hu_agent_t *agent, const char *text,
                                              size_t text_len);

/* G6 — cross-turn director-history echo through agent_turn (Sprint 37).
 *
 * Drives a real hu_agent_turn through a mock provider. The agent's
 * scene_direction_text is NULL (no current director) but the ring
 * buffer holds yesterday's director from a previous turn. The mock
 * leaks a 30+ byte verbatim quote of the historical director — G6
 * must fire via the history path, not the current-turn path. */
static void agent_g6_history_cross_turn_rejects_and_retries(void) {
    hu_allocator_t alloc = hu_system_allocator();
    length_provider_ctx_t pctx;
    memset(&pctx, 0, sizeof(pctx));

    static const char yesterday_director[] =
        "casual short, dry; respond briefly with a skeptical follow-up";
    static const char leaked[] =
        "yeah respond briefly with a skeptical follow-up question, sure";
    static const char retry_ok[] = "yeah lol";
    pctx.call_text[0] = leaked;
    pctx.call_text_len[0] = sizeof(leaked) - 1;
    pctx.call_text[1] = retry_ok;
    pctx.call_text_len[1] = sizeof(retry_ok) - 1;

    hu_provider_t provider = length_provider_create(&pctx);
    hu_agent_t agent;
    hu_error_t err = hu_agent_from_config(&agent, &alloc, provider, NULL, 0, NULL, NULL, NULL, NULL,
                                          "test-model", 10, "history_echo_mock", 17, 0.7,
                                          "/tmp", 4, 5, 50, false, 1, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    agent.active_channel = "imessage";
    agent.active_channel_len = 8;

    /* Simulate the daemon at end-of-previous-turn: push yesterday's
     * director into history, leave scene_direction_text NULL. */
    hu_agent_internal_push_director_history(&agent, yesterday_director,
                                             sizeof(yesterday_director) - 1);
    HU_ASSERT_EQ(agent.director_history_count, (size_t)1);
    HU_ASSERT(agent.scene_direction_text == NULL);

    char *r = NULL;
    size_t rlen = 0;
    HU_ASSERT_EQ(hu_agent_turn(&agent, "tell me what to do today", 24, &r, &rlen), HU_OK);

    /* The leaked verbatim director quote must NOT have made it
     * through to the user. */
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT(strstr(r, "skeptical follow-up") == NULL);
    HU_ASSERT(pctx.calls >= 2);

    alloc.free(alloc.ctx, r, rlen + 1);
    hu_agent_deinit(&agent);
}

/* Daemon clear-on-exit — proves no stale-memory read on next turn
 * (Sprint 37, also closes Sprint 34 carry-over).
 *
 * Simulates the daemon lifecycle:
 *   Turn 1: scene_direction_text points at a stack-local buffer.
 *           After the turn, daemon NULLs scene_direction_text and
 *           pushes the buffer into the heap-owned ring buffer.
 *   Turn 2: scene_direction_text is NULL. The ring buffer holds a
 *           heap-owned copy. ASan must see no use-after-free if the
 *           original stack buffer is later overwritten.
 *
 * We exercise the second-turn path explicitly: zero out the stack
 * buffer between turns, then run turn 2 with a benign mock. The
 * guard must run cleanly (no crash, no false positive from stale
 * pointer reads). */
static void agent_clear_on_exit_no_stale_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    length_provider_ctx_t pctx;
    memset(&pctx, 0, sizeof(pctx));

    /* Turn 2 mock — benign, very short reply. G6 should not fire
     * because (a) scene_direction_text is NULL, (b) the ring buffer
     * holds yesterday's director (heap-owned, alive), but the response
     * doesn't quote it. */
    static const char benign[] = "yeah, sounds good";
    pctx.call_text[0] = benign;
    pctx.call_text_len[0] = sizeof(benign) - 1;

    hu_provider_t provider = length_provider_create(&pctx);
    hu_agent_t agent;
    hu_error_t err = hu_agent_from_config(&agent, &alloc, provider, NULL, 0, NULL, NULL, NULL, NULL,
                                          "test-model", 10, "clear_on_exit_mock", 18, 0.7,
                                          "/tmp", 4, 5, 50, false, 1, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    agent.active_channel = "imessage";
    agent.active_channel_len = 8;

    /* Turn 1 simulation: stack buffer holding the director, then push
     * to history and clear scene_direction_text — exact daemon flow. */
    {
        char turn1_director[256];
        memset(turn1_director, 0, sizeof(turn1_director));
        const char *t1 =
            "warm and curious; ask one clarifying question; minimum two sentences";
        size_t t1_len = strlen(t1);
        memcpy(turn1_director, t1, t1_len);
        agent.scene_direction_text = turn1_director;
        agent.scene_direction_text_len = t1_len;
        /* End-of-turn: push then clear. */
        hu_agent_internal_push_director_history(&agent, agent.scene_direction_text,
                                                 agent.scene_direction_text_len);
        agent.scene_direction_text = NULL;
        agent.scene_direction_text_len = 0;
    }
    /* Stack frame goes out of scope. The original buffer is now
     * undefined. The agent must NOT have a dangling pointer. */
    HU_ASSERT(agent.scene_direction_text == NULL);
    HU_ASSERT_EQ(agent.scene_direction_text_len, (size_t)0);
    HU_ASSERT_EQ(agent.director_history_count, (size_t)1);
    HU_ASSERT_NOT_NULL(agent.director_history[0]);

    /* Turn 2 — guard must run cleanly. Under ASan, any read of stale
     * stack memory through scene_direction_text would crash here. */
    char *r = NULL;
    size_t rlen = 0;
    HU_ASSERT_EQ(hu_agent_turn(&agent, "ok thanks", 9, &r, &rlen), HU_OK);
    HU_ASSERT_NOT_NULL(r);
    /* Provider was called exactly once — no retry, guard accepted. */
    HU_ASSERT_EQ(pctx.calls, (size_t)1);

    alloc.free(alloc.ctx, r, rlen + 1);
    hu_agent_deinit(&agent);
}

/* Anti-CoT slim-prompt inspection — captures system message on first chat(). */
typedef struct {
    char system_msg[2048];
    size_t system_msg_len;
} slim_inspect_ctx_t;

static const char *slim_inspect_provider_name(void *ctx) {
    (void)ctx;
    return "slim_inspect_mock";
}

static bool slim_inspect_provider_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}

static bool slim_inspect_provider_supports_streaming(void *ctx) {
    (void)ctx;
    return false;
}

static void slim_inspect_provider_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static hu_error_t slim_inspect_provider_chat(void *ctx, hu_allocator_t *alloc,
                                             const hu_chat_request_t *request, const char *model,
                                             size_t model_len, double temperature,
                                             hu_chat_response_t *out) {
    (void)model;
    (void)model_len;
    (void)temperature;
    slim_inspect_ctx_t *ic = (slim_inspect_ctx_t *)ctx;

    if (request && request->messages_count >= 1 && request->messages[0].role == HU_ROLE_SYSTEM &&
        request->messages[0].content) {
        size_t n = request->messages[0].content_len;
        if (n >= sizeof(ic->system_msg))
            n = sizeof(ic->system_msg) - 1;
        memcpy(ic->system_msg, request->messages[0].content, n);
        ic->system_msg[n] = '\0';
        ic->system_msg_len = n;
    }

    const char *text = "ok";
    out->content = hu_strndup(alloc, text, 2);
    out->content_len = out->content ? 2 : 0;
    out->tool_calls = NULL;
    out->tool_calls_count = 0;
    out->reasoning_content = NULL;
    out->reasoning_content_len = 0;
    return out->content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static const hu_provider_vtable_t slim_inspect_vtable = {
    .chat = slim_inspect_provider_chat,
    .supports_native_tools = slim_inspect_provider_supports_native_tools,
    .get_name = slim_inspect_provider_name,
    .deinit = slim_inspect_provider_deinit,
    .supports_streaming = slim_inspect_provider_supports_streaming,
    .stream_chat = NULL,
};

static void slim_prompt_contains_anti_cot_instructions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    slim_inspect_ctx_t ic;
    memset(&ic, 0, sizeof(ic));
    hu_provider_t prov = {.ctx = &ic, .vtable = &slim_inspect_vtable};

    char *out = NULL;
    size_t out_len = 0;
    const char *user_msg = "hey";
    hu_error_t err = hu_response_guard_retry_slim(&alloc, NULL, NULL, &prov, "test-model", 10,
                                                  user_msg, strlen(user_msg), &out, &out_len, NULL);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(ic.system_msg_len > 0);
    HU_ASSERT(strstr(ic.system_msg, "Do not think out loud or narrate your reasoning") != NULL);
    HU_ASSERT(strstr(ic.system_msg, "Do not refer to yourself in third-person by name") != NULL);
    HU_ASSERT(strstr(ic.system_msg, "Do not end with AI-helper closers") != NULL);

    if (out)
        alloc.free(alloc.ctx, out, out_len + 1);
}

void run_response_guard_retry_tests(void) {
    HU_TEST_SUITE("Response Guard Retry");
    HU_RUN_TEST(guard_reject_retry_produces_human_like_replacement);
    HU_RUN_TEST(stream_guard_reject_retry_produces_human_like_replacement);
    HU_RUN_TEST(stream_guard_buffers_raw_output_until_retry_passes);
    HU_RUN_TEST(slim_prompt_contains_anti_cot_instructions);

    /* Sprint 34 — end-to-end wired G5 + G6 through hu_agent_turn. */
    HU_RUN_TEST(agent_g5_length_anomaly_rejects_and_retries);
    HU_RUN_TEST(agent_g6_director_echo_rejects_and_retries);

    /* Sprint 35 — end-to-end persona-PII echo (G7). */
    HU_RUN_TEST(agent_g7_persona_pii_echo_rejects_and_retries);

    /* Sprint 36 — end-to-end persona identity echo (G8). */
    HU_RUN_TEST(agent_g8_persona_identity_echo_rejects_and_retries);

    /* Sprint 37 — cross-turn director history (G6 extension) +
     * daemon clear-on-exit lifetime safety. */
    HU_RUN_TEST(agent_g6_history_cross_turn_rejects_and_retries);
    HU_RUN_TEST(agent_clear_on_exit_no_stale_memory);
}
