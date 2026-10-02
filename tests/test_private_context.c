/* src/providers/private_context.c — private prompt blocks never reach a
 * fallback attempt.
 *
 * Owner rule (2026-10-01): real message or memory text never reaches a cloud
 * model without explicit opt-in. Production's reply provider is the reliable
 * wrapper around local mlx, with gemini as the declared fallback; when the
 * local attempt fails, the SAME request is re-issued to the cloud. These tests
 * pin that the HU_IMMERSIVE_CONTEXT block is removed from every attempt except
 * the primary one, on all three reliable paths (chat, chat_with_system,
 * stream_chat) and on the degradation fallback model. */
#include "human/agent/degradation.h"
#include "human/core/allocator.h"
#include "human/provider.h"
#include "human/providers/private_context.h"
#include "human/providers/reliable.h"
#include "test_framework.h"
#include <string.h>

#define BLOCK                                                                                \
    HU_PRIVATE_BLOCK_IMMERSIVE_CONTEXT "Still open between you two:\n- PRIVATE_MARKER call " \
                                       "her\n\n"

static const char k_sys_private[] = "PERSONA head\n\n" BLOCK "MEMORY rest of prompt\n";
static const char k_sys_public[] = "PERSONA head\n\nMEMORY rest of prompt\n";

/* ── strip ─────────────────────────────────────────────────────────────── */

static void private_context_strip_restores_text_without_block(void) {
    char buf[256];
    memcpy(buf, k_sys_private, sizeof(k_sys_private));
    HU_ASSERT_TRUE(hu_private_context_present(buf, strlen(buf)));
    size_t n = hu_private_context_strip(buf, strlen(buf));
    HU_ASSERT_EQ(n, sizeof(k_sys_public) - 1);
    HU_ASSERT_STR_EQ(buf, k_sys_public);
    HU_ASSERT_FALSE(hu_private_context_present(buf, n));
}

static void private_context_strip_leaves_other_text_alone(void) {
    /* No block, and a heading quoted mid-line (not at a line start). */
    char buf[160];
    static const char quoted[] = "they said \"## What you know right now\\n\" once\n";
    memcpy(buf, quoted, sizeof(quoted));
    HU_ASSERT_FALSE(hu_private_context_present(buf, strlen(buf)));
    HU_ASSERT_EQ(hu_private_context_strip(buf, strlen(buf)), sizeof(quoted) - 1);
    HU_ASSERT_STR_EQ(buf, quoted);
}

static void private_context_strip_block_at_end_without_blank_line(void) {
    char buf[160];
    static const char tail[] = "head\n" HU_PRIVATE_BLOCK_IMMERSIVE_CONTEXT "- PRIVATE_MARKER";
    memcpy(buf, tail, sizeof(tail));
    HU_ASSERT_EQ(hu_private_context_strip(buf, strlen(buf)), 5);
    HU_ASSERT_STR_EQ(buf, "head\n");
}

static void private_context_redact_request_touches_only_system_messages(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_chat_message_t msgs[2] = {
        {.role = HU_ROLE_SYSTEM,
         .content = k_sys_private,
         .content_len = sizeof(k_sys_private) - 1},
        {.role = HU_ROLE_USER, .content = BLOCK, .content_len = sizeof(BLOCK) - 1},
    };
    hu_chat_request_t req = {
        .messages = msgs, .messages_count = 2, .prompt_cache_id = "pcid", .prompt_cache_id_len = 4};
    hu_private_request_t scratch;
    const hu_chat_request_t *r = hu_private_context_redact_request(&a, &req, &scratch);
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT_TRUE(r != &req);
    HU_ASSERT_STR_EQ(r->messages[0].content, k_sys_public);
    HU_ASSERT_EQ(r->messages[0].content_len, sizeof(k_sys_public) - 1);
    HU_ASSERT_TRUE(r->messages[1].content == msgs[1].content); /* user text untouched */
    HU_ASSERT_NULL(r->prompt_cache_id);                        /* hashed the unredacted prompt */
    HU_ASSERT_TRUE(msgs[0].content == k_sys_private);          /* caller's request unchanged */
    hu_private_context_release(&a, &scratch);

    msgs[0].content = k_sys_public;
    msgs[0].content_len = sizeof(k_sys_public) - 1;
    r = hu_private_context_redact_request(&a, &req, &scratch);
    HU_ASSERT_TRUE(r == &req); /* nothing private: no copy */
    hu_private_context_release(&a, &scratch);
}

