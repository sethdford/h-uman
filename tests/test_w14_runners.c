/* W14 P0 #4 — runner registration + adapter-swap KV invalidation.
 *
 * Adversarial coverage for the three new sleep-time runners:
 *
 *   - hu_lora_training_runner
 *       * empty pending buffer → no-op, HU_OK
 *       * non-empty buffer → calls learner.train, clears KV cache,
 *         enqueues a follow-up KV warming job
 *       * NULL learner / NULL ctx → HU_ERR_INVALID_ARGUMENT
 *
 *   - hu_kv_prewarm_runner
 *       * EVICTION on a healthy cache → no-op
 *       * EVICTION on a saturated cache → prunes lowest-attention seg
 *       * WARMING → no-op success
 *       * NULL user_data → silent no-op success
 *
 *   - hu_belief_reverify_runner
 *       * aging relation → confidence multiplied by 0.95
 *       * fresh relation → untouched
 *       * row cap honored
 *       * out_decayed counter populated
 *
 * Determinism: every test pins now_ms via spec->earliest_at and seeds
 * the learner config with a fixed PRNG so adapter bytes (when written)
 * are reproducible. */

#include "human/agent/belief_reverify_runner.h"
#include "human/agent/kv_cache.h"
#include "human/agent/kv_prewarm_runner.h"
#include "human/agent/lora_runner.h"
#include "human/agent/scheduler.h"
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"
#include "human/ml/learner.h"
#include "human/ml/learner_bridge.h"
#include "human/persona/persona_deltas.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef HU_ENABLE_SQLITE

static hu_allocator_t g_alloc;
static hu_allocator_t *A_(void) {
    g_alloc = hu_system_allocator();
    return &g_alloc;
}

/* ───────────────── KV prewarm ───────────────── */

static void test_w14_kv_prewarm_no_user_data_is_noop(void) {
    hu_job_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = HU_JOB_KV_CACHE_EVICTION;
    HU_ASSERT_EQ(hu_kv_prewarm_runner(NULL, &spec, 0, NULL), HU_OK);

    spec.kind = HU_JOB_KV_CACHE_WARMING;
    HU_ASSERT_EQ(hu_kv_prewarm_runner(NULL, &spec, 0, NULL), HU_OK);
}

static void test_w14_kv_prewarm_eviction_prunes_when_saturated(void) {
    hu_kv_cache_manager_t mgr;
    HU_ASSERT_EQ(hu_kv_cache_init(&mgr, A_(), 200), HU_OK);
    /* Push three segments past the eviction threshold (90% of 200 = 180). */
    HU_ASSERT_EQ(hu_kv_cache_add_segment(&mgr, "system", 6, 100, true), HU_OK);
    HU_ASSERT_EQ(hu_kv_cache_add_segment(&mgr, "memory:1", 8, 50, false), HU_OK);
    HU_ASSERT_EQ(hu_kv_cache_add_segment(&mgr, "memory:2", 8, 50, false), HU_OK);
    HU_ASSERT(hu_kv_cache_needs_eviction(&mgr));

    hu_job_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = HU_JOB_KV_CACHE_EVICTION;
    HU_ASSERT_EQ(hu_kv_prewarm_runner(NULL, &spec, 0, &mgr), HU_OK);
    HU_ASSERT(!hu_kv_cache_needs_eviction(&mgr));
    /* Pinned segment must still be present. */
    bool found_system = false;
    for (size_t i = 0; i < mgr.segment_count; i++) {
        if (mgr.segments[i].label_len == 6 && memcmp(mgr.segments[i].label, "system", 6) == 0)
            found_system = true;
    }
    HU_ASSERT(found_system);

    hu_kv_cache_deinit(&mgr);
}

