/* Local-only prompt spans (providers/local_only.h): the HU_THREAD_CONTEXT
 * "## Recent thread" block may reach the on-device model but never a cloud
 * one. Pins the strip primitive and, end to end through the reliable
 * provider, that a cloud fallback attempt (prod: mlx_local primary failing
 * over to the gemini extra) carries no thread block while the local primary
 * still gets it and the rest of the prompt is unchanged. Fakes only. */
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/provider.h"
#include "human/providers/local_only.h"
#include "human/providers/reliable.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

#define BLOCK           \
    HU_LOCAL_ONLY_BEGIN \
    " (you and Mike, oldest first)\n[2d ago]\nMike: secret plans\nyou: ok\n" HU_LOCAL_ONLY_END

static const char k_prompt[] = "persona head\n" BLOCK "guard tail\n";
static const char k_prompt_stripped[] = "persona head\nguard tail\n";

static void test_local_only_names(void) {
    HU_ASSERT_TRUE(hu_local_only_provider_name_is_local("mlx_local"));
    HU_ASSERT_TRUE(hu_local_only_provider_name_is_local("ollama"));
    HU_ASSERT_TRUE(hu_local_only_provider_name_is_local("llamacpp"));
    HU_ASSERT_FALSE(hu_local_only_provider_name_is_local("gemini"));
    HU_ASSERT_FALSE(hu_local_only_provider_name_is_local("compatible"));
    HU_ASSERT_FALSE(hu_local_only_provider_name_is_local("mlx_localhost.evil"));
    HU_ASSERT_FALSE(hu_local_only_provider_name_is_local(""));
    HU_ASSERT_FALSE(hu_local_only_provider_name_is_local(NULL));
}

static void test_local_only_strip_removes_span_keeps_rest(void) {
    hu_allocator_t a = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_local_only_strip(&a, k_prompt, sizeof(k_prompt) - 1, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, k_prompt_stripped);
    HU_ASSERT_EQ(out_len, sizeof(k_prompt_stripped) - 1);
    a.free(a.ctx, out, out_len + 1);
}

static void test_local_only_strip_no_span_returns_null(void) {
    hu_allocator_t a = hu_system_allocator();
    static const char plain[] = "persona\nnothing local here\n";
    char *out = (char *)1;
    size_t out_len = 9;
    HU_ASSERT_EQ(hu_local_only_strip(&a, plain, sizeof(plain) - 1, &out, &out_len), HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(out_len, 0);
    /* The marker mid-line is not a span start. */
    static const char mid[] = "see ## Recent thread inline\n";
    HU_ASSERT_EQ(hu_local_only_strip(&a, mid, sizeof(mid) - 1, &out, &out_len), HU_OK);
    HU_ASSERT_NULL(out);
}

/* The positional prompt cap can cut the block's tail (END line included):
 * fail closed — drop everything from BEGIN on. */
static void test_local_only_strip_missing_end_fails_closed(void) {
    hu_allocator_t a = hu_system_allocator();
    static const char cut[] = "persona head\n" HU_LOCAL_ONLY_BEGIN " (x)\nMike: secret pl";
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_local_only_strip(&a, cut, sizeof(cut) - 1, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, "persona head\n");
    a.free(a.ctx, out, out_len + 1);
}

static void test_local_only_strip_two_spans(void) {
    hu_allocator_t a = hu_system_allocator();
    static const char two[] = "a\n" BLOCK "b\n" BLOCK "c\n";
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_local_only_strip(&a, two, sizeof(two) - 1, &out, &out_len), HU_OK);
    HU_ASSERT_STR_EQ(out, "a\nb\nc\n");
    a.free(a.ctx, out, out_len + 1);
}

/* ── through the reliable provider ──────────────────────────────────── */

typedef struct cap_provider {
    const char *name;
    int calls;
    hu_error_t fail_err; /* HU_OK = succeed */
    char seen_system[512];
    bool saw_cache_id;
} cap_provider_t;

