/* tests/test_llm_purpose.c — X-HU-Purpose tagging + the purpose -> priority map.
 *
 * mlx-server's admission queue reads X-HU-Priority and treats ONLY `batch` as
 * low priority (no header = live). Reply calls must therefore go out unmarked
 * and background calls must carry `batch`. Every request also names its
 * purpose in X-HU-Purpose, [a-z_]{1,24}, never message text.
 *
 * Production symbols: hu_llm_purpose_headers (the single map), the compatible
 * transport (hu_compatible_create -> chat), the HTTP embedder, hu_agent_turn
 * (tags its LLM calls `reply`), hu_fact_extract_llm (tags `extract`). */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/llm_purpose.h"
#include "human/memory/fact_extract.h"
#include "human/memory/fact_extract_llm.h"
#include "human/memory/vector/embedder_http.h"
#include "human/provider.h"
#include "human/providers/compatible.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

static void lp_reset(void) {
    (void)hu_llm_purpose_set(HU_LLM_PURPOSE_UNTAGGED);
    while (hu_llm_background_active())
        hu_llm_background_exit();
}

static void lp_headers(hu_llm_purpose_t p, char *buf, size_t cap) {
    HU_ASSERT_GT(hu_llm_purpose_headers(p, buf, cap), 0u);
}

/* ── the single map ────────────────────────────────────────────────────── */

static void reply_purposes_send_no_priority_header(void) {
    lp_reset();
    const hu_llm_purpose_t fg[] = {HU_LLM_PURPOSE_UNTAGGED,    HU_LLM_PURPOSE_REPLY,
                                   HU_LLM_PURPOSE_GUARD_RETRY, HU_LLM_PURPOSE_PLANNER,
                                   HU_LLM_PURPOSE_EXTRACT,     HU_LLM_PURPOSE_JUDGE};
    for (size_t i = 0; i < sizeof(fg) / sizeof(fg[0]); i++) {
        char h[128];
        lp_headers(fg[i], h, sizeof(h));
        char want[64];
        snprintf(want, sizeof(want), "X-HU-Purpose: %s\r\n", hu_llm_purpose_name(fg[i]));
        HU_ASSERT_STR_EQ(h, want); /* exactly the purpose line: no priority header */
    }
}

static void background_purposes_send_batch(void) {
    lp_reset();
    const hu_llm_purpose_t bg[] = {HU_LLM_PURPOSE_EMBED, HU_LLM_PURPOSE_PROACTIVE,
                                   HU_LLM_PURPOSE_BACKGROUND};
    for (size_t i = 0; i < sizeof(bg) / sizeof(bg[0]); i++) {
        char h[128];
        lp_headers(bg[i], h, sizeof(h));
        HU_ASSERT_NOT_NULL(strstr(h, "X-HU-Priority: batch\r\n"));
        HU_ASSERT_TRUE(hu_llm_purpose_is_background(bg[i]));
    }
}

static void background_lane_marks_everything_batch(void) {
    lp_reset();
    char h[128];
    lp_headers(HU_LLM_PURPOSE_REPLY, h, sizeof(h));
    HU_ASSERT_NULL(strstr(h, "X-HU-Priority")); /* pre: a reply is unmarked */
    hu_llm_background_enter();
    lp_headers(HU_LLM_PURPOSE_REPLY, h, sizeof(h));
    HU_ASSERT_STR_EQ(h, "X-HU-Purpose: background\r\nX-HU-Priority: batch\r\n");
    lp_headers(HU_LLM_PURPOSE_EXTRACT, h, sizeof(h));
    HU_ASSERT_STR_EQ(h, "X-HU-Purpose: extract\r\nX-HU-Priority: batch\r\n");
    hu_llm_background_enter(); /* nests */
    hu_llm_background_exit();
    HU_ASSERT_TRUE(hu_llm_background_active());
    hu_llm_background_exit();
    HU_ASSERT_FALSE(hu_llm_background_active());
    lp_headers(HU_LLM_PURPOSE_JUDGE, h, sizeof(h));
    HU_ASSERT_NULL(strstr(h, "X-HU-Priority")); /* post: lane closed */
}

