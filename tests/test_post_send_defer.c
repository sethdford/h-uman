/* tests/test_post_send_defer.c — HU_POST_SEND_DEFER=off|shadow|live.
 *
 * The LLM fact-extraction fallback (~1.4 s) and the per-row semantic-index
 * embeddings run inside the reply turn before the send. With the gate LIVE the
 * daemon arms a window before the turn (hu_post_send_defer_begin) and flushes it
 * after the send (hu_post_send_defer_flush): the same work runs, exactly once,
 * after the send. OFF must be byte-identical to today.
 *
 * Why it is gated, not a pure timing change: the extracted facts merge into the
 * personal model BEFORE the prompt is built, and the "Key facts:" / "Avoid:"
 * lines read them on the same turn. Deferring moves them to the next turn's
 * prompt. SHADOW measures how many facts that is, and how many the inbound
 * already states verbatim.
 *
 * Production symbols: hu_post_send_defer_*, hu_personal_model_ingest,
 * hu_sqlite_memory_set_semantic_index + memory store. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/llm_purpose.h"
#include "human/core/post_send_defer.h"
#include "human/memory/fact_extract.h"
#include "human/memory/personal_model.h"
#include "human/provider.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

/* ── recording extractor (same shape as test_personal_model_llm_extract.c) ── */

typedef struct psd_rec {
    const char *canned;
    int calls;
} psd_rec_t;

static hu_error_t psd_chat_with_system(void *ctx, hu_allocator_t *alloc, const char *sys,
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
    psd_rec_t *r = (psd_rec_t *)ctx;
    r->calls++;
    size_t n = strlen(r->canned);
    *out = (char *)alloc->alloc(alloc->ctx, n + 1);
    memcpy(*out, r->canned, n + 1);
    *out_len = n;
    return HU_OK;
}

static const hu_provider_vtable_t psd_vtable = {.chat_with_system = psd_chat_with_system};

static const char *psd_canned = "{\"facts\":[{\"subject\":\"user\",\"predicate\":\"asked_about\","
                                "\"object\":\"the email\",\"confidence\":0.8}]}";
static const char *psd_msg = "Did you not get the email I sent yesterday?"; /* regex-empty */
#define PSD_TS 1700000000LL

static hu_allocator_t s_alloc;
static psd_rec_t s_rec;
static hu_provider_t s_prov;

static void psd_setup(const char *defer_mode) {
    s_alloc = hu_system_allocator();
    s_rec = (psd_rec_t){.canned = psd_canned, .calls = 0};
    s_prov = (hu_provider_t){.ctx = &s_rec, .vtable = &psd_vtable};
    setenv("HU_LLM_FACT_EXTRACT", "on", 1);
    if (defer_mode)
        setenv("HU_POST_SEND_DEFER", defer_mode, 1);
    else
        unsetenv("HU_POST_SEND_DEFER");
    hu_personal_model_set_llm_extractor(&s_alloc, &s_prov, "m", 1);
}

static void psd_teardown(void) {
    (void)hu_post_send_defer_flush();
    unsetenv("HU_POST_SEND_DEFER");
    unsetenv("HU_LLM_FACT_EXTRACT");
    hu_personal_model_set_llm_extractor(NULL, NULL, NULL, 0);
}

static bool psd_has(const hu_personal_model_t *m, const char *obj) {
    for (size_t i = 0; i < m->fact_count; i++)
        if (strcmp(m->facts[i].object, obj) == 0)
            return true;
    return false;
}

static void psd_ingest(hu_personal_model_t *m) {
    HU_ASSERT_EQ(hu_personal_model_ingest(m, psd_msg, strlen(psd_msg), true, PSD_TS, NULL), HU_OK);
}

/* ── the gate ──────────────────────────────────────────────────────────── */

static void off_gate_is_byte_identical_to_no_window(void) {
    static hu_personal_model_t plain, windowed;
    memset(&plain, 0, sizeof(plain));
    memset(&windowed, 0, sizeof(windowed));

    psd_setup(NULL);
    psd_ingest(&plain); /* today: no window at all */
    HU_ASSERT_EQ(s_rec.calls, 1);

    hu_post_send_defer_begin(); /* gate unset == OFF */
    HU_ASSERT_FALSE(hu_post_send_defer_armed());
    psd_ingest(&windowed);
    HU_ASSERT_EQ(s_rec.calls, 2); /* ran inline, before the "send" */
    HU_ASSERT_TRUE(psd_has(&windowed, "the email"));
    HU_ASSERT_EQ(hu_post_send_defer_flush(), 0u);
    HU_ASSERT_EQ(memcmp(&plain, &windowed, sizeof(plain)), 0);
    psd_teardown();
}

