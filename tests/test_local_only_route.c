/* tests/test_local_only_route.c
 *
 * local_only route selection (owner decision 2026-10-01): with the switch
 * LIVE, the routes that used to pick a cloud model stay on the primary local
 * model or are skipped —
 *   - the analytical/deep tier of inline routing (hard-coded
 *     gemini-3.1-pro-preview),
 *   - the S3 sensitivity switch (fallback_model / s3_local_model),
 *   - the on-device-failure retry (hard-coded gemini-3.1-flash-lite),
 *   - the response-guard slim retry's cloud fallback (gemini, openai).
 * Each is driven through the real production path with a fake provider that
 * records the model it was asked for. OFF arms prove the route exists (so a
 * LIVE pass is not vacuous). Hermetic: fakes and the HU_IS_TEST HTTP mock.
 */

#include "human/agent.h"
#include "human/agent/local_only_route.h"
#include "human/agent/model_router.h"
#include "human/agent/response_guard_retry.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/local_only_guard.h"
#include "human/provider.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

#define PRIMARY "lo-local-model"

static void lo_clean(void) {
    unsetenv("HU_LOCAL_ONLY");
    hu_local_only_reset();
}

static void router_defaults_untouched_when_off(void) {
    lo_clean();
    hu_model_router_config_t cfg = hu_model_router_default_config();
    HU_ASSERT_FALSE(hu_local_only_router_defaults(&cfg, PRIMARY, strlen(PRIMARY)));
    HU_ASSERT_STR_EQ(cfg.analytical_model, "gemini-3.1-pro-preview");
    hu_local_only_configure(HU_GATE_SHADOW); /* audit routes as OFF */
    HU_ASSERT_FALSE(hu_local_only_router_defaults(&cfg, PRIMARY, strlen(PRIMARY)));
    HU_ASSERT_STR_EQ(cfg.deep_model, "gemini-3.1-pro-preview");
    lo_clean();
}

static void router_defaults_become_primary_when_live(void) {
    lo_clean();
    hu_local_only_configure(HU_GATE_LIVE);
    hu_model_router_config_t cfg = hu_model_router_default_config();
    HU_ASSERT_TRUE(hu_local_only_router_defaults(&cfg, PRIMARY, strlen(PRIMARY)));
    HU_ASSERT_STR_EQ(cfg.reflexive_model, PRIMARY);
    HU_ASSERT_STR_EQ(cfg.conversational_model, PRIMARY);
    HU_ASSERT_STR_EQ(cfg.analytical_model, PRIMARY);
    HU_ASSERT_STR_EQ(cfg.deep_model, PRIMARY);
    HU_ASSERT_EQ(cfg.analytical_model_len, strlen(PRIMARY));
    /* No primary to stay on: leave the config alone rather than blank it. */
    hu_model_router_config_t empty = hu_model_router_default_config();
    HU_ASSERT_FALSE(hu_local_only_router_defaults(&empty, NULL, 0));
    lo_clean();
}

/* ── Fake provider that records the requested model ─────────────────────── */

typedef struct rec_prov {
    char models[8][64];
    unsigned calls;
    const char *fail_model; /* chat fails for this model */
} rec_prov_t;

static void rec_note(rec_prov_t *r, const char *model, size_t model_len) {
    if (r->calls < 8) {
        size_t n = model_len < 63 ? model_len : 63;
        memcpy(r->models[r->calls], model ? model : "", model ? n : 0);
        r->models[r->calls][model ? n : 0] = '\0';
    }
    r->calls++;
}

static bool rec_saw(const rec_prov_t *r, const char *model) {
    for (unsigned i = 0; i < r->calls && i < 8; i++)
        if (strcmp(r->models[i], model) == 0)
            return true;
    return false;
}

static hu_error_t rec_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                           const char *model, size_t model_len, double temperature,
                           hu_chat_response_t *out) {
    (void)req;
    (void)temperature;
    rec_prov_t *r = (rec_prov_t *)ctx;
    rec_note(r, model, model_len);
    memset(out, 0, sizeof(*out));
    if (r->fail_model && model && strlen(r->fail_model) == model_len &&
        memcmp(r->fail_model, model, model_len) == 0)
        return HU_ERR_PROVIDER_RESPONSE;
    const char *body = "sure thing";
    size_t n = strlen(body);
    char *buf = (char *)alloc->alloc(alloc->ctx, n + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, body, n + 1);
    out->content = buf;
    out->content_len = n;
    return HU_OK;
}