static void purpose_names_fit_server_charset(void) {
    for (int p = 0; p < (int)HU_LLM_PURPOSE__COUNT; p++) {
        const char *n = hu_llm_purpose_name((hu_llm_purpose_t)p);
        size_t len = strlen(n);
        HU_ASSERT_TRUE(len >= 1 && len <= 24);
        for (size_t i = 0; i < len; i++)
            HU_ASSERT_TRUE((n[i] >= 'a' && n[i] <= 'z') || n[i] == '_');
    }
    HU_ASSERT_STR_EQ(hu_llm_purpose_name((hu_llm_purpose_t)999), "untagged");
}

static void set_returns_previous_and_set_if_untagged_keeps_caller_tag(void) {
    lp_reset();
    HU_ASSERT_EQ((int)hu_llm_purpose_current(), (int)HU_LLM_PURPOSE_UNTAGGED);
    hu_llm_purpose_t prev = hu_llm_purpose_set(HU_LLM_PURPOSE_PROACTIVE);
    HU_ASSERT_EQ((int)prev, (int)HU_LLM_PURPOSE_UNTAGGED);
    prev = hu_llm_purpose_set_if_untagged(HU_LLM_PURPOSE_REPLY);
    HU_ASSERT_EQ((int)prev, (int)HU_LLM_PURPOSE_PROACTIVE);
    HU_ASSERT_EQ((int)hu_llm_purpose_current(), (int)HU_LLM_PURPOSE_PROACTIVE); /* caller wins */
    (void)hu_llm_purpose_set(HU_LLM_PURPOSE_UNTAGGED);
    prev = hu_llm_purpose_set_if_untagged(HU_LLM_PURPOSE_REPLY);
    HU_ASSERT_EQ((int)hu_llm_purpose_current(), (int)HU_LLM_PURPOSE_REPLY);
    (void)hu_llm_purpose_set(prev);
    HU_ASSERT_EQ((int)hu_llm_purpose_current(), (int)HU_LLM_PURPOSE_UNTAGGED);
}

static void headers_refuse_tiny_buffer(void) {
    char h[8];
    HU_ASSERT_EQ(hu_llm_purpose_headers(HU_LLM_PURPOSE_EMBED, h, sizeof(h)), 0u);
    HU_ASSERT_EQ(h[0], '\0');
}

/* ── transports: what goes on the wire ─────────────────────────────────── */

static void compatible_chat_sends_current_purpose(void) {
    lp_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t p;
    HU_ASSERT_EQ(hu_compatible_create(&alloc, "k", 1, "http://127.0.0.1:8741/v1", 24, &p), HU_OK);
    hu_chat_message_t m = {.role = HU_ROLE_USER, .content = "hi", .content_len = 2};
    hu_chat_request_t req = {.messages = &m, .messages_count = 1};

    hu_chat_response_t r;
    hu_llm_purpose_t prev = hu_llm_purpose_set(HU_LLM_PURPOSE_REPLY);
    HU_ASSERT_EQ(p.vtable->chat(p.ctx, &alloc, &req, "m", 1, 0.5, &r), HU_OK);
    hu_chat_response_free(&alloc, &r);
    HU_ASSERT_STR_EQ(hu_compatible_test_last_headers(), "X-HU-Purpose: reply\r\n");

    (void)hu_llm_purpose_set(HU_LLM_PURPOSE_PROACTIVE);
    HU_ASSERT_EQ(p.vtable->chat(p.ctx, &alloc, &req, "m", 1, 0.5, &r), HU_OK);
    hu_chat_response_free(&alloc, &r);
    HU_ASSERT_STR_EQ(hu_compatible_test_last_headers(),
                     "X-HU-Purpose: proactive\r\nX-HU-Priority: batch\r\n");
    (void)hu_llm_purpose_set(prev);
    if (p.vtable->deinit)
        p.vtable->deinit(p.ctx, &alloc);
}

static void embedder_http_sends_embed_batch(void) {
    lp_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_embedder_t e = hu_embedder_http_create(&alloc, "http://127.0.0.1:8741");
    HU_ASSERT_NOT_NULL(e.ctx);
    hu_embedding_t emb = {0};
    /* The HU_IS_TEST HTTP mock answers with a chat body, so the parse fails;
     * the request (and its headers) still went out. */
    (void)e.vtable->embed(e.ctx, &alloc, "went hiking", 11, &emb);
    if (emb.values)
        alloc.free(alloc.ctx, emb.values, emb.dim * sizeof(float));
    HU_ASSERT_STR_EQ(hu_http_test_last_extra_headers(),
                     "X-HU-Purpose: embed\r\nX-HU-Priority: batch\r\n");
    e.vtable->deinit(e.ctx, &alloc);
}