static hu_error_t cap_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                           const char *model, size_t model_len, double temperature,
                           hu_chat_response_t *out) {
    (void)model;
    (void)model_len;
    (void)temperature;
    cap_provider_t *p = (cap_provider_t *)ctx;
    p->calls++;
    p->seen_system[0] = '\0';
    for (size_t i = 0; i < req->messages_count; i++) {
        if (req->messages[i].role == HU_ROLE_SYSTEM) {
            size_t n = req->messages[i].content_len;
            if (n >= sizeof(p->seen_system))
                n = sizeof(p->seen_system) - 1;
            memcpy(p->seen_system, req->messages[i].content, n);
            p->seen_system[n] = '\0';
        }
    }
    p->saw_cache_id = req->prompt_cache_id != NULL;
    if (p->fail_err != HU_OK)
        return p->fail_err;
    memset(out, 0, sizeof(*out));
    size_t n = strlen(p->name);
    char *buf = (char *)alloc->alloc(alloc->ctx, n + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, p->name, n + 1);
    out->content = buf;
    out->content_len = n;
    return HU_OK;
}

static hu_error_t cap_chat_with_system(void *ctx, hu_allocator_t *alloc, const char *sp,
                                       size_t sp_len, const char *msg, size_t msg_len,
                                       const char *model, size_t model_len, double temperature,
                                       char **out, size_t *out_len) {
    (void)msg;
    (void)msg_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    cap_provider_t *p = (cap_provider_t *)ctx;
    p->calls++;
    size_t n = sp_len < sizeof(p->seen_system) - 1 ? sp_len : sizeof(p->seen_system) - 1;
    memcpy(p->seen_system, sp, n);
    p->seen_system[n] = '\0';
    if (p->fail_err != HU_OK)
        return p->fail_err;
    *out = hu_strndup(alloc, "ok", 2);
    *out_len = 2;
    return HU_OK;
}

static const char *cap_get_name(void *ctx) {
    return ((cap_provider_t *)ctx)->name;
}

static void cap_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const hu_provider_vtable_t cap_vtable = {
    .chat = cap_chat,
    .chat_with_system = cap_chat_with_system,
    .get_name = cap_get_name,
    .deinit = cap_deinit,
};

typedef struct lo_rig {
    hu_allocator_t alloc;
    cap_provider_t prim;
    cap_provider_t cloud;
    hu_reliable_provider_entry_t extras[1];
    hu_provider_t reliable;
} lo_rig_t;

/* Prod shape: an mlx_local primary (reports "compatible") that fails, and a
 * gemini extra. `primary_local` mirrors what from_config sets. */
static void lo_rig_init(lo_rig_t *g, hu_error_t prim_err, bool primary_local) {
    memset(g, 0, sizeof(*g));
    g->alloc = hu_system_allocator();
    g->prim.name = "compatible";
    g->prim.fail_err = prim_err;
    g->cloud.name = "gemini";
    hu_provider_t prim = {.ctx = &g->prim, .vtable = &cap_vtable};
    hu_provider_t cloud = {.ctx = &g->cloud, .vtable = &cap_vtable};
    g->extras[0].name = "gemini";
    g->extras[0].name_len = 6;
    g->extras[0].provider = cloud;
    HU_ASSERT_EQ(hu_reliable_create_ex(&g->alloc, prim, 0, 50, g->extras, 1, NULL, 0, &g->reliable),
                 HU_OK);
    hu_reliable_set_circuit(&g->reliable, -1, 0);
    hu_reliable_set_primary_local(&g->reliable, primary_local);
}

static hu_error_t lo_rig_chat(lo_rig_t *g, hu_chat_response_t *resp) {
    hu_chat_message_t msgs[2];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = HU_ROLE_SYSTEM;
    msgs[0].content = k_prompt;
    msgs[0].content_len = sizeof(k_prompt) - 1;
    msgs[1].role = HU_ROLE_USER;
    msgs[1].content = "are you coming";
    msgs[1].content_len = 14;
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.messages = msgs;
    req.messages_count = 2;
    req.prompt_cache_id = "persona-v1";
    req.prompt_cache_id_len = 10;
    return g->reliable.vtable->chat(g->reliable.ctx, &g->alloc, &req, "GLM-4.5-Air-4bit", 16, 0.7,
                                    resp);
}

