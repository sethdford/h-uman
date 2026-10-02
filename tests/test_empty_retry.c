/* tests/test_empty_retry.c — one retry for a draft the serving layer discarded.
 *
 * Shape under test is the 2026-10-01 22:35 incident: the local server generated
 * 9 tokens, its echo-guard discarded them, and the daemon got HU_OK with an empty
 * body, so the contact got nothing. */
#include "human/agent.h"
#include "human/agent/empty_retry.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

static hu_chat_response_t stripped_reply(void) {
    hu_chat_response_t r;
    memset(&r, 0, sizeof(r));
    r.usage.completion_tokens = 9;
    return r;
}

/* ── predicate ─────────────────────────────────────────────────────────────── */

static void empty_retry_applies_to_stripped_draft_when_gated_on(void) {
    hu_chat_response_t r = stripped_reply();
    HU_ASSERT_TRUE(hu_empty_retry_applies(HU_GATE_SHADOW, &r, false));
    HU_ASSERT_TRUE(hu_empty_retry_applies(HU_GATE_LIVE, &r, false));
}

static void empty_retry_off_gate_never_applies(void) {
    hu_chat_response_t r = stripped_reply();
    HU_ASSERT_FALSE(hu_empty_retry_applies(HU_GATE_OFF, &r, false));
}

static void empty_retry_never_applies_twice(void) {
    hu_chat_response_t r = stripped_reply();
    HU_ASSERT_FALSE(hu_empty_retry_applies(HU_GATE_LIVE, &r, true));
}

static void empty_retry_ignores_reply_with_text(void) {
    hu_chat_response_t r = stripped_reply();
    r.content = "hey";
    r.content_len = 3;
    HU_ASSERT_FALSE(hu_empty_retry_applies(HU_GATE_LIVE, &r, false));
}

static void empty_retry_ignores_tool_call_step(void) {
    hu_chat_response_t r = stripped_reply();
    r.tool_calls_count = 1;
    HU_ASSERT_FALSE(hu_empty_retry_applies(HU_GATE_LIVE, &r, false));
}

static void empty_retry_ignores_backend_that_generated_nothing(void) {
    hu_chat_response_t r = stripped_reply();
    r.usage.completion_tokens = 0;
    HU_ASSERT_FALSE(hu_empty_retry_applies(HU_GATE_LIVE, &r, false));
    HU_ASSERT_FALSE(hu_empty_retry_applies(HU_GATE_LIVE, NULL, false));
}

static void empty_retry_mode_reads_env_default_off(void) {
    const char *prev = getenv("HU_EMPTY_REPLY_RETRY");
    char *saved = prev ? strdup(prev) : NULL;
    unsetenv("HU_EMPTY_REPLY_RETRY");
    HU_ASSERT_EQ((int)hu_empty_retry_mode(), (int)HU_GATE_OFF);
    setenv("HU_EMPTY_REPLY_RETRY", "shadow", 1);
    HU_ASSERT_EQ((int)hu_empty_retry_mode(), (int)HU_GATE_SHADOW);
    setenv("HU_EMPTY_REPLY_RETRY", "live", 1);
    HU_ASSERT_EQ((int)hu_empty_retry_mode(), (int)HU_GATE_LIVE);
    if (saved) {
        setenv("HU_EMPTY_REPLY_RETRY", saved, 1);
        free(saved);
    } else {
        unsetenv("HU_EMPTY_REPLY_RETRY");
    }
}

/* ── request helper ────────────────────────────────────────────────────────── */

typedef struct er_capture {
    size_t calls;
    size_t last_count;
    hu_role_t last_role;
    char last_content[256];
} er_capture_t;

static hu_error_t er_capture_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                                  const char *model, size_t model_len, double temperature,
                                  hu_chat_response_t *out) {
    (void)alloc;
    (void)model;
    (void)model_len;
    (void)temperature;
    er_capture_t *c = (er_capture_t *)ctx;
    c->calls++;
    c->last_count = req->messages_count;
    const hu_chat_message_t *m = &req->messages[req->messages_count - 1];
    c->last_role = m->role;
    size_t n =
        m->content_len < sizeof(c->last_content) - 1 ? m->content_len : sizeof(c->last_content) - 1;
    memcpy(c->last_content, m->content, n);
    c->last_content[n] = '\0';
    memset(out, 0, sizeof(*out));
    return HU_OK;
}