/* ── call sites: the purpose current while the provider runs ───────────── */

typedef struct lp_seen {
    hu_llm_purpose_t purpose[8];
    char headers[8][128];
    int n;
} lp_seen_t;

static void lp_record(lp_seen_t *s) {
    if (s->n >= 8)
        return;
    s->purpose[s->n] = hu_llm_purpose_current();
    (void)hu_llm_purpose_headers(s->purpose[s->n], s->headers[s->n], sizeof(s->headers[0]));
    s->n++;
}

static hu_error_t lp_chat_with_system(void *ctx, hu_allocator_t *alloc, const char *sys,
                                      size_t sys_len, const char *msg, size_t msg_len,
                                      const char *model, size_t model_len, double temperature,
                                      char **out, size_t *out_len) {
    (void)sys;
    (void)sys_len;
    (void)msg;
    (void)msg_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    lp_record((lp_seen_t *)ctx);
    static const char body[] = "{\"facts\":[]}";
    *out = (char *)alloc->alloc(alloc->ctx, sizeof(body));
    memcpy(*out, body, sizeof(body));
    *out_len = sizeof(body) - 1;
    return HU_OK;
}

static hu_error_t lp_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                          const char *model, size_t model_len, double temperature,
                          hu_chat_response_t *out) {
    (void)req;
    (void)model;
    (void)model_len;
    (void)temperature;
    lp_record((lp_seen_t *)ctx);
    memset(out, 0, sizeof(*out));
    static const char body[] = "ok sure";
    char *buf = (char *)alloc->alloc(alloc->ctx, sizeof(body));
    memcpy(buf, body, sizeof(body));
    out->content = buf;
    out->content_len = sizeof(body) - 1;
    return HU_OK;
}

static const char *lp_name(void *ctx) {
    (void)ctx;
    return "lp_mock";
}

static const hu_provider_vtable_t lp_vtable = {
    .chat = lp_chat,
    .chat_with_system = lp_chat_with_system,
    .get_name = lp_name,
};

static void fact_extract_llm_tags_extract_and_restores(void) {
    lp_reset();
    hu_allocator_t alloc = hu_system_allocator();
    lp_seen_t seen = {0};
    hu_provider_t prov = {.ctx = &seen, .vtable = &lp_vtable};
    hu_fact_extract_result_t res;
    hu_llm_purpose_t outer = hu_llm_purpose_set(HU_LLM_PURPOSE_REPLY);
    (void)hu_fact_extract_llm(&alloc, &prov, "m", 1, "did you get the email", 21, 1700000000LL,
                              &res, NULL);
    HU_ASSERT_EQ(seen.n, 1);
    HU_ASSERT_EQ((int)seen.purpose[0], (int)HU_LLM_PURPOSE_EXTRACT);
    HU_ASSERT_EQ((int)hu_llm_purpose_current(), (int)HU_LLM_PURPOSE_REPLY); /* restored */
    (void)hu_llm_purpose_set(outer);
}

#ifdef HU_ENABLE_SQLITE
#include "human/agent.h"
#include "human/agent/world_model_bridge.h"
#include "human/memory/graph.h"

typedef struct lp_agent_fx {
    hu_allocator_t alloc;
    hu_provider_t prov;
    hu_graph_t *g;
    hu_w7_facade_t *wf;
    hu_agent_t agent;
    lp_seen_t seen;
} lp_agent_fx_t;

static void lp_agent_open(lp_agent_fx_t *f) {
    memset(f, 0, sizeof(*f));
    f->alloc = hu_system_allocator();
    f->prov.ctx = &f->seen;
    f->prov.vtable = &lp_vtable;
    HU_ASSERT_EQ(hu_graph_open(&f->alloc, NULL, 0, &f->g), HU_OK);
    HU_ASSERT_EQ(hu_w7_facade_open(f->g, &f->alloc, &f->wf), HU_OK);
    HU_ASSERT_EQ(hu_agent_from_config(&f->agent, &f->alloc, f->prov, NULL, 0, NULL, NULL, NULL,
                                      NULL, "lp-model", 8, "lp_mock", 7, 0.7, ".", 1, 5, 50, false,
                                      0, NULL, 0, NULL, 0, NULL),
                 HU_OK);
    f->agent.verifier_graph = f->g;
    f->agent.w7_facade = f->wf;
    f->agent.memory_session_id = "lp-session";
    f->agent.memory_session_id_len = 10;
    f->agent.active_channel = "imessage";
    f->agent.active_channel_len = 8;
}

