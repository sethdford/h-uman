/* tests/test_chat_oneshot.c
 *
 * hu_provider_chat_oneshot (src/providers/chat_oneshot.c): the one-system,
 * one-user, short-answer request with thinking OFF that init_proposer uses
 * and the prospective Decide judge reuses. A recording mock provider pins
 * the request shape — thinking_budget 0 is the whole point (CLAUDE.md
 * "Gemini 3.x thinking-token budget gotcha"). No network. */
#include "test_framework.h"

#include "human/providers/chat_oneshot.h"
#include <string.h>

typedef struct mock {
    int calls;
    int cws_calls;
    int thinking_budget;
    uint32_t max_tokens;
    double temperature;
    char response_format[32];
    char system[64];
    char user[64];
    char model[32];
    const char *reply; /* NULL -> empty answer */
} mock_t;

static hu_error_t mock_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                            const char *model, size_t model_len, double temperature,
                            hu_chat_response_t *out) {
    mock_t *m = (mock_t *)ctx;
    m->calls++;
    m->thinking_budget = req->thinking_budget;
    m->max_tokens = req->max_tokens;
    m->temperature = temperature;
    snprintf(m->response_format, sizeof(m->response_format), "%.*s", (int)req->response_format_len,
             req->response_format ? req->response_format : "");
    HU_ASSERT_EQ(req->messages_count, (size_t)2);
    HU_ASSERT_EQ(req->messages[0].role, HU_ROLE_SYSTEM);
    HU_ASSERT_EQ(req->messages[1].role, HU_ROLE_USER);
    snprintf(m->system, sizeof(m->system), "%.*s", (int)req->messages[0].content_len,
             req->messages[0].content);
    snprintf(m->user, sizeof(m->user), "%.*s", (int)req->messages[1].content_len,
             req->messages[1].content);
    snprintf(m->model, sizeof(m->model), "%.*s", (int)model_len, model);
    memset(out, 0, sizeof(*out));
    if (m->reply) {
        size_t n = strlen(m->reply);
        char *c = (char *)alloc->alloc(alloc->ctx, n + 1);
        memcpy(c, m->reply, n + 1);
        out->content = c;
        out->content_len = n;
    }
    return HU_OK;
}

static hu_error_t mock_cws(void *ctx, hu_allocator_t *alloc, const char *sys, size_t sys_len,
                           const char *msg, size_t msg_len, const char *model, size_t model_len,
                           double temperature, char **out, size_t *out_len) {
    (void)sys;
    (void)sys_len;
    (void)msg;
    (void)msg_len;
    (void)model;
    (void)model_len;
    mock_t *m = (mock_t *)ctx;
    m->cws_calls++;
    m->temperature = temperature;
    *out = (char *)alloc->alloc(alloc->ctx, 5);
    memcpy(*out, "fire", 5);
    *out_len = 4;
    return HU_OK;
}

static void oneshot_sends_a_short_thinking_off_request(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_t m;
    memset(&m, 0, sizeof(m));
    m.reply = "not_now";
    hu_provider_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.chat = mock_chat;
    vt.chat_with_system = mock_cws;
    hu_provider_t p = {.ctx = &m, .vtable = &vt};
    const hu_chat_oneshot_opts_t opts = {
        .temperature = 0.0, .max_tokens = 16, .json_object = false};
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(
        hu_provider_chat_oneshot(&alloc, &p, "glm", 3, "sys", 3, "usr", 3, &opts, &out, &len),
        HU_OK);
    HU_ASSERT_EQ(m.calls, 1);
    HU_ASSERT_EQ(m.cws_calls, 0); /* the structured chat() path is preferred */
    HU_ASSERT_EQ(m.thinking_budget, 0);
    HU_ASSERT_EQ(m.max_tokens, (uint32_t)16);
    HU_ASSERT_STR_EQ(m.response_format, "");
    HU_ASSERT_STR_EQ(m.system, "sys");
    HU_ASSERT_STR_EQ(m.user, "usr");
    HU_ASSERT_STR_EQ(m.model, "glm");
    HU_ASSERT_STR_EQ(out, "not_now");
    HU_ASSERT_EQ(len, (size_t)7);
    alloc.free(alloc.ctx, out, len + 1);

    const hu_chat_oneshot_opts_t json = {
        .temperature = 0.2, .max_tokens = 512, .json_object = true};
    m.reply = NULL; /* empty answer: HU_OK, nothing allocated */
    HU_ASSERT_EQ(hu_provider_chat_oneshot(&alloc, &p, NULL, 0, "s", 1, "u", 1, &json, &out, &len),
                 HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(len, (size_t)0);
    HU_ASSERT_STR_EQ(m.response_format, "json_object");
    HU_ASSERT_EQ(m.max_tokens, (uint32_t)512);
}

static void oneshot_falls_back_and_refuses_cleanly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_t m;
    memset(&m, 0, sizeof(m));
    hu_provider_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.chat_with_system = mock_cws;
    hu_provider_t p = {.ctx = &m, .vtable = &vt};
    const hu_chat_oneshot_opts_t opts = {
        .temperature = 0.1, .max_tokens = 16, .json_object = false};
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_provider_chat_oneshot(&alloc, &p, "m", 1, "s", 1, "u", 1, &opts, &out, &len),
                 HU_OK);
    HU_ASSERT_EQ(m.cws_calls, 1);
    HU_ASSERT_STR_EQ(out, "fire");
    alloc.free(alloc.ctx, out, len + 1);

    vt.chat_with_system = NULL;
    HU_ASSERT_EQ(hu_provider_chat_oneshot(&alloc, &p, "m", 1, "s", 1, "u", 1, &opts, &out, &len),
                 HU_ERR_NOT_SUPPORTED);
    hu_provider_t none = {.ctx = NULL, .vtable = NULL};
    HU_ASSERT_EQ(hu_provider_chat_oneshot(&alloc, &none, "m", 1, "s", 1, "u", 1, &opts, &out, &len),
                 HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(hu_provider_chat_oneshot(&alloc, &p, "m", 1, "s", 1, "u", 1, NULL, &out, &len),
                 HU_ERR_INVALID_ARGUMENT);
}

void run_chat_oneshot_tests(void) {
    HU_TEST_SUITE("chat oneshot");
    HU_RUN_TEST(oneshot_sends_a_short_thinking_off_request);
    HU_RUN_TEST(oneshot_falls_back_and_refuses_cleanly);
}