static hu_provider_vtable_t er_capture_vtable = {.chat = er_capture_chat};

static void empty_retry_chat_appends_nudge_and_leaves_request_alone(void) {
    hu_allocator_t alloc = hu_system_allocator();
    er_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    hu_provider_t prov = {.ctx = &cap, .vtable = &er_capture_vtable};
    hu_chat_message_t msgs[2] = {
        {.role = HU_ROLE_SYSTEM, .content = "persona", .content_len = 7},
        {.role = HU_ROLE_USER, .content = "you around?", .content_len = 11},
    };
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.messages = msgs;
    req.messages_count = 2;

    hu_chat_response_t out;
    HU_ASSERT_EQ(hu_empty_retry_chat(&prov, &alloc, &req, "m", 1, 0.7, &out), HU_OK);
    HU_ASSERT_EQ(cap.calls, 1u);
    HU_ASSERT_EQ(cap.last_count, 3u);
    HU_ASSERT_EQ((int)cap.last_role, (int)HU_ROLE_SYSTEM);
    HU_ASSERT_STR_EQ(cap.last_content, HU_EMPTY_RETRY_NUDGE);
    /* The caller's request is untouched: the turn may still use it. */
    HU_ASSERT_EQ(req.messages_count, 2u);
    HU_ASSERT_TRUE(req.messages == msgs);
    hu_chat_response_free(&alloc, &out);
}

static void empty_retry_chat_rejects_bad_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    hu_chat_response_t out;
    hu_provider_t no_vtable = {0};
    HU_ASSERT_EQ(hu_empty_retry_chat(NULL, &alloc, &req, "m", 1, 0.7, &out),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_empty_retry_chat(&no_vtable, &alloc, &req, "m", 1, 0.7, &out),
                 HU_ERR_INVALID_ARGUMENT);
}

/* ── wired through hu_agent_turn ───────────────────────────────────────────── */
#ifdef HU_ENABLE_SQLITE
#include "human/agent/world_model_bridge.h"
#include "human/memory/graph.h"

/* Answers a request ending in the retry nudge with a fresh reply; every other
 * request gets the discarded-draft shape (empty body, 9 completion tokens). */
typedef struct er_turn_mock {
    size_t calls;
    size_t retry_calls;
} er_turn_mock_t;

static hu_error_t er_turn_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                               const char *model, size_t model_len, double temperature,
                               hu_chat_response_t *out) {
    (void)model;
    (void)model_len;
    (void)temperature;
    er_turn_mock_t *m = (er_turn_mock_t *)ctx;
    m->calls++;
    memset(out, 0, sizeof(*out));
    out->usage.completion_tokens = 9;
    const hu_chat_message_t *last =
        req->messages_count ? &req->messages[req->messages_count - 1] : NULL;
    if (last && last->content && last->content_len == sizeof(HU_EMPTY_RETRY_NUDGE) - 1 &&
        memcmp(last->content, HU_EMPTY_RETRY_NUDGE, last->content_len) == 0) {
        m->retry_calls++;
        const char *body = "fresh retry reply";
        size_t n = strlen(body);
        char *buf = (char *)alloc->alloc(alloc->ctx, n + 1);
        if (!buf)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(buf, body, n + 1);
        out->content = buf;
        out->content_len = n;
    }
    return HU_OK;
}

static const char *er_turn_name(void *ctx) {
    (void)ctx;
    return "er_mock";
}

static hu_provider_vtable_t er_turn_vtable = {.chat = er_turn_chat, .get_name = er_turn_name};

typedef struct er_fixture {
    hu_allocator_t alloc;
    hu_provider_t prov;
    hu_graph_t *g;
    hu_w7_facade_t *wf;
    hu_agent_t agent;
    er_turn_mock_t mock;
} er_fixture_t;