/* THE contract: LIVE runs the extraction once per turn, after the send. */
static void live_runs_extraction_once_after_flush(void) {
    static hu_personal_model_t m;
    memset(&m, 0, sizeof(m));
    psd_setup("live");
    hu_post_send_defer_begin();
    HU_ASSERT_TRUE(hu_post_send_defer_armed());
    psd_ingest(&m);
    HU_ASSERT_EQ(s_rec.calls, 0);                 /* not on the reply path */
    HU_ASSERT_FALSE(psd_has(&m, "the email"));    /* nothing merged yet */
    HU_ASSERT_EQ(hu_post_send_defer_flush(), 1u); /* "after the send" */
    HU_ASSERT_EQ(s_rec.calls, 1);
    HU_ASSERT_TRUE(psd_has(&m, "the email"));
    HU_ASSERT_EQ(hu_post_send_defer_flush(), 0u); /* never twice */
    HU_ASSERT_EQ(s_rec.calls, 1);
    hu_post_send_defer_stats_t st = hu_post_send_defer_last_stats();
    HU_ASSERT_EQ(st.extract, 1u);
    HU_ASSERT_EQ(st.facts, 1u);
    HU_ASSERT_EQ(st.facts_literal, 1u); /* "the email" is in the inbound */
    psd_teardown();
}

/* Only the timing moves: the model after a deferred flush equals the model
 * the inline path produces, byte for byte. */
static void live_final_model_equals_inline_model(void) {
    static hu_personal_model_t inline_m, deferred_m;
    memset(&inline_m, 0, sizeof(inline_m));
    memset(&deferred_m, 0, sizeof(deferred_m));
    psd_setup(NULL);
    psd_ingest(&inline_m);
    psd_teardown();

    psd_setup("live");
    hu_post_send_defer_begin();
    psd_ingest(&deferred_m);
    /* the reply's own ingest (from_user=false) moves updated_at forward */
    HU_ASSERT_EQ(hu_personal_model_ingest(&deferred_m, "ok", 2, false, PSD_TS + 9, NULL), HU_OK);
    (void)hu_post_send_defer_flush();
    HU_ASSERT_EQ(deferred_m.updated_at, PSD_TS + 9); /* restored after the merge */
    deferred_m.updated_at = PSD_TS;
    deferred_m.interaction_count--;
    HU_ASSERT_EQ(memcmp(&inline_m, &deferred_m, sizeof(inline_m)), 0);
    psd_teardown();
}

static void shadow_keeps_extraction_inline_and_counts_it(void) {
    static hu_personal_model_t m;
    memset(&m, 0, sizeof(m));
    psd_setup("shadow");
    hu_post_send_defer_begin();
    psd_ingest(&m);
    HU_ASSERT_EQ(s_rec.calls, 1);             /* inline: shadow changes nothing */
    HU_ASSERT_TRUE(psd_has(&m, "the email")); /* in the model before the prompt */
    HU_ASSERT_EQ(hu_post_send_defer_flush(), 0u);
    hu_post_send_defer_stats_t st = hu_post_send_defer_last_stats();
    HU_ASSERT_EQ((int)st.mode, (int)HU_GATE_SHADOW);
    HU_ASSERT_EQ(st.extract, 1u); /* would have deferred one extraction */
    HU_ASSERT_EQ(st.facts, 1u);   /* that added one fact before the prompt */
    HU_ASSERT_EQ(st.facts_literal, 1u);
    psd_teardown();
}

/* SHADOW only counts: it never copies the message for a job it will not
 * queue (#604 review). Same ingest under OFF and SHADOW: identical bytes. */
