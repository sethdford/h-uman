/* DEF-4 through hu_agent_turn: the imperfect-delivery hedge reaches the
 * model's prompt only when the agent's own trajectory confidence is low AND
 * this turn's retrieval found nothing relevant (hu_certainty_classify in
 * src/humanness.c, fed by hu_agent_build_humanness_context). Before
 * 2026-10-01 a hedge went into ~260 of 307 reply turns; the round-0 fix could
 * never fire in prod because the always-present "[Core Memory]" block made
 * memory_ctx non-empty on every turn. */
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include "human/agent.h"
#include "human/agent/turn.h"
#include "human/core/allocator.h"
#include "human/humanness.h"
#include "human/memory.h"
#include "test_env_guard.h"
#include "test_tmpdir.h"
#include "turn_recording_provider.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The HU_UNCERTAIN directive (hu_imperfect_delivery_directive). */
#define HEDGE_SNIPPET "Be genuinely transparent about uncertainty here"

typedef struct {
    hu_agent_t agent;
    trp_t trp;
    hu_memory_t mem;
    bool have_mem;
    char dir[256];
    hu_test_env_guard_t env; /* HOME / HU_STATE_DIR as they were before the turn */
} hedge_turn_t;

/* The current test's tmpdir, for the runner (HU_RUN_TEST_ENV_GUARDED) to
 * remove after restoring the env — a failed assert longjmps past deinit. */
static char s_hedge_scratch[256];

static bool hedge_turn_init(hedge_turn_t *t, hu_allocator_t *alloc, bool seed_memory) {
    memset(t, 0, sizeof(*t));
    if (!hu_test_mkdtemp("/tmp/hu_hedge_turn_", t->dir, sizeof(t->dir)))
        return false;
    snprintf(s_hedge_scratch, sizeof(s_hedge_scratch), "%s", t->dir);
    hu_test_env_guard_save(&t->env);
    setenv("HOME", t->dir, 1);
    setenv("HU_STATE_DIR", t->dir, 1);
    if (seed_memory) {
        t->mem = hu_sqlite_memory_create(alloc, ":memory:");
        t->have_mem = t->mem.vtable != NULL;
        if (!t->have_mem)
            goto fail;
        hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
        const char *key = "fav_color", *val = "favorite color: teal";
        (void)t->mem.vtable->store(t->mem.ctx, key, strlen(key), val, strlen(val), &cat, NULL, 0);
    }
    trp_init(&t->trp, NULL, 0, "ok.");
    if (hu_agent_from_config(&t->agent, alloc, trp_provider(&t->trp), NULL, 0,
                             t->have_mem ? &t->mem : NULL, NULL, NULL, NULL, "hedge-model", 11,
                             "hedge", 5, 0.7, t->dir, strlen(t->dir), 5, 50, false, 1, NULL, 0,
                             NULL, 0, NULL) != HU_OK) {
        trp_deinit(&t->trp);
        goto fail;
    }
    return true;
fail:
    if (t->have_mem && t->mem.vtable->deinit)
        t->mem.vtable->deinit(t->mem.ctx);
    hu_test_env_guard_restore(&t->env);
    hu_test_rm_rf(t->dir);
    s_hedge_scratch[0] = '\0';
    return false;
}

static void hedge_turn_deinit(hedge_turn_t *t) {
    hu_agent_deinit(&t->agent);
    trp_deinit(&t->trp);
    if (t->have_mem && t->mem.vtable->deinit)
        t->mem.vtable->deinit(t->mem.ctx);
    hu_test_env_guard_restore(&t->env);
    hu_test_rm_rf(t->dir);
    s_hedge_scratch[0] = '\0';
}

/* Seed the metacognition ring with one low-confidence turn: the learned
 * signal hu_metacog_trajectory_confidence reads (0.2 < 0.5 threshold). */
static void set_low_self_confidence(hu_agent_t *a) {
    hu_metacognition_t *mc = &a->infra.metacognition;
    mc->signals[0].confidence = 0.2f;
    mc->signals[0].coherence = 0.2f;
    mc->signals[0].emotional_alignment = 0.2f;
    mc->signal_count = 1;
    mc->signal_idx = 1;
}