static void test_reliable_cloud_fallback_carries_no_thread_block(void) {
    lo_rig_t g;
    lo_rig_init(&g, HU_ERR_PROVIDER_RESPONSE, true);
    hu_chat_response_t resp;
    HU_ASSERT_EQ(lo_rig_chat(&g, &resp), HU_OK);
    /* The local primary saw the block (and the cache id). */
    HU_ASSERT_GE(g.prim.calls, 1);
    HU_ASSERT_STR_CONTAINS(g.prim.seen_system, "secret plans");
    HU_ASSERT_TRUE(g.prim.saw_cache_id);
    /* The cloud fallback answered, and its request had no thread block. */
    HU_ASSERT_EQ(g.cloud.calls, 1);
    HU_ASSERT_STR_EQ(resp.content, "gemini");
    HU_ASSERT_STR_NOT_CONTAINS(g.cloud.seen_system, HU_LOCAL_ONLY_BEGIN);
    HU_ASSERT_STR_NOT_CONTAINS(g.cloud.seen_system, "secret plans");
    HU_ASSERT_STR_EQ(g.cloud.seen_system, k_prompt_stripped);
    HU_ASSERT_FALSE(g.cloud.saw_cache_id);
    hu_chat_response_free(&g.alloc, &resp);
    g.reliable.vtable->deinit(g.reliable.ctx, &g.alloc);
}

static void test_reliable_chat_with_system_fallback_strips(void) {
    lo_rig_t g;
    lo_rig_init(&g, HU_ERR_PROVIDER_RESPONSE, true);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(g.reliable.vtable->chat_with_system(g.reliable.ctx, &g.alloc, k_prompt,
                                                     sizeof(k_prompt) - 1, "hi", 2, "m", 1, 0.5,
                                                     &out, &out_len),
                 HU_OK);
    HU_ASSERT_STR_CONTAINS(g.prim.seen_system, "secret plans");
    HU_ASSERT_STR_EQ(g.cloud.seen_system, k_prompt_stripped);
    g.alloc.free(g.alloc.ctx, out, out_len + 1);
    g.reliable.vtable->deinit(g.reliable.ctx, &g.alloc);
}

/* A primary not known to be local is treated as cloud: it never sees the
 * block either (fail closed). */
static void test_reliable_unknown_primary_is_stripped_too(void) {
    lo_rig_t g;
    lo_rig_init(&g, HU_OK, false);
    HU_ASSERT_FALSE(hu_reliable_primary_is_local(&g.reliable));
    hu_chat_response_t resp;
    HU_ASSERT_EQ(lo_rig_chat(&g, &resp), HU_OK);
    HU_ASSERT_EQ(g.prim.calls, 1);
    HU_ASSERT_STR_EQ(g.prim.seen_system, k_prompt_stripped);
    HU_ASSERT_EQ(g.cloud.calls, 0);
    hu_chat_response_free(&g.alloc, &resp);
    g.reliable.vtable->deinit(g.reliable.ctx, &g.alloc);
}

static void test_reliable_primary_local_flag(void) {
    lo_rig_t g;
    lo_rig_init(&g, HU_OK, true);
    HU_ASSERT_TRUE(hu_reliable_primary_is_local(&g.reliable));
    hu_provider_t not_reliable = {.ctx = &g.prim, .vtable = &cap_vtable};
    HU_ASSERT_FALSE(hu_reliable_primary_is_local(&not_reliable));
    HU_ASSERT_FALSE(hu_reliable_primary_is_local(NULL));
    g.reliable.vtable->deinit(g.reliable.ctx, &g.alloc);
}

void run_local_only_tests(void) {
    HU_TEST_SUITE("local_only");
    HU_RUN_TEST(test_local_only_names);
    HU_RUN_TEST(test_local_only_strip_removes_span_keeps_rest);
    HU_RUN_TEST(test_local_only_strip_no_span_returns_null);
    HU_RUN_TEST(test_local_only_strip_missing_end_fails_closed);
    HU_RUN_TEST(test_local_only_strip_two_spans);
    HU_RUN_TEST(test_reliable_cloud_fallback_carries_no_thread_block);
    HU_RUN_TEST(test_reliable_chat_with_system_fallback_strips);
    HU_RUN_TEST(test_reliable_unknown_primary_is_stripped_too);
    HU_RUN_TEST(test_reliable_primary_local_flag);
}
