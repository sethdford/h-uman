/* Local-only prompt spans (providers/local_only.h): the HU_THREAD_CONTEXT
 * "## Recent thread" block may reach the on-device model but never a cloud
 * one. Pins the strip primitive and, end to end through the reliable
 * provider, that a cloud fallback attempt (prod: mlx_local primary failing
 * over to the gemini extra) carries no thread block while the local primary
 * still gets it and the rest of the prompt is unchanged. Fakes only. */
#include "human/agent/model_router.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/provider.h"
#include "human/providers/factory.h"
#include "human/providers/local_only.h"
#include "human/providers/reliable.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

#define BLOCK                                                            \
    HU_LOCAL_ONLY_THREAD_BEGIN                                           \
    " (you and Mike, oldest first)\n[2d ago]\nMike: secret plans\nyou: " \
    "ok\n" HU_LOCAL_ONLY_THREAD_END

static const char k_prompt[] = "persona head\n" BLOCK "guard tail\n";
static const char k_prompt_stripped[] = "persona head\nguard tail\n";

/* A provider NAME alone is local only for an in-process backend; an HTTP
 * provider's locality comes from its endpoint (see the endpoint tests). */
static void test_local_only_names(void) {
    HU_ASSERT_TRUE(hu_local_only_provider_name_is_local("apple"));
    HU_ASSERT_TRUE(hu_local_only_provider_name_is_local("coreml"));
    HU_ASSERT_TRUE(hu_local_only_provider_name_is_local("llamacpp"));
    HU_ASSERT_FALSE(hu_local_only_provider_name_is_local("mlx_local"));
    HU_ASSERT_FALSE(hu_local_only_provider_name_is_local("ollama"));
    HU_ASSERT_FALSE(hu_local_only_provider_name_is_local("gemini"));
    HU_ASSERT_FALSE(hu_local_only_provider_name_is_local("compatible"));
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
    static const char cut[] = "persona head\n" HU_LOCAL_ONLY_THREAD_BEGIN " (x)\nMike: secret pl";
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

static void test_local_only_model_names(void) {
    HU_ASSERT_TRUE(hu_local_only_model_is_cloud("gemini-3.1-pro-preview", 22));
    HU_ASSERT_TRUE(hu_local_only_model_is_cloud("gemini-3.8-flash", 16));
    HU_ASSERT_TRUE(hu_local_only_model_is_cloud("publishers/google/models/gemini-3.8-flash", 41));
    HU_ASSERT_TRUE(hu_local_only_model_is_cloud("GPT-4o", 6));
    HU_ASSERT_TRUE(hu_local_only_model_is_cloud("claude-opus", 11));
    HU_ASSERT_FALSE(hu_local_only_model_is_cloud("GLM-4.5-Air-4bit", 16));
    HU_ASSERT_FALSE(hu_local_only_model_is_cloud("gemma-4-31b-it-4bit", 19));
    HU_ASSERT_FALSE(hu_local_only_model_is_cloud("apple-foundationmodel", 21));
    HU_ASSERT_FALSE(hu_local_only_model_is_cloud("", 0));
    HU_ASSERT_TRUE(hu_local_only_attempt_is_local(true, "GLM-4.5-Air-4bit", 16));
    HU_ASSERT_FALSE(hu_local_only_attempt_is_local(true, "gemini-3.8-flash", 16));
    HU_ASSERT_FALSE(hu_local_only_attempt_is_local(false, "GLM-4.5-Air-4bit", 16));
}

static void test_local_only_span_table_has_thread_kind(void) {
    size_t n = 0;
    const hu_local_only_span_kind_t *k = hu_local_only_span_kinds(&n);
    HU_ASSERT_GE(n, 1);
    HU_ASSERT_STR_EQ(k[0].begin, HU_LOCAL_ONLY_THREAD_BEGIN);
    HU_ASSERT_STR_EQ(k[0].end, HU_LOCAL_ONLY_THREAD_END);
}

/* Allocator that fails its Nth allocation (1-based); 0 = never. */
typedef struct fail_alloc {
    hu_allocator_t sys;
    int fail_at;
    int calls;
    long live;
} fail_alloc_t;

static void *fa_alloc(void *ctx, size_t n) {
    fail_alloc_t *f = (fail_alloc_t *)ctx;
    if (++f->calls == f->fail_at)
        return NULL;
    void *p = f->sys.alloc(f->sys.ctx, n);
    if (p)
        f->live++;
    return p;
}

static void *fa_realloc(void *ctx, void *ptr, size_t o, size_t n) {
    fail_alloc_t *f = (fail_alloc_t *)ctx;
    return f->sys.realloc(f->sys.ctx, ptr, o, n);
}

static void fa_free(void *ctx, void *ptr, size_t n) {
    fail_alloc_t *f = (fail_alloc_t *)ctx;
    if (ptr)
        f->live--;
    f->sys.free(f->sys.ctx, ptr, n);
}

/* Every allocation inside prepare can fail without freeing an uninitialised
 * pointer or leaking, and a failed prepare never hands back a request. */
static void test_local_only_prepare_oom_at_every_step(void) {
    hu_chat_message_t msgs[2];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = HU_ROLE_SYSTEM;
    msgs[0].content = k_prompt;
    msgs[0].content_len = sizeof(k_prompt) - 1;
    msgs[1].role = HU_ROLE_USER;
    msgs[1].content = k_prompt; /* two stripped messages: 5 allocations */
    msgs[1].content_len = sizeof(k_prompt) - 1;
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.messages = msgs;
    req.messages_count = 2;
    for (int k = 1; k <= 6; k++) {
        fail_alloc_t f = {.sys = hu_system_allocator(), .fail_at = k};
        hu_allocator_t a = {.ctx = &f, .alloc = fa_alloc, .realloc = fa_realloc, .free = fa_free};
        hu_local_only_request_t lo;
        const hu_chat_request_t *use = (const hu_chat_request_t *)&f;
        hu_error_t err = hu_local_only_request_prepare(&a, &req, &lo, &use);
        if (k <= 5) {
            HU_ASSERT_EQ(err, HU_ERR_OUT_OF_MEMORY);
            HU_ASSERT_NULL(use);
        } else {
            HU_ASSERT_EQ(err, HU_OK);
            HU_ASSERT_NOT_NULL(use);
            HU_ASSERT_EQ(lo.stripped, 2);
        }
        hu_local_only_request_release(&a, &lo);
        HU_ASSERT_EQ(f.live, 0);
    }
}

/* ── locality is keyed on the endpoint, not the provider name ─────────── */

static void test_local_only_url_loopback_and_socket(void) {
    HU_ASSERT_TRUE(hu_local_only_url_is_local("http://127.0.0.1:8741/v1"));
    HU_ASSERT_TRUE(hu_local_only_url_is_local("http://localhost:11434"));
    HU_ASSERT_TRUE(hu_local_only_url_is_local("http://[::1]:8080/v1"));
    HU_ASSERT_TRUE(hu_local_only_url_is_local("http://user@127.0.0.2:9000"));
    HU_ASSERT_TRUE(hu_local_only_url_is_local("unix:///tmp/mlx.sock"));
    HU_ASSERT_TRUE(hu_local_only_url_is_local("/var/run/llama.sock"));
    HU_ASSERT_FALSE(hu_local_only_url_is_local("https://gpu.example.com/v1"));
    HU_ASSERT_FALSE(hu_local_only_url_is_local("http://localhost.example.com:8741"));
    HU_ASSERT_FALSE(hu_local_only_url_is_local("http://127.0.0.1.example.com"));
    HU_ASSERT_FALSE(hu_local_only_url_is_local("http://evil.com/127.0.0.1"));
    HU_ASSERT_FALSE(hu_local_only_url_is_local("http://0.0.0.0:8741"));
    /* strict parser (#587): octets range-checked, scheme required at the start */
    HU_ASSERT_FALSE(hu_local_only_url_is_local("http://127.0.0.999:8741"));
    HU_ASSERT_FALSE(hu_local_only_url_is_local("http://127.0.0.256"));
    HU_ASSERT_TRUE(hu_local_only_url_is_local("http://127.255.255.254:1"));
    HU_ASSERT_FALSE(hu_local_only_url_is_local("evil.com/?u=http://127.0.0.1"));
    HU_ASSERT_FALSE(hu_local_only_url_is_local("localhost:8741"));
    HU_ASSERT_FALSE(hu_local_only_url_is_local("ftp://127.0.0.1"));
    HU_ASSERT_TRUE(hu_local_only_url_is_local("WS://localhost:9000/ws"));
    HU_ASSERT_TRUE(hu_local_only_url_is_local("https://127.0.0.1"));
    HU_ASSERT_FALSE(hu_local_only_url_is_local(""));
    HU_ASSERT_FALSE(hu_local_only_url_is_local(NULL));
}

static void test_local_only_endpoint_decision(void) {
    /* mlx_local pointed at a remote host is NOT local */
    HU_ASSERT_FALSE(hu_local_only_endpoint_is_local("mlx_local", "https://gpu.example.com/v1", 0));
    HU_ASSERT_TRUE(hu_local_only_endpoint_is_local("mlx_local", "http://127.0.0.1:8741/v1", 0));
    /* a gateway on loopback still forwards to the cloud */
    HU_ASSERT_FALSE(hu_local_only_endpoint_is_local("litellm", "http://localhost:4000", 0));
    HU_ASSERT_FALSE(hu_local_only_endpoint_is_local("openrouter", "http://127.0.0.1:9", 0));
    /* in-process backends have no URL */
    HU_ASSERT_TRUE(hu_local_only_endpoint_is_local("apple", NULL, 0));
    /* path backends: base_url is a model path, never a host */
    HU_ASSERT_TRUE(hu_local_only_endpoint_is_local("embedded", "models/x.gguf", 0));
    HU_ASSERT_TRUE(hu_local_only_endpoint_is_local("llamacpp", "/opt/models/q4.gguf", 0));
    HU_ASSERT_TRUE(hu_local_only_endpoint_is_local("huml", "ckpt/huml.bin", 0));
    HU_ASSERT_FALSE(hu_local_only_endpoint_is_local("mlx_local", "models/x.gguf", 0));
    HU_ASSERT_FALSE(hu_local_only_endpoint_is_local("gemini", NULL, 0));
    HU_ASSERT_FALSE(hu_local_only_endpoint_is_local("compatible", NULL, 0));
    /* the explicit per-provider override wins both ways */
    HU_ASSERT_TRUE(hu_local_only_endpoint_is_local("mlx_local", "https://box.lan.example/v1", 1));
    HU_ASSERT_FALSE(hu_local_only_endpoint_is_local("mlx_local", "http://127.0.0.1:8741", -1));
}

/* Ollama on loopback serving a cloud model ("…-cloud" / ":cloud") forwards
 * the prompt to ollama.com: the model-name veto makes the attempt non-local. */
static void test_local_only_ollama_cloud_model_not_local(void) {
    bool ep = hu_local_only_endpoint_is_local("ollama", "http://localhost:11434", 0);
    HU_ASSERT_TRUE(ep);
    HU_ASSERT_FALSE(hu_local_only_attempt_is_local(ep, "gpt-oss:120b-cloud", 18));
    HU_ASSERT_FALSE(hu_local_only_attempt_is_local(ep, "qwen3-coder:cloud", 17));
    HU_ASSERT_FALSE(hu_local_only_attempt_is_local(ep, "kimi-k2-cloud", 13));
    HU_ASSERT_TRUE(hu_local_only_attempt_is_local(ep, "llama3.1:8b", 11));
    HU_ASSERT_TRUE(hu_local_only_model_is_cloud("o3", 2));
    HU_ASSERT_TRUE(hu_local_only_model_is_cloud("o4-mini", 7));
    HU_ASSERT_TRUE(hu_local_only_model_is_cloud("o1", 2));
    HU_ASSERT_FALSE(hu_local_only_model_is_cloud("olmo-2-7b", 9));
}

/* from_config wiring: the reliable primary's locality comes from the
 * configured base_url (+ override), not the "mlx_local" name. */
static bool reliable_primary_local_for(const char *providers_json) {
    char json[512];
    snprintf(json, sizeof(json),
             "{\"default_provider\":\"reliable\",\"reliability\":{\"primary_provider\":"
             "\"mlx_local\"},\"providers\":[%s]}",
             providers_json);
    hu_allocator_t a = hu_system_allocator();
    hu_config_t c;
    HU_ASSERT_EQ(hu_config_load(&a, &c), HU_OK);
    HU_ASSERT_EQ(hu_config_parse_json(&c, json, strlen(json)), HU_OK);
    hu_provider_t p;
    memset(&p, 0, sizeof(p));
    HU_ASSERT_EQ(hu_provider_create_from_config(&a, &c, "reliable", 8, &p), HU_OK);
    bool local = hu_reliable_primary_is_local(&p);
    if (p.vtable && p.vtable->deinit)
        p.vtable->deinit(p.ctx, &a);
    hu_config_deinit(&c);
    return local;
}

static void test_from_config_primary_locality_follows_base_url(void) {
    HU_ASSERT_TRUE(reliable_primary_local_for(
        "{\"name\":\"mlx_local\",\"base_url\":\"http://127.0.0.1:8741/v1\"}"));
    HU_ASSERT_FALSE(reliable_primary_local_for(
        "{\"name\":\"mlx_local\",\"base_url\":\"https://gpu.example.com/v1\"}"));
    HU_ASSERT_TRUE(reliable_primary_local_for(
        "{\"name\":\"mlx_local\",\"base_url\":\"https://gpu.example.com/v1\",\"local\":true}"));
    HU_ASSERT_FALSE(reliable_primary_local_for(
        "{\"name\":\"mlx_local\",\"base_url\":\"http://127.0.0.1:8741/v1\",\"local\":false}"));
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

static hu_error_t cap_stream_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                                  const char *model, size_t model_len, double temperature,
                                  hu_stream_callback_t cb, void *cb_ctx,
                                  hu_stream_chat_result_t *out) {
    (void)cb;
    (void)cb_ctx;
    (void)out;
    hu_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));
    hu_error_t err = cap_chat(ctx, alloc, req, model, model_len, temperature, &resp);
    hu_chat_response_free(alloc, &resp);
    return err;
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
    .stream_chat = cap_stream_chat,
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