static bool run_turn(hedge_turn_t *t, const char *msg) {
    char *r = NULL;
    size_t rlen = 0;
    hu_error_t err = hu_agent_turn(&t->agent, msg, strlen(msg), &r, &rlen);
    if (r)
        t->agent.alloc->free(t->agent.alloc->ctx, r, rlen + 1);
    return err == HU_OK;
}

static void hedge_turn_low_confidence_and_empty_retrieval_hedges(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hedge_turn_t t;
    HU_ASSERT_TRUE(hedge_turn_init(&t, &alloc, false));
    set_low_self_confidence(&t.agent);
    HU_ASSERT_TRUE(run_turn(&t, "what time does the game start tomorrow?"));
    HU_ASSERT_GT(t.trp.calls, 0u);
    HU_ASSERT_NOT_NULL(strstr(t.trp.log, HEDGE_SNIPPET));
    hedge_turn_deinit(&t);
}

static void hedge_turn_relevant_retrieval_does_not_hedge(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hedge_turn_t t;
    HU_ASSERT_TRUE(hedge_turn_init(&t, &alloc, true));
    set_low_self_confidence(&t.agent); /* same low confidence: retrieval is the difference */
    HU_ASSERT_TRUE(run_turn(&t, "what is my favorite color"));
    HU_ASSERT_GT(t.trp.calls, 0u);
    HU_ASSERT_NOT_NULL(strstr(t.trp.log, "teal")); /* the recall reached the prompt */
    HU_ASSERT_NULL(strstr(t.trp.log, HEDGE_SNIPPET));
    hedge_turn_deinit(&t);
}

static void hedge_turn_casual_turn_does_not_hedge(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hedge_turn_t t;
    HU_ASSERT_TRUE(hedge_turn_init(&t, &alloc, false));
    /* No metacognition history: the ring reports its 0.5 no-data value. */
    HU_ASSERT_TRUE(run_turn(&t, "lol how was the game?"));
    HU_ASSERT_GT(t.trp.calls, 0u);
    HU_ASSERT_NULL(strstr(t.trp.log, HEDGE_SNIPPET));
    HU_ASSERT_EQ((int)hu_certainty_classify(false, 0.5f), (int)HU_CERTAIN);
    hedge_turn_deinit(&t);
}

/* The stream path's retrieval signal (hu_turn_memory_relevant) applies the
 * same Self-RAG gate + verify as S3, so a recall that S3 would drop as
 * irrelevant does not count as "retrieval found something" there either. */
static void hedge_memory_relevant_uses_self_rag_like_s3(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hedge_turn_t t;
    HU_ASSERT_TRUE(hedge_turn_init(&t, &alloc, false));
    HU_ASSERT_TRUE(t.agent.sota.sota_initialized && t.agent.sota.srag_config.enabled);
    static const char q[] = "what actually happened around here yesterday";
    static const char unrelated[] = "yesterday it rained hard, otherwise a quiet day";
    static const char related[] = "around here yesterday the street fair actually happened";
    HU_ASSERT_FALSE(hu_turn_memory_relevant(&t.agent, q, sizeof(q) - 1, NULL, 0));
    HU_ASSERT_FALSE(
        hu_turn_memory_relevant(&t.agent, q, sizeof(q) - 1, unrelated, sizeof(unrelated) - 1));
    HU_ASSERT_TRUE(
        hu_turn_memory_relevant(&t.agent, q, sizeof(q) - 1, related, sizeof(related) - 1));
    hedge_turn_deinit(&t);
}

void run_humanness_hedge_turn_tests(void) {
    HU_TEST_SUITE("humanness_hedge_turn");
    HU_RUN_TEST_ENV_GUARDED(hedge_turn_low_confidence_and_empty_retrieval_hedges, s_hedge_scratch);
    HU_RUN_TEST_ENV_GUARDED(hedge_turn_relevant_retrieval_does_not_hedge, s_hedge_scratch);
    HU_RUN_TEST_ENV_GUARDED(hedge_turn_casual_turn_does_not_hedge, s_hedge_scratch);
    HU_RUN_TEST_ENV_GUARDED(hedge_memory_relevant_uses_self_rag_like_s3, s_hedge_scratch);
}

#else

void run_humanness_hedge_turn_tests(void) {
    (void)0;
}

#endif