/* ── recording fake provider ───────────────────────────────────────────── */

typedef struct rec_provider {
    const char *name;
    bool fail;
    const char *fail_model; /* non-NULL: fail only for this model */
    int calls;
    bool saw_private;          /* any call carried the private block */
    bool saw_private_fallback; /* a call for a model other than fail_model did */
} rec_provider_t;

static bool rec_should_fail(rec_provider_t *p, const char *model, size_t model_len) {
    if (p->fail)
        return true;
    return p->fail_model && strlen(p->fail_model) == model_len &&
           memcmp(p->fail_model, model, model_len) == 0;
}

static void rec_mark(rec_provider_t *p, const char *model, size_t model_len) {
    p->saw_private = true;
    if (p->fail_model &&
        !(strlen(p->fail_model) == model_len && memcmp(p->fail_model, model, model_len) == 0))
        p->saw_private_fallback = true;
}

static void rec_note(rec_provider_t *p, const hu_chat_request_t *req, const char *model,
                     size_t model_len) {
    for (size_t i = 0; i < req->messages_count; i++)
        if (req->messages[i].content && strstr(req->messages[i].content, "PRIVATE_MARKER") != NULL)
            rec_mark(p, model, model_len);
}

static char *rec_reply(hu_allocator_t *alloc, const char *name) {
    size_t n = strlen(name);
    char *s = (char *)alloc->alloc(alloc->ctx, n + 1);
    if (s)
        memcpy(s, name, n + 1);
    return s;
}

static hu_error_t rec_chat_with_system(void *ctx, hu_allocator_t *alloc, const char *sys,
                                       size_t sys_len, const char *msg, size_t msg_len,
                                       const char *model, size_t model_len, double temperature,
                                       char **out, size_t *out_len) {
    (void)msg;
    (void)msg_len;
    (void)temperature;
    rec_provider_t *p = (rec_provider_t *)ctx;
    p->calls++;
    if (sys && sys_len > 0 && strstr(sys, "PRIVATE_MARKER"))
        rec_mark(p, model, model_len);
    if (rec_should_fail(p, model, model_len))
        return HU_ERR_PROVIDER_RESPONSE;
    *out = rec_reply(alloc, p->name);
    *out_len = strlen(p->name);
    return HU_OK;
}

static hu_error_t rec_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                           const char *model, size_t model_len, double temperature,
                           hu_chat_response_t *out) {
    (void)temperature;
    rec_provider_t *p = (rec_provider_t *)ctx;
    p->calls++;
    rec_note(p, req, model, model_len);
    if (rec_should_fail(p, model, model_len))
        return HU_ERR_PROVIDER_RESPONSE;
    out->content = rec_reply(alloc, p->name);
    out->content_len = strlen(p->name);
    return HU_OK;
}

static hu_error_t rec_stream(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                             const char *model, size_t model_len, double temperature,
                             hu_stream_callback_t cb, void *cb_ctx, hu_stream_chat_result_t *out) {
    (void)alloc;
    (void)temperature;
    (void)cb;
    (void)cb_ctx;
    (void)out;
    rec_provider_t *p = (rec_provider_t *)ctx;
    p->calls++;
    rec_note(p, req, model, model_len);
    return rec_should_fail(p, model, model_len) ? HU_ERR_PROVIDER_RESPONSE : HU_OK;
}

static const char *rec_name(void *ctx) {
    return ((rec_provider_t *)ctx)->name;
}