static hu_error_t lo_rig_chat_model(lo_rig_t *g, const char *model, hu_chat_response_t *resp) {
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
    return g->reliable.vtable->chat(g->reliable.ctx, &g->alloc, &req, model, strlen(model), 0.7,
                                    resp);
}

static hu_error_t lo_rig_chat(lo_rig_t *g, hu_chat_response_t *resp) {
    return lo_rig_chat_model(g, "GLM-4.5-Air-4bit", resp);
}

/* agent_turn.c switches the model BY NAME on the same (reliable) provider.
 * The first attempt then goes to the local primary carrying a cloud model
 * name; it must not carry the thread block either. */
static void route_first_attempt_strips(const char *model) {
    lo_rig_t g;
    lo_rig_init(&g, HU_OK, true);
    hu_chat_response_t resp;
    HU_ASSERT_EQ(lo_rig_chat_model(&g, model, &resp), HU_OK);
    HU_ASSERT_EQ(g.prim.calls, 1);
    HU_ASSERT_STR_EQ(g.prim.seen_system, k_prompt_stripped);
    HU_ASSERT_FALSE(g.prim.saw_cache_id);
    hu_chat_response_free(&g.alloc, &resp);
    g.reliable.vtable->deinit(g.reliable.ctx, &g.alloc);
}