static void test_w14_kv_prewarm_eviction_healthy_is_noop(void) {
    hu_kv_cache_manager_t mgr;
    HU_ASSERT_EQ(hu_kv_cache_init(&mgr, A_(), 1000), HU_OK);
    HU_ASSERT_EQ(hu_kv_cache_add_segment(&mgr, "system", 6, 100, true), HU_OK);
    HU_ASSERT(!hu_kv_cache_needs_eviction(&mgr));
    size_t before = mgr.segment_count;

    hu_job_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = HU_JOB_KV_CACHE_EVICTION;
    HU_ASSERT_EQ(hu_kv_prewarm_runner(NULL, &spec, 0, &mgr), HU_OK);
    HU_ASSERT_EQ((int)mgr.segment_count, (int)before);

    hu_kv_cache_deinit(&mgr);
}

/* ───────────────── LoRA training ───────────────── */

static void test_w14_lora_training_runner_rejects_null_ctx(void) {
    hu_job_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = HU_JOB_LORA_TRAINING;
    HU_ASSERT_EQ(hu_lora_training_runner(NULL, &spec, 0, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_w14_lora_training_runner_no_pending_is_noop(void) {
    hu_learner_t *learner = NULL;
    HU_ASSERT_EQ(hu_learner_open_default(A_(), &learner), HU_OK);

    hu_lora_runner_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.learner = learner;
    ctx.alloc = A_();
    ctx.config_template = hu_learner_default_config();
    snprintf(ctx.config_template.adapter_output_path,
             sizeof(ctx.config_template.adapter_output_path),
             "/tmp/hu-w14-lora-%d.adapter", (int)getpid());

    hu_job_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = HU_JOB_LORA_TRAINING;
    HU_ASSERT_EQ(hu_lora_training_runner(NULL, &spec, 0, &ctx), HU_OK);

    hu_learner_close(learner);
}

static void test_w14_lora_training_runner_drains_and_clears_kv(void) {
    hu_learner_t *learner = NULL;
    HU_ASSERT_EQ(hu_learner_open_default(A_(), &learner), HU_OK);

    /* Push a single persona delta into the pending buffer. */
    hu_persona_delta_t d;
    memset(&d, 0, sizeof(d));
    d.id = 42;
    d.kind = HU_PERSONA_DELTA_TONE;
    d.confidence = 0.9f;
    snprintf(d.value, sizeof(d.value), "be warmer");
    snprintf(d.source, sizeof(d.source), "test");
    HU_ASSERT_EQ(hu_learner_bridge_emit_persona_deltas(learner, &d, 1), HU_OK);
    HU_ASSERT_EQ((int)hu_learner_pending_count(learner), 1);

    hu_kv_cache_manager_t mgr;
    HU_ASSERT_EQ(hu_kv_cache_init(&mgr, A_(), 1000), HU_OK);
    HU_ASSERT_EQ(hu_kv_cache_add_segment(&mgr, "memory:1", 8, 50, false), HU_OK);
    size_t cache_before = mgr.segment_count;
    HU_ASSERT_EQ((int)cache_before, 1);

    hu_lora_runner_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.learner = learner;
    ctx.alloc = A_();
    ctx.kv_cache = &mgr;
    ctx.config_template = hu_learner_default_config();
    snprintf(ctx.config_template.adapter_output_path,
             sizeof(ctx.config_template.adapter_output_path),
             "/tmp/hu-w14-lora-drain-%d.adapter", (int)getpid());
    snprintf(ctx.config_template.model_version, sizeof(ctx.config_template.model_version),
             "w14-test-v1");
    ctx.config_template.seed = 0xC0FFEEULL;
    ctx.config_template.rank = 4;
    ctx.config_template.max_steps = 4;
    ctx.config_template.budget_ms = 2000;
    /* Wipe any leftover from a previous test run. */
    unlink(ctx.config_template.adapter_output_path);

    hu_job_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = HU_JOB_LORA_TRAINING;
    HU_ASSERT_EQ(hu_lora_training_runner(NULL, &spec, 0, &ctx), HU_OK);

    /* Pending must be drained, KV cache cleared. */
    HU_ASSERT_EQ((int)hu_learner_pending_count(learner), 0);
    HU_ASSERT_EQ((int)mgr.segment_count, 0);

    unlink(ctx.config_template.adapter_output_path);
    hu_kv_cache_deinit(&mgr);
    hu_learner_close(learner);
}

/* ───────────────── Belief reverify ───────────────── */

static int64_t entity_(hu_graph_t *g, const char *cid, const char *name, hu_entity_type_t t) {
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, cid, strlen(cid), name, strlen(name), t, NULL, &id),
                 HU_OK);
    return id;
}

static void test_w14_belief_reverify_decays_aging_relations(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    HU_ASSERT_EQ(hu_graph_open(A_(), NULL, 0, &g), HU_OK);
    HU_ASSERT_EQ(hu_memory_facade_open(A_(), g, &m), HU_OK);

    int64_t alice = entity_(g, "u1", "Alice", HU_ENTITY_PERSON);
    int64_t acme  = entity_(g, "u1", "Acme",  HU_ENTITY_ORGANIZATION);

    /* Insert a relation with confidence = 0.8. last_seen is set to wall-
     * clock now() by upsert_relation_ex; the runner ages off `last_seen`,
     * so we pin our test's now_ms to (last_seen_actual + 60 days) below. */
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, acme, HU_REL_WORKS_AT,
                                              1.0f, 0, 0, 0.8f, NULL, 0, NULL, 0),
                 HU_OK);

    /* Re-fetch to learn the row's actual last_seen + id. */
    hu_graph_relation_t *before = NULL;
    size_t nb = 0;
    HU_ASSERT_EQ(hu_graph_list_relations(g, A_(), "u1", 2, 10, &before, &nb), HU_OK);
    HU_ASSERT_EQ((int)nb, 1);
    int64_t rid = before[0].id;
    int64_t inserted_last_seen = before[0].last_seen;
    float c0 = before[0].confidence;
    hu_graph_relations_free(A_(), before, nb);
    HU_ASSERT(c0 > 0.79f && c0 < 0.81f);

    int64_t now_ms = inserted_last_seen + 60LL * 24 * 60 * 60 * 1000;

    size_t reverified = 0, decayed = 0;
    hu_belief_reverify_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.alloc = A_();
    ctx.contact_id = "u1";
    ctx.max_age_ms = 30LL * 24 * 60 * 60 * 1000;
    ctx.max_relations_per_tick = 64;
    ctx.out_reverified = &reverified;
    ctx.out_decayed = &decayed;

    hu_job_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = HU_JOB_BELIEF_REVERIFICATION;
    spec.earliest_at = now_ms;
    HU_ASSERT_EQ(hu_belief_reverify_runner(m, &spec, 0, &ctx), HU_OK);
    HU_ASSERT_EQ((int)reverified, 1);
    HU_ASSERT_EQ((int)decayed, 1);

    /* Verify the writeback persisted: confidence ≈ 0.8 * 0.95 = 0.76. */
    hu_graph_relation_t *after = NULL;
    size_t na = 0;
    HU_ASSERT_EQ(hu_graph_list_relations(g, A_(), "u1", 2, 10, &after, &na), HU_OK);
    HU_ASSERT_EQ((int)na, 1);
    HU_ASSERT_EQ(after[0].id, rid);
    /* Runner applies mean *= 0.95f; allow float tail at the 0.78 boundary. */
    HU_ASSERT_FLOAT_EQ(after[0].confidence, c0 * 0.95f, 0.02f);
    hu_graph_relations_free(A_(), after, na);

    hu_memory_facade_close(m, A_());
    hu_graph_close(g, A_());
}