static void rec_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const hu_provider_vtable_t rec_vtable = {
    .chat_with_system = rec_chat_with_system,
    .chat = rec_chat,
    .stream_chat = rec_stream,
    .get_name = rec_name,
    .deinit = rec_deinit,
};

typedef struct fb_rig {
    hu_allocator_t alloc;
    rec_provider_t local;
    rec_provider_t cloud;
    hu_reliable_provider_entry_t extras[1];
    hu_provider_t reliable;
} fb_rig_t;

/* Local primary fails; the cloud extra answers — production's fallback shape. */
static void fb_rig_init(fb_rig_t *g) {
    memset(g, 0, sizeof(*g));
    g->alloc = hu_system_allocator();
    g->local.name = "local";
    g->local.fail = true;
    g->cloud.name = "cloud";
    g->extras[0].name = "cloud";
    g->extras[0].name_len = 5;
    g->extras[0].provider = (hu_provider_t){.ctx = &g->cloud, .vtable = &rec_vtable};
    hu_provider_t inner = {.ctx = &g->local, .vtable = &rec_vtable};
    HU_ASSERT_EQ(
        hu_reliable_create_ex(&g->alloc, inner, 0, 50, g->extras, 1, NULL, 0, &g->reliable), HU_OK);
}

static void fb_rig_deinit(fb_rig_t *g) {
    g->reliable.vtable->deinit(g->reliable.ctx, &g->alloc);
}

static void private_context_reliable_chat_fallback_carries_no_block(void) {
    fb_rig_t g;
    fb_rig_init(&g);
    hu_chat_message_t msgs[1] = {
        {.role = HU_ROLE_SYSTEM,
         .content = k_sys_private,
         .content_len = sizeof(k_sys_private) - 1},
    };
    hu_chat_request_t req = {.messages = msgs, .messages_count = 1};
    hu_chat_response_t resp;
    HU_ASSERT_EQ(
        g.reliable.vtable->chat(g.reliable.ctx, &g.alloc, &req, "local-model", 11, 0.5, &resp),
        HU_OK);
    HU_ASSERT_STR_EQ(resp.content, "cloud");
    HU_ASSERT_TRUE(g.local.saw_private); /* primary attempt: block kept */
    HU_ASSERT_TRUE(g.cloud.calls > 0);
    HU_ASSERT_FALSE(g.cloud.saw_private); /* fallback attempt: block removed */
    g.alloc.free(g.alloc.ctx, (void *)resp.content, resp.content_len + 1);
    fb_rig_deinit(&g);
}

static void private_context_reliable_chat_with_system_fallback_carries_no_block(void) {
    fb_rig_t g;
    fb_rig_init(&g);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(g.reliable.vtable->chat_with_system(g.reliable.ctx, &g.alloc, k_sys_private,
                                                     sizeof(k_sys_private) - 1, "hi", 2,
                                                     "local-model", 11, 0.5, &out, &out_len),
                 HU_OK);
    HU_ASSERT_STR_EQ(out, "cloud");
    HU_ASSERT_TRUE(g.local.saw_private);
    HU_ASSERT_FALSE(g.cloud.saw_private);
    g.alloc.free(g.alloc.ctx, out, out_len + 1);
    fb_rig_deinit(&g);
}

static void private_context_reliable_stream_fallback_carries_no_block(void) {
    fb_rig_t g;
    fb_rig_init(&g);
    hu_chat_message_t msgs[1] = {
        {.role = HU_ROLE_SYSTEM,
         .content = k_sys_private,
         .content_len = sizeof(k_sys_private) - 1},
    };
    hu_chat_request_t req = {.messages = msgs, .messages_count = 1};
    hu_stream_chat_result_t res;
    memset(&res, 0, sizeof(res));
    HU_ASSERT_EQ(g.reliable.vtable->stream_chat(g.reliable.ctx, &g.alloc, &req, "local-model", 11,
                                                0.5, NULL, NULL, &res),
                 HU_OK);
    HU_ASSERT_TRUE(g.local.saw_private);
    HU_ASSERT_EQ(g.cloud.calls, 1);
    HU_ASSERT_FALSE(g.cloud.saw_private);
    fb_rig_deinit(&g);
}