/* agent_turn.c:4156 — analytical tier -> the router's analytical model. */
static void test_route_analytical_tier_model_strips(void) {
    hu_model_router_config_t cfg = hu_model_router_default_config();
    HU_ASSERT_NOT_NULL(cfg.analytical_model);
    route_first_attempt_strips(cfg.analytical_model);
}

/* agent_turn.c:4220 — S3 sensitivity -> degradation fallback_model (prod:
 * gemini-3.8-flash, the configured reliability.model_fallbacks target). */
static void test_route_s3_fallback_model_strips(void) {
    route_first_attempt_strips("gemini-3.8-flash");
}

/* agent_turn.c:4384 — on-device failure -> the router's reflexive model. */
static void test_route_on_device_failure_reflexive_model_strips(void) {
    hu_model_router_config_t cfg = hu_model_router_default_config();
    HU_ASSERT_NOT_NULL(cfg.reflexive_model);
    route_first_attempt_strips(cfg.reflexive_model);
}

/* The serving model on the local primary keeps the block. */
static void test_route_local_model_keeps_block(void) {
    lo_rig_t g;
    lo_rig_init(&g, HU_OK, true);
    hu_chat_response_t resp;
    HU_ASSERT_EQ(lo_rig_chat(&g, &resp), HU_OK);
    HU_ASSERT_STR_EQ(g.prim.seen_system, k_prompt);
    HU_ASSERT_TRUE(g.prim.saw_cache_id);
    hu_chat_response_free(&g.alloc, &resp);
    g.reliable.vtable->deinit(g.reliable.ctx, &g.alloc);
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
    HU_ASSERT_STR_NOT_CONTAINS(g.cloud.seen_system, HU_LOCAL_ONLY_THREAD_BEGIN);
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

/* Streaming: the primary's stream fails, the cloud extra streams — and gets
 * no thread block. */
static void test_reliable_stream_fallback_strips(void) {
    lo_rig_t g;
    lo_rig_init(&g, HU_ERR_PROVIDER_RESPONSE, true);
    hu_chat_message_t msgs[1];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = HU_ROLE_SYSTEM;
    msgs[0].content = k_prompt;
    msgs[0].content_len = sizeof(k_prompt) - 1;
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.messages = msgs;
    req.messages_count = 1;
    hu_stream_chat_result_t out;
    memset(&out, 0, sizeof(out));
    HU_ASSERT_EQ(g.reliable.vtable->stream_chat(g.reliable.ctx, &g.alloc, &req, "GLM-4.5-Air-4bit",
                                                16, 0.7, NULL, NULL, &out),
                 HU_OK);
    HU_ASSERT_STR_CONTAINS(g.prim.seen_system, "secret plans");
    HU_ASSERT_EQ(g.cloud.calls, 1);
    HU_ASSERT_STR_EQ(g.cloud.seen_system, k_prompt_stripped);
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
    HU_RUN_TEST(test_local_only_model_names);
    HU_RUN_TEST(test_local_only_span_table_has_thread_kind);
    HU_RUN_TEST(test_local_only_prepare_oom_at_every_step);
    HU_RUN_TEST(test_local_only_url_loopback_and_socket);
    HU_RUN_TEST(test_local_only_endpoint_decision);
    HU_RUN_TEST(test_local_only_ollama_cloud_model_not_local);
    HU_RUN_TEST(test_from_config_primary_locality_follows_base_url);
    HU_RUN_TEST(test_reliable_stream_fallback_strips);
    HU_RUN_TEST(test_route_analytical_tier_model_strips);
    HU_RUN_TEST(test_route_s3_fallback_model_strips);
    HU_RUN_TEST(test_route_on_device_failure_reflexive_model_strips);
    HU_RUN_TEST(test_route_local_model_keeps_block);
}