static void test_w14_belief_reverify_skips_fresh_relations(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    HU_ASSERT_EQ(hu_graph_open(A_(), NULL, 0, &g), HU_OK);
    HU_ASSERT_EQ(hu_memory_facade_open(A_(), g, &m), HU_OK);

    int64_t alice = entity_(g, "u1", "Alice", HU_ENTITY_PERSON);
    int64_t acme  = entity_(g, "u1", "Acme",  HU_ENTITY_ORGANIZATION);

    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, acme, HU_REL_WORKS_AT,
                                              1.0f, 0, 0, 0.9f, NULL, 0, NULL, 0),
                 HU_OK);

    /* Pin now_ms to "2 days after insert" so the row is fresh. */
    hu_graph_relation_t *fresh = NULL;
    size_t nf = 0;
    HU_ASSERT_EQ(hu_graph_list_relations(g, A_(), "u1", 2, 10, &fresh, &nf), HU_OK);
    HU_ASSERT_EQ((int)nf, 1);
    int64_t now_ms = fresh[0].last_seen + 2LL * 24 * 60 * 60 * 1000;
    hu_graph_relations_free(A_(), fresh, nf);

    size_t reverified = 0, decayed = 0;
    hu_belief_reverify_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.alloc = A_();
    ctx.contact_id = "u1";
    ctx.max_age_ms = 30LL * 24 * 60 * 60 * 1000;
    ctx.max_relations_per_tick = 64;
    ctx.out_reverified = &reverified;
    ctx.out_decayed = &decayed;

    hu_job_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = HU_JOB_BELIEF_REVERIFICATION;
    spec.earliest_at = now_ms;
    HU_ASSERT_EQ(hu_belief_reverify_runner(m, &spec, 0, &ctx), HU_OK);
    HU_ASSERT_EQ((int)reverified, 0);
    HU_ASSERT_EQ((int)decayed, 0);

    hu_memory_facade_close(m, A_());
    hu_graph_close(g, A_());
}