/* A declared fallback MODEL on the primary provider is a fallback attempt too. */
static void private_context_reliable_model_fallback_carries_no_block(void) {
    hu_allocator_t a = hu_system_allocator();
    rec_provider_t local = {.name = "local", .fail_model = "glm"};
    hu_reliable_fallback_model_t fbm[1] = {{"gemini-x", 8}};
    hu_reliable_model_fallback_entry_t entry = {"glm", 3, fbm, 1};
    hu_provider_t rel;
    HU_ASSERT_EQ(hu_reliable_create_ex(&a, (hu_provider_t){.ctx = &local, .vtable = &rec_vtable}, 0,
                                       50, NULL, 0, &entry, 1, &rel),
                 HU_OK);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(rel.vtable->chat_with_system(rel.ctx, &a, k_sys_private, sizeof(k_sys_private) - 1,
                                              "hi", 2, "glm", 3, 0.5, &out, &out_len),
                 HU_OK);
    HU_ASSERT_EQ(local.calls, 2);                /* glm failed, then gemini-x answered */
    HU_ASSERT_TRUE(local.saw_private);           /* the primary (glm) attempt kept it */
    HU_ASSERT_FALSE(local.saw_private_fallback); /* the gemini-x attempt did not */
    a.free(a.ctx, out, out_len + 1);
    rel.vtable->deinit(rel.ctx, &a);
}

static void private_context_degradation_fallback_model_carries_no_block(void) {
    hu_allocator_t a = hu_system_allocator();
    rec_provider_t prov = {.name = "local", .fail_model = "primary"};
    hu_provider_t p = {.ctx = &prov, .vtable = &rec_vtable};
    char fallback[] = "fallback";
    hu_provider_degradation_config_t cfg = {
        .enabled = true, .fallback_model = fallback, .fallback_model_len = 8, .max_retries = 1};
    hu_circuit_breaker_init(&cfg.breaker, 3, 5000);
    hu_chat_message_t msgs[1] = {
        {.role = HU_ROLE_SYSTEM,
         .content = k_sys_private,
         .content_len = sizeof(k_sys_private) - 1},
    };
    hu_chat_request_t req = {.messages = msgs, .messages_count = 1};
    hu_degradation_result_t res;
    HU_ASSERT_EQ(hu_provider_degrade_chat(&cfg, &p, &a, &req, "primary", 7, 0.5, &res), HU_OK);
    HU_ASSERT_EQ((int)res.strategy_used, (int)HU_DEGRADE_FALLBACK);
    HU_ASSERT_EQ(prov.calls, 2);
    HU_ASSERT_TRUE(prov.saw_private);           /* primary model attempt kept it */
    HU_ASSERT_FALSE(prov.saw_private_fallback); /* fallback model attempt did not */
    hu_chat_response_free(&a, &res.response);
}

void run_private_context_tests(void) {
    HU_TEST_SUITE("PrivateContext");
    HU_RUN_TEST(private_context_strip_restores_text_without_block);
    HU_RUN_TEST(private_context_strip_leaves_other_text_alone);
    HU_RUN_TEST(private_context_strip_block_at_end_without_blank_line);
    HU_RUN_TEST(private_context_redact_request_touches_only_system_messages);
    HU_RUN_TEST(private_context_reliable_chat_fallback_carries_no_block);
    HU_RUN_TEST(private_context_reliable_chat_with_system_fallback_carries_no_block);
    HU_RUN_TEST(private_context_reliable_stream_fallback_carries_no_block);
    HU_RUN_TEST(private_context_reliable_model_fallback_carries_no_block);
    HU_RUN_TEST(private_context_degradation_fallback_model_carries_no_block);
}