static void lp_agent_close(lp_agent_fx_t *f) {
    hu_agent_deinit(&f->agent);
    hu_graph_close(f->g, &f->alloc);
}

static void lp_agent_run(lp_agent_fx_t *f) {
    char *resp = NULL;
    size_t resp_len = 0;
    HU_ASSERT_EQ(hu_agent_turn(&f->agent, "hey", 3, &resp, &resp_len), HU_OK);
    if (resp)
        f->alloc.free(f->alloc.ctx, resp, resp_len + 1);
}

/* THE reply contract: the turn's main call goes out as `reply`, unmarked. */
static void agent_turn_main_call_is_reply_without_priority(void) {
    lp_reset();
    lp_agent_fx_t f;
    lp_agent_open(&f);
    HU_ASSERT_EQ((int)hu_llm_purpose_current(), (int)HU_LLM_PURPOSE_UNTAGGED); /* pre */
    lp_agent_run(&f);
    HU_ASSERT_GT(f.seen.n, 0);
    HU_ASSERT_EQ((int)f.seen.purpose[0], (int)HU_LLM_PURPOSE_REPLY);
    HU_ASSERT_STR_EQ(f.seen.headers[0], "X-HU-Purpose: reply\r\n");
    HU_ASSERT_EQ((int)hu_llm_purpose_current(), (int)HU_LLM_PURPOSE_UNTAGGED); /* restored */
    lp_agent_close(&f);
}

/* A turn run from background work (housekeeping lane) goes out as batch. */
static void agent_turn_inside_background_lane_is_batch(void) {
    lp_reset();
    lp_agent_fx_t f;
    lp_agent_open(&f);
    hu_llm_background_enter();
    lp_agent_run(&f);
    hu_llm_background_exit();
    HU_ASSERT_GT(f.seen.n, 0);
    HU_ASSERT_NOT_NULL(strstr(f.seen.headers[0], "X-HU-Priority: batch\r\n"));
    lp_agent_close(&f);
}

/* A caller's tag wins over the turn's `reply` default. */
static void agent_turn_keeps_caller_purpose(void) {
    lp_reset();
    lp_agent_fx_t f;
    lp_agent_open(&f);
    hu_llm_purpose_t prev = hu_llm_purpose_set(HU_LLM_PURPOSE_PROACTIVE);
    lp_agent_run(&f);
    (void)hu_llm_purpose_set(prev);
    HU_ASSERT_GT(f.seen.n, 0);
    HU_ASSERT_EQ((int)f.seen.purpose[0], (int)HU_LLM_PURPOSE_PROACTIVE);
    HU_ASSERT_NOT_NULL(strstr(f.seen.headers[0], "X-HU-Priority: batch\r\n"));
    lp_agent_close(&f);
}
#endif

void run_llm_purpose_tests(void) {
    HU_TEST_SUITE("llm_purpose");
    HU_RUN_TEST(reply_purposes_send_no_priority_header);
    HU_RUN_TEST(background_purposes_send_batch);
    HU_RUN_TEST(background_lane_marks_everything_batch);
    HU_RUN_TEST(purpose_names_fit_server_charset);
    HU_RUN_TEST(set_returns_previous_and_set_if_untagged_keeps_caller_tag);
    HU_RUN_TEST(headers_refuse_tiny_buffer);
    HU_RUN_TEST(compatible_chat_sends_current_purpose);
    HU_RUN_TEST(embedder_http_sends_embed_batch);
    HU_RUN_TEST(fact_extract_llm_tags_extract_and_restores);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(agent_turn_main_call_is_reply_without_priority);
    HU_RUN_TEST(agent_turn_inside_background_lane_is_batch);
    HU_RUN_TEST(agent_turn_keeps_caller_purpose);
#endif
}