static size_t psd_extract_bytes_under(const char *mode) {
    static hu_personal_model_t m;
    memset(&m, 0, sizeof(m));
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    psd_setup(mode);
    s_alloc = hu_tracking_allocator_allocator(ta);
    hu_personal_model_set_llm_extractor(&s_alloc, &s_prov, "m", 1);
    hu_post_send_defer_begin();
    psd_ingest(&m);
    HU_ASSERT_EQ(s_rec.calls, 1); /* inline either way */
    (void)hu_post_send_defer_flush();
    size_t bytes = hu_tracking_allocator_total_allocated(ta);
    psd_teardown();
    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0u);
    hu_tracking_allocator_destroy(ta);
    return bytes;
}

static void shadow_extract_allocates_nothing_extra(void) {
    size_t off = psd_extract_bytes_under(NULL);
    size_t shadow = psd_extract_bytes_under("shadow");
    HU_ASSERT_TRUE(off > 0); /* the inline extraction does allocate */
    HU_ASSERT_EQ(shadow, off);
    HU_ASSERT_EQ(hu_post_send_defer_last_stats().extract, 1u); /* still counted */
}

static void live_outside_window_runs_inline(void) {
    static hu_personal_model_t m;
    memset(&m, 0, sizeof(m));
    psd_setup("live");
    psd_ingest(&m); /* no begin(): CLI, gateway, tests — never deferred */
    HU_ASSERT_EQ(s_rec.calls, 1);
    HU_ASSERT_TRUE(psd_has(&m, "the email"));
    psd_teardown();
}

static void begin_runs_jobs_a_skipped_flush_left(void) {
    static hu_personal_model_t m;
    memset(&m, 0, sizeof(m));
    psd_setup("live");
    hu_post_send_defer_begin();
    psd_ingest(&m);
    HU_ASSERT_EQ(s_rec.calls, 0);
    hu_post_send_defer_begin(); /* next turn starts without a flush */
    HU_ASSERT_EQ(s_rec.calls, 1);
    HU_ASSERT_TRUE(psd_has(&m, "the email"));
    psd_teardown();
}

/* ── queue mechanics ───────────────────────────────────────────────────── */

static int s_runs;
static bool s_ran_in_lane;
static void psd_count_job(void *arg) {
    (void)arg;
    s_runs++;
    s_ran_in_lane = hu_llm_background_active();
}

static void full_queue_refuses_and_flush_runs_in_background_lane(void) {
    setenv("HU_POST_SEND_DEFER", "live", 1);
    s_runs = 0;
    s_ran_in_lane = false;
    hu_post_send_defer_begin();
    for (unsigned i = 0; i < HU_POST_SEND_DEFER_MAX_JOBS; i++)
        HU_ASSERT_TRUE(hu_post_send_defer_offer(HU_POST_SEND_JOB_EMBED, psd_count_job, NULL, NULL));
    HU_ASSERT_FALSE(hu_post_send_defer_offer(HU_POST_SEND_JOB_EMBED, psd_count_job, NULL, NULL));
    HU_ASSERT_EQ(s_runs, 0);
    HU_ASSERT_EQ(hu_post_send_defer_flush(), (size_t)HU_POST_SEND_DEFER_MAX_JOBS);
    HU_ASSERT_EQ(s_runs, (int)HU_POST_SEND_DEFER_MAX_JOBS);
    HU_ASSERT_TRUE(s_ran_in_lane);               /* deferred work goes out as batch */
    HU_ASSERT_FALSE(hu_llm_background_active()); /* lane closed after */
    HU_ASSERT_EQ(hu_post_send_defer_last_stats().inline_full, 1u);
    unsetenv("HU_POST_SEND_DEFER");
}

/* ── the semantic-index embedding of stored rows ───────────────────────── */
#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/vector.h"
#include "human/memory/vector/store_sqlite_vec.h"

static int s_embeds;
static hu_error_t psd_embed(void *ctx, hu_allocator_t *alloc, const char *text, size_t len,
                            hu_embedding_t *out) {
    (void)ctx;
    (void)text;
    (void)len;
    s_embeds++;
    float *v = (float *)alloc->alloc(alloc->ctx, 3 * sizeof(float));
    if (!v)
        return HU_ERR_OUT_OF_MEMORY;
    v[0] = 1.0f;
    v[1] = 0.0f;
    v[2] = 0.0f;
    out->values = v;
    out->dim = 3;
    return HU_OK;
}
static size_t psd_dims(void *ctx) {
    (void)ctx;
    return 3;
}
static const hu_embedder_vtable_t psd_emb_vt = {.embed = psd_embed, .dimensions = psd_dims};

