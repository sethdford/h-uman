/* tests/test_daemon_director_local_only.c
 *
 * local_only routing in the daemon (owner decision 2026-10-01):
 *   (a) the director / emotion / double-text classify provider. OFF keeps the
 *       hybrid route (a Gemini provider is created); LIVE never creates one —
 *       it borrows the agent's own provider and model when the primary
 *       endpoint is local, and leaves classification off when it is not.
 *   (c) images. LIVE sends no image bytes anywhere (the local server is
 *       text-only and the cloud is off-limits): describe_image refuses before
 *       any provider call, and a bare "[Photo]" becomes a placeholder the
 *       model can react to.
 * Hermetic: fake provider vtables, the HU_IS_TEST HTTP mock, no files.
 */

#include "human/agent.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/local_only_guard.h"
#include "human/daemon/common.h"
#include "human/daemon/director.h"
#include "human/daemon/message_router.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

#define LOCAL_PRIMARY                                                                          \
    "{\"default_provider\":\"reliable\",\"default_model\":\"GLM-4.5-Air-4bit\","               \
    "\"providers\":[{\"name\":\"gemini\",\"base_url\":\"https://aiplatform.googleapis.com/v1/" \
    "projects/p/locations/global/publishers/google/models\"},"                                 \
    "{\"name\":\"mlx_local\",\"base_url\":\"http://127.0.0.1:8741/v1\"}],"                     \
    "\"reliability\":{\"primary_provider\":\"mlx_local\",\"fallback_providers\":[]}}"

#define CLOUD_PRIMARY \
    "{\"default_provider\":\"gemini\",\"default_model\":\"gemini-3.1-flash-lite\"}"

typedef struct fake_prov {
    unsigned chat_calls;
} fake_prov_t;

static hu_error_t fake_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                            const char *model, size_t model_len, double temperature,
                            hu_chat_response_t *out) {
    (void)req;
    (void)model;
    (void)model_len;
    (void)temperature;
    ((fake_prov_t *)ctx)->chat_calls++;
    memset(out, 0, sizeof(*out));
    const char *body = "a dog on a beach";
    size_t n = strlen(body);
    char *buf = (char *)alloc->alloc(alloc->ctx, n + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, body, n + 1);
    out->content = buf;
    out->content_len = n;
    return HU_OK;
}

static hu_error_t fake_chat_sys(void *ctx, hu_allocator_t *alloc, const char *sp, size_t sl,
                                const char *m, size_t ml, const char *model, size_t model_len,
                                double t, char **out, size_t *out_len) {
    (void)alloc;
    (void)sp;
    (void)sl;
    (void)m;
    (void)ml;
    (void)model;
    (void)model_len;
    (void)t;
    ((fake_prov_t *)ctx)->chat_calls++;
    *out = NULL;
    *out_len = 0;
    return HU_ERR_NOT_SUPPORTED;
}

static bool fake_vision(void *ctx) {
    (void)ctx;
    return true;
}

static const char *fake_name(void *ctx) {
    (void)ctx;
    return "fake_local";
}

static const hu_provider_vtable_t k_fake_vtable = {
    .chat_with_system = fake_chat_sys,
    .chat = fake_chat,
    .get_name = fake_name,
    .supports_vision = fake_vision,
};

static hu_config_t *mk_cfg(const char *json) {
    hu_allocator_t backing = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(backing);
    hu_config_t *cfg = (hu_config_t *)backing.alloc(backing.ctx, sizeof(hu_config_t));
    HU_ASSERT_NOT_NULL(cfg);
    memset(cfg, 0, sizeof(*cfg));
    cfg->arena = arena;
    cfg->allocator = hu_arena_allocator(arena);
    HU_ASSERT_EQ(hu_config_parse_json(cfg, json, strlen(json)), HU_OK);
    return cfg;
}

static void free_cfg(hu_config_t *cfg) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_deinit(cfg);
    backing.free(backing.ctx, cfg, sizeof(*cfg));
}

static hu_agent_t *mk_agent(fake_prov_t *fp) {
    hu_agent_t *a = (hu_agent_t *)calloc(1, sizeof(hu_agent_t));
    HU_ASSERT_NOT_NULL(a);
    a->provider.ctx = fp;
    a->provider.vtable = &k_fake_vtable;
    a->model_name = "GLM-4.5-Air-4bit";
    a->model_name_len = 16;
    return a;
}