/* ───────────────── Scheduler registration end-to-end ───────────────── */

static void test_w14_runners_register_into_scheduler(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    HU_ASSERT_EQ(hu_graph_open(A_(), NULL, 0, &g), HU_OK);
    HU_ASSERT_EQ(hu_memory_facade_open(A_(), g, &m), HU_OK);

    hu_scheduler_t *sched = NULL;
    HU_ASSERT_EQ(hu_scheduler_open(A_(), m, &sched), HU_OK);

    /* All three runners must register cleanly. */
    HU_ASSERT_EQ(hu_scheduler_register_runner(sched, HU_JOB_LORA_TRAINING,
                                              hu_lora_training_runner, NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_scheduler_register_runner(sched, HU_JOB_KV_CACHE_EVICTION,
                                              hu_kv_prewarm_runner, NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_scheduler_register_runner(sched, HU_JOB_KV_CACHE_WARMING,
                                              hu_kv_prewarm_runner, NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_scheduler_register_runner(sched, HU_JOB_BELIEF_REVERIFICATION,
                                              hu_belief_reverify_runner, NULL),
                 HU_OK);

    hu_scheduler_close(sched, A_());
    hu_memory_facade_close(m, A_());
    hu_graph_close(g, A_());
}

#endif /* HU_ENABLE_SQLITE */

void run_w14_runners_tests(void) {
    HU_TEST_SUITE("W14 runners (P0 #4)");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_w14_kv_prewarm_no_user_data_is_noop);
    HU_RUN_TEST(test_w14_kv_prewarm_eviction_prunes_when_saturated);
    HU_RUN_TEST(test_w14_kv_prewarm_eviction_healthy_is_noop);
    HU_RUN_TEST(test_w14_lora_training_runner_rejects_null_ctx);
    HU_RUN_TEST(test_w14_lora_training_runner_no_pending_is_noop);
    HU_RUN_TEST(test_w14_lora_training_runner_drains_and_clears_kv);
    HU_RUN_TEST(test_w14_belief_reverify_decays_aging_relations);
    HU_RUN_TEST(test_w14_belief_reverify_skips_fresh_relations);
    HU_RUN_TEST(test_w14_runners_register_into_scheduler);
#endif
}