static void live_defers_row_embedding_until_flush(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    hu_embedder_t emb = {.ctx = NULL, .vtable = &psd_emb_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);
    s_embeds = 0;

    setenv("HU_POST_SEND_DEFER", "live", 1);
    hu_post_send_defer_begin();
    HU_ASSERT_EQ(mem.vtable->store(mem.ctx, "user:likes:tea", 14, "tea", 3, NULL, "", 0), HU_OK);
    HU_ASSERT_EQ(s_embeds, 0); /* row stored, not embedded yet */
    HU_ASSERT_EQ((long)vs.vtable->count(vs.ctx), 0L);
    HU_ASSERT_EQ(hu_post_send_defer_flush(), 1u);
    HU_ASSERT_EQ(s_embeds, 1);
    HU_ASSERT_EQ((long)vs.vtable->count(vs.ctx), 1L); /* same vector, after the send */

    unsetenv("HU_POST_SEND_DEFER"); /* OFF: embedded at write time, as today */
    hu_post_send_defer_begin();
    HU_ASSERT_EQ(mem.vtable->store(mem.ctx, "user:likes:jam", 14, "jam", 3, NULL, "", 0), HU_OK);
    HU_ASSERT_EQ(s_embeds, 2);
    HU_ASSERT_EQ((long)vs.vtable->count(vs.ctx), 2L);
    (void)hu_post_send_defer_flush();

    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}
/* SHADOW only counts the embedding: no key/content copy for a job it will
 * not queue (#604 review). Same-size rows under OFF and SHADOW: identical
 * bytes through the memory's allocator. */
static size_t psd_embed_bytes_under(const char *mode, size_t *embeds) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    hu_embedder_t emb = {.ctx = NULL, .vtable = &psd_emb_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);
    s_embeds = 0;
    if (mode)
        setenv("HU_POST_SEND_DEFER", mode, 1);
    else
        unsetenv("HU_POST_SEND_DEFER");
    hu_post_send_defer_begin();
    size_t before = hu_tracking_allocator_total_allocated(ta);
    HU_ASSERT_EQ(mem.vtable->store(mem.ctx, "user:likes:tea", 14, "tea", 3, NULL, "", 0), HU_OK);
    size_t bytes = hu_tracking_allocator_total_allocated(ta) - before;
    (void)hu_post_send_defer_flush();
    unsetenv("HU_POST_SEND_DEFER");
    *embeds = (size_t)s_embeds;
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0u);
    hu_tracking_allocator_destroy(ta);
    return bytes;
}

static void shadow_embed_allocates_nothing_extra(void) {
    size_t off_embeds = 0, shadow_embeds = 0;
    size_t off = psd_embed_bytes_under(NULL, &off_embeds);
    size_t shadow = psd_embed_bytes_under("shadow", &shadow_embeds);
    HU_ASSERT_EQ(off_embeds, 1u);
    HU_ASSERT_EQ(shadow_embeds, 1u); /* inline: SHADOW changes nothing */
    HU_ASSERT_EQ(shadow, off);
    hu_post_send_defer_stats_t st = hu_post_send_defer_last_stats();
    HU_ASSERT_EQ((int)st.mode, (int)HU_GATE_SHADOW);
    HU_ASSERT_EQ(st.embed, 1u); /* still counted for the SHADOW->LIVE measurement */
}
#endif

void run_post_send_defer_tests(void) {
    HU_TEST_SUITE("post_send_defer");
    HU_RUN_TEST(off_gate_is_byte_identical_to_no_window);
    HU_RUN_TEST(live_runs_extraction_once_after_flush);
    HU_RUN_TEST(live_final_model_equals_inline_model);
    HU_RUN_TEST(shadow_keeps_extraction_inline_and_counts_it);
    HU_RUN_TEST(shadow_extract_allocates_nothing_extra);
    HU_RUN_TEST(live_outside_window_runs_inline);
    HU_RUN_TEST(begin_runs_jobs_a_skipped_flush_left);
    HU_RUN_TEST(full_queue_refuses_and_flush_runs_in_background_lane);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(live_defers_row_embedding_until_flush);
    HU_RUN_TEST(shadow_embed_allocates_nothing_extra);
#endif
}