/* Restore the director globals exactly as the daemon starts them. */
static void reset_classify(hu_allocator_t *alloc) {
    if (g_classify_provider_ok && g_classify_provider.vtable != &k_fake_vtable &&
        g_classify_provider.vtable && g_classify_provider.vtable->deinit)
        g_classify_provider.vtable->deinit(g_classify_provider.ctx, alloc);
    memset(&g_classify_provider, 0, sizeof(g_classify_provider));
    g_classify_provider_ok = false;
    g_classify_model = "gemini-3.1-flash-lite";
    g_classify_model_len = 21;
    unsetenv("HU_LOCAL_ONLY");
    hu_local_only_reset();
}

/* OFF: today's hybrid route — a separate (cloud) classify provider. */
static void classify_off_creates_separate_cloud_provider(void) {
    hu_allocator_t alloc = hu_system_allocator();
    reset_classify(&alloc);
    fake_prov_t fp = {0};
    hu_agent_t *agent = mk_agent(&fp);
    hu_config_t *cfg = mk_cfg(LOCAL_PRIMARY);
    hu_daemon_classify_provider_init(&alloc, cfg, agent, true);
    HU_ASSERT_TRUE(g_classify_provider_ok);
    HU_ASSERT_TRUE(g_classify_provider.vtable != &k_fake_vtable);
    HU_ASSERT_STR_EQ(g_classify_model, "gemini-3.1-flash-lite");
    reset_classify(&alloc);
    free_cfg(cfg);
    free(agent);
}

/* LIVE + local primary: the director runs on the agent's own provider and
 * model; no Gemini provider exists. */
static void classify_live_borrows_local_primary(void) {
    hu_allocator_t alloc = hu_system_allocator();
    reset_classify(&alloc);
    hu_local_only_configure(HU_GATE_LIVE);
    fake_prov_t fp = {0};
    hu_agent_t *agent = mk_agent(&fp);
    hu_config_t *cfg = mk_cfg(LOCAL_PRIMARY);
    hu_daemon_classify_provider_init(&alloc, cfg, agent, true);
    HU_ASSERT_TRUE(g_classify_provider_ok);
    HU_ASSERT_TRUE(g_classify_provider.vtable == &k_fake_vtable);
    HU_ASSERT_TRUE(g_classify_provider.ctx == (void *)&fp);
    HU_ASSERT_EQ(g_classify_model_len, (size_t)16);
    HU_ASSERT_STR_EQ(g_classify_model, "GLM-4.5-Air-4bit");
    reset_classify(&alloc);
    free_cfg(cfg);
    free(agent);
}

/* LIVE + cloud primary: nothing local to borrow, so classification is off
 * (the director already handles !g_classify_provider_ok by skipping). */
static void classify_live_without_local_primary_is_skipped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    reset_classify(&alloc);
    hu_local_only_configure(HU_GATE_LIVE);
    fake_prov_t fp = {0};
    hu_agent_t *agent = mk_agent(&fp);
    hu_config_t *cfg = mk_cfg(CLOUD_PRIMARY);
    hu_daemon_classify_provider_init(&alloc, cfg, agent, true);
    HU_ASSERT_FALSE(g_classify_provider_ok);
    HU_ASSERT_NULL(g_classify_provider.vtable);
    reset_classify(&alloc);
    free_cfg(cfg);
    free(agent);
}

/* Audit routes as OFF (only the HTTP backstop logs). */
static void classify_audit_routes_like_off(void) {
    hu_allocator_t alloc = hu_system_allocator();
    reset_classify(&alloc);
    hu_local_only_configure(HU_GATE_SHADOW);
    fake_prov_t fp = {0};
    hu_agent_t *agent = mk_agent(&fp);
    hu_config_t *cfg = mk_cfg(LOCAL_PRIMARY);
    hu_daemon_classify_provider_init(&alloc, cfg, agent, true);
    HU_ASSERT_TRUE(g_classify_provider_ok);
    HU_ASSERT_TRUE(g_classify_provider.vtable != &k_fake_vtable);
    reset_classify(&alloc);
    free_cfg(cfg);
    free(agent);
}

/* (c) OFF: with no cloud vision route declared, the image goes to the agent's
 * own provider (one chat call). LIVE: no provider is called at all. */