static hu_error_t rec_chat_sys_fail(void *ctx, hu_allocator_t *alloc, const char *sp, size_t sl,
                                    const char *m, size_t ml, const char *model, size_t model_len,
                                    double t, char **out, size_t *out_len) {
    (void)alloc;
    (void)sp;
    (void)sl;
    (void)m;
    (void)ml;
    (void)t;
    rec_note((rec_prov_t *)ctx, model, model_len);
    *out = NULL;
    *out_len = 0;
    return HU_ERR_PROVIDER_RESPONSE;
}

static const char *rec_name(void *ctx) {
    (void)ctx;
    return "rec_local";
}

static const hu_provider_vtable_t k_rec_vtable = {
    .chat_with_system = rec_chat_sys_fail,
    .chat = rec_chat,
    .get_name = rec_name,
};

/* Guard slim retry: a failing primary used to fall back to gemini/openai
 * created from config. The fallback block only compiles with HU_ENABLE_CURL
 * (the production library), so the decision is a predicate the block calls;
 * here it is pinned directly, and the end-to-end LIVE arm proves the primary's
 * error stands without any request reaching the backstop. */
static void guard_retry_cloud_fallback_blocked_when_live(void) {
    lo_clean();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    HU_ASSERT_TRUE(hu_response_guard_retry_cloud_fallback_allowed(&cfg)); /* OFF */
    HU_ASSERT_FALSE(hu_response_guard_retry_cloud_fallback_allowed(NULL));
    hu_local_only_configure(HU_GATE_SHADOW);
    HU_ASSERT_TRUE(hu_response_guard_retry_cloud_fallback_allowed(&cfg)); /* audit = OFF */
    hu_local_only_configure(HU_GATE_LIVE);
    HU_ASSERT_FALSE(hu_response_guard_retry_cloud_fallback_allowed(&cfg));

    hu_allocator_t alloc = hu_system_allocator();
    rec_prov_t rp = {0};
    rp.fail_model = PRIMARY;
    hu_provider_t primary = {.ctx = &rp, .vtable = &k_rec_vtable};
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_response_guard_retry_slim(&alloc, NULL, &cfg, &primary, PRIMARY,
                                                  strlen(PRIMARY), "hey", 3, &out, &out_len, NULL);
    HU_ASSERT_TRUE(err != HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_TRUE(rp.calls >= 1u); /* the local primary was tried */
    HU_ASSERT_EQ(hu_local_only_refused_count(), 0u);
    lo_clean();
}

#ifdef HU_ENABLE_SQLITE
#include "human/agent/world_model_bridge.h"
#include "human/memory/graph.h"

typedef struct lo_fix {
    hu_allocator_t alloc;
    hu_graph_t *g;
    hu_w7_facade_t *wf;
    hu_agent_t agent;
    rec_prov_t rec;
} lo_fix_t;

static void lo_open(lo_fix_t *f) {
    memset(f, 0, sizeof(*f));
    f->alloc = hu_system_allocator();
    hu_provider_t prov = {.ctx = &f->rec, .vtable = &k_rec_vtable};
    HU_ASSERT_EQ(hu_graph_open(&f->alloc, NULL, 0, &f->g), HU_OK);
    HU_ASSERT_EQ(hu_w7_facade_open(f->g, &f->alloc, &f->wf), HU_OK);
    HU_ASSERT_EQ(hu_agent_from_config(&f->agent, &f->alloc, prov, NULL, 0, NULL, NULL, NULL, NULL,
                                      PRIMARY, strlen(PRIMARY), "rec_local", 9, 0.7, ".", 1, 5, 50,
                                      false, 0, NULL, 0, NULL, 0, NULL),
                 HU_OK);
    f->agent.verifier_graph = f->g;
    f->agent.w7_facade = f->wf;
    f->agent.memory_session_id = "lo-session";
    f->agent.memory_session_id_len = 10;
    f->agent.active_channel = "imessage";
    f->agent.active_channel_len = 8;
}

static void lo_close(lo_fix_t *f) {
    hu_agent_deinit(&f->agent);
    hu_graph_close(f->g, &f->alloc);
}

static void lo_turn(lo_fix_t *f, const char *msg) {
    char *resp = NULL;
    size_t resp_len = 0;
    (void)hu_agent_turn(&f->agent, msg, strlen(msg), &resp, &resp_len);
    if (resp)
        f->alloc.free(f->alloc.ctx, resp, resp_len + 1);
}

/* A message the default router sends to the analytical-or-deeper tier. */
static const char k_analytical_msg[] =
    "Can you explain step by step why the quarterly budget analysis compares the two "
    "investment strategies differently, and evaluate the tradeoffs between the long term "
    "risks and benefits of each approach in detail?";

static void analytical_precondition_routes_to_cloud_by_default(void) {
    hu_model_router_config_t cfg = hu_model_router_default_config();
    hu_model_selection_t sel =
        hu_model_route(&cfg, k_analytical_msg, strlen(k_analytical_msg), NULL, 0, -1, 0);
    HU_ASSERT_TRUE(sel.tier >= HU_TIER_ANALYTICAL);
    HU_ASSERT_STR_EQ(sel.model, "gemini-3.1-pro-preview");
}

static void analytical_turn_stays_on_primary_when_live(void) {
    lo_clean();
    lo_fix_t f;
    lo_open(&f);
    lo_turn(&f, k_analytical_msg);
    HU_ASSERT_TRUE(rec_saw(&f.rec, "gemini-3.1-pro-preview")); /* OFF: the cloud route */
    lo_close(&f);

    hu_local_only_configure(HU_GATE_LIVE);
    lo_open(&f);
    lo_turn(&f, k_analytical_msg);
    HU_ASSERT_TRUE(f.rec.calls >= 1u);
    HU_ASSERT_FALSE(rec_saw(&f.rec, "gemini-3.1-pro-preview"));
    HU_ASSERT_STR_EQ(f.rec.models[0], PRIMARY);
    lo_close(&f);
    lo_clean();
}

static void s3_turn_does_not_switch_model_when_live(void) {
    lo_clean();
    static const char s3_msg[] = "Here is my private key for the server";
    lo_fix_t f;
    lo_open(&f);
    f.agent.sota.degradation_config.fallback_model = "cloud-fallback-x";
    f.agent.sota.degradation_config.fallback_model_len = 16;
    lo_turn(&f, s3_msg);
    HU_ASSERT_STR_EQ(f.rec.models[0], "cloud-fallback-x"); /* OFF: the S3 switch */
    lo_close(&f);

    hu_local_only_configure(HU_GATE_LIVE);
    lo_open(&f);
    f.agent.sota.degradation_config.fallback_model = "cloud-fallback-x";
    f.agent.sota.degradation_config.fallback_model_len = 16;
    lo_turn(&f, s3_msg);
    HU_ASSERT_TRUE(f.rec.calls >= 1u);
    HU_ASSERT_FALSE(rec_saw(&f.rec, "cloud-fallback-x"));
    HU_ASSERT_STR_EQ(f.rec.models[0], PRIMARY);
    lo_close(&f);
    lo_clean();
}

static void on_device_failure_retry_skipped_when_live(void) {
    lo_clean();
    hu_model_router_config_t d = hu_model_router_default_config();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.agent.mr_on_device_enabled = true;

    lo_fix_t f;
    lo_open(&f);
    f.agent.config = &cfg;
    f.agent.turn_model = d.on_device_model;
    f.agent.turn_model_len = d.on_device_model_len;
    f.rec.fail_model = d.on_device_model;
    lo_turn(&f, "hi");
    HU_ASSERT_TRUE(rec_saw(&f.rec, d.reflexive_model)); /* OFF: retried on the cloud model */
    f.agent.config = NULL;
    lo_close(&f);

    hu_local_only_configure(HU_GATE_LIVE);
    lo_open(&f);
    f.agent.config = &cfg;
    f.agent.turn_model = d.on_device_model;
    f.agent.turn_model_len = d.on_device_model_len;
    f.rec.fail_model = d.on_device_model;
    lo_turn(&f, "hi");
    HU_ASSERT_TRUE(rec_saw(&f.rec, d.on_device_model));
    HU_ASSERT_FALSE(rec_saw(&f.rec, d.reflexive_model));
    f.agent.config = NULL;
    lo_close(&f);
    lo_clean();
}
#endif

void run_local_only_route_tests(void) {
    HU_TEST_SUITE("local_only_route");
    HU_RUN_TEST(router_defaults_untouched_when_off);
    HU_RUN_TEST(router_defaults_become_primary_when_live);
    HU_RUN_TEST(guard_retry_cloud_fallback_blocked_when_live);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(analytical_precondition_routes_to_cloud_by_default);
    HU_RUN_TEST(analytical_turn_stays_on_primary_when_live);
    HU_RUN_TEST(s3_turn_does_not_switch_model_when_live);
    HU_RUN_TEST(on_device_failure_retry_skipped_when_live);
#endif
}