static void er_open(er_fixture_t *f) {
    memset(f, 0, sizeof(*f));
    f->alloc = hu_system_allocator();
    f->prov.ctx = &f->mock;
    f->prov.vtable = &er_turn_vtable;
    HU_ASSERT_EQ(hu_graph_open(&f->alloc, NULL, 0, &f->g), HU_OK);
    HU_ASSERT_EQ(hu_w7_facade_open(f->g, &f->alloc, &f->wf), HU_OK);
    HU_ASSERT_EQ(hu_agent_from_config(&f->agent, &f->alloc, f->prov, NULL, 0, NULL, NULL, NULL,
                                      NULL, "er-mock-model", 13, "er_mock", 7, 0.7, ".", 1,
                                      /*max_tool_iterations=*/5, 50, false, 0, NULL, 0, NULL, 0,
                                      NULL),
                 HU_OK);
    f->agent.verifier_graph = f->g;
    f->agent.w7_facade = f->wf;
    f->agent.memory_session_id = "er-session";
    f->agent.memory_session_id_len = 10;
    f->agent.active_channel = "imessage";
    f->agent.active_channel_len = 8;
}

static void er_close(er_fixture_t *f) {
    hu_agent_deinit(&f->agent);
    hu_graph_close(f->g, &f->alloc);
}

/* Runs one turn under `mode` ("off"/"shadow"/"live", NULL = unset) and reports
 * whether the reply the turn returned contains the retry's text. */
static bool er_run_turn(const char *mode, er_fixture_t *f) {
    const char *prev = getenv("HU_EMPTY_REPLY_RETRY");
    char *saved = prev ? strdup(prev) : NULL;
    if (mode)
        setenv("HU_EMPTY_REPLY_RETRY", mode, 1);
    else
        unsetenv("HU_EMPTY_REPLY_RETRY");
    char *resp = NULL;
    size_t resp_len = 0;
    (void)hu_agent_turn(&f->agent, "you around?", 11, &resp, &resp_len);
    bool got_retry = resp && strstr(resp, "fresh retry reply") != NULL;
    if (resp)
        f->alloc.free(f->alloc.ctx, resp, resp_len + 1);
    if (saved) {
        setenv("HU_EMPTY_REPLY_RETRY", saved, 1);
        free(saved);
    } else {
        unsetenv("HU_EMPTY_REPLY_RETRY");
    }
    return got_retry;
}

static void empty_retry_turn_off_makes_no_retry_call(void) {
    er_fixture_t f;
    er_open(&f);
    HU_ASSERT_FALSE(er_run_turn(NULL, &f));
    HU_ASSERT_TRUE(f.mock.calls >= 1);
    HU_ASSERT_EQ(f.mock.retry_calls, 0u);
    er_close(&f);
}

static void empty_retry_turn_shadow_retries_but_keeps_empty_reply(void) {
    er_fixture_t f;
    er_open(&f);
    HU_ASSERT_FALSE(er_run_turn("shadow", &f));
    HU_ASSERT_TRUE(f.mock.retry_calls >= 1);
    er_close(&f);
}

static void empty_retry_turn_live_returns_retry_text(void) {
    er_fixture_t f;
    er_open(&f);
    HU_ASSERT_TRUE(er_run_turn("live", &f));
    HU_ASSERT_TRUE(f.mock.retry_calls >= 1);
    er_close(&f);
}
#endif /* HU_ENABLE_SQLITE */

void run_empty_retry_tests(void) {
    HU_TEST_SUITE("empty_retry");
    HU_RUN_TEST(empty_retry_applies_to_stripped_draft_when_gated_on);
    HU_RUN_TEST(empty_retry_off_gate_never_applies);
    HU_RUN_TEST(empty_retry_never_applies_twice);
    HU_RUN_TEST(empty_retry_ignores_reply_with_text);
    HU_RUN_TEST(empty_retry_ignores_tool_call_step);
    HU_RUN_TEST(empty_retry_ignores_backend_that_generated_nothing);
    HU_RUN_TEST(empty_retry_mode_reads_env_default_off);
    HU_RUN_TEST(empty_retry_chat_appends_nudge_and_leaves_request_alone);
    HU_RUN_TEST(empty_retry_chat_rejects_bad_args);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(empty_retry_turn_off_makes_no_retry_call);
    HU_RUN_TEST(empty_retry_turn_shadow_retries_but_keeps_empty_reply);
    HU_RUN_TEST(empty_retry_turn_live_returns_retry_text);
#endif
}