static void describe_image_live_sends_no_bytes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    reset_classify(&alloc);
    hu_config_t *cfg = mk_cfg(LOCAL_PRIMARY);
    const char *path = "/tmp/never-read.jpg"; /* HU_IS_TEST mocks the read */

    fake_prov_t fp = {0};
    hu_agent_t *agent = mk_agent(&fp);
    char *desc = NULL;
    size_t desc_len = 0;
    HU_ASSERT_EQ((int)hu_daemon_describe_image(&alloc, agent, cfg, path, strlen(path),
                                               agent->model_name, agent->model_name_len, &desc,
                                               &desc_len),
                 (int)HU_OK);
    HU_ASSERT_EQ(fp.chat_calls, 1u);
    alloc.free(alloc.ctx, desc, desc_len + 1);

    hu_local_only_configure(HU_GATE_LIVE);
    desc = NULL;
    desc_len = 0;
    HU_ASSERT_TRUE(hu_daemon_describe_image(&alloc, agent, cfg, path, strlen(path),
                                            agent->model_name, agent->model_name_len, &desc,
                                            &desc_len) != HU_OK);
    HU_ASSERT_NULL(desc);
    HU_ASSERT_EQ(fp.chat_calls, 1u); /* unchanged: nothing was sent */
    reset_classify(&alloc);
    free_cfg(cfg);
    free(agent);
}

/* LIVE: a declared cloud vision route (fallback provider + substitute model)
 * is not taken either. */
static void describe_image_live_ignores_cloud_vision_route(void) {
    hu_allocator_t alloc = hu_system_allocator();
    reset_classify(&alloc);
    hu_config_t *cfg = mk_cfg(
        "{\"default_provider\":\"reliable\",\"default_model\":\"GLM-4.5-Air-4bit\","
        "\"providers\":[{\"name\":\"mlx_local\",\"base_url\":\"http://127.0.0.1:8741/v1\"}],"
        "\"reliability\":{\"primary_provider\":\"mlx_local\",\"fallback_providers\":[\"gemini\"],"
        "\"model_fallbacks\":[{\"model\":\"GLM-4.5-Air-4bit\",\"fallbacks\":[\"gemini-3.5-flash\"]}"
        "]"
        "}}");
    const char *vp = NULL, *vm = NULL;
    HU_ASSERT_TRUE(
        hu_daemon_vision_route(cfg, "GLM-4.5-Air-4bit", 16, &vp, &vm)); /* precondition */
    hu_local_only_configure(HU_GATE_LIVE);
    fake_prov_t fp = {0};
    hu_agent_t *agent = mk_agent(&fp);
    char *desc = NULL;
    size_t desc_len = 0;
    HU_ASSERT_TRUE(hu_daemon_describe_image(&alloc, agent, cfg, "/tmp/x.jpg", 10, agent->model_name,
                                            agent->model_name_len, &desc, &desc_len) != HU_OK);
    HU_ASSERT_NULL(desc);
    HU_ASSERT_EQ(fp.chat_calls, 0u);
    HU_ASSERT_EQ(hu_local_only_refused_count(), 0u); /* never even reached the backstop */
    reset_classify(&alloc);
    free_cfg(cfg);
    free(agent);
}

static void photo_placeholder_bare_and_captioned(void) {
    char buf[128];
    size_t len = 7;
    const char *out = hu_daemon_photo_placeholder("[Photo]", &len, buf, sizeof(buf));
    HU_ASSERT_STR_EQ(out, "[They sent a photo]");
    HU_ASSERT_EQ(len, strlen("[They sent a photo]"));
    /* A captioned photo keeps its caption and gets the placeholder too; the
     * U+FFFC attachment character is dropped (no second "didn't load" note). */
    const char cap[] = "look at this \xEF\xBF\xBC";
    len = sizeof(cap) - 1;
    out = hu_daemon_photo_placeholder(cap, &len, buf, sizeof(buf));
    HU_ASSERT_STR_EQ(out, "look at this\n[They sent a photo]");
    HU_ASSERT_EQ(len, strlen("look at this\n[They sent a photo]"));
    const char plain[] = "look at this";
    len = sizeof(plain) - 1;
    out = hu_daemon_photo_placeholder(plain, &len, buf, sizeof(buf));
    HU_ASSERT_STR_EQ(out, "look at this\n[They sent a photo]");
}

void run_daemon_director_local_only_tests(void) {
    HU_TEST_SUITE("daemon_director_local_only");
    HU_RUN_TEST(classify_off_creates_separate_cloud_provider);
    HU_RUN_TEST(classify_live_borrows_local_primary);
    HU_RUN_TEST(classify_live_without_local_primary_is_skipped);
    HU_RUN_TEST(classify_audit_routes_like_off);
    HU_RUN_TEST(describe_image_live_sends_no_bytes);
    HU_RUN_TEST(describe_image_live_ignores_cloud_vision_route);
    HU_RUN_TEST(photo_placeholder_bare_and_captioned);
}
