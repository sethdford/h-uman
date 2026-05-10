/* W5 — Agent-writable persona deltas + evolver.
 * Adversarial coverage: rate-limit, low-confidence drop, corroboration gating,
 * malicious flood quarantine. */

#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/persona/persona_deltas.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) {
    g_alloc = hu_system_allocator();
    return &g_alloc;
}

static void open_graph(hu_graph_t **g) {
    HU_ASSERT_EQ(hu_graph_open(A(), NULL, 0, g), HU_OK);
}

/* --- Propose + list round-trip --- */
static void test_w5_propose_persists_and_lists(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_persona_delta_propose(g, "u1", 2, HU_PERSONA_DELTA_TONE, "slack",
                                          "warmer", 0.8f, "agent-inference",
                                          1735689600000LL, &id),
                 HU_OK);
    HU_ASSERT(id > 0);

    hu_persona_delta_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_persona_delta_list(g, A(), "u1", 2, HU_DELTA_STATUS_PENDING, 16, &out, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_EQ(out[0].id, id);
    HU_ASSERT_STR_EQ(out[0].value, "warmer");
    HU_ASSERT_STR_EQ(out[0].key, "slack");
    HU_ASSERT(out[0].confidence > 0.79f && out[0].confidence < 0.81f);
    hu_persona_delta_free(A(), out, n);
    hu_graph_close(g, A());
}

/* --- Evolver applies high-confidence deltas --- */
static void test_w5_evolver_applies_high_confidence(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    int64_t id = 0;
    hu_persona_delta_propose(g, "u1", 2, HU_PERSONA_DELTA_LENGTH, "slack", "shorter", 0.9f,
                              "agent-inference", 1735689600000LL, &id);

    hu_persona_evolver_config_t cfg = hu_persona_evolver_default_config();
    cfg.now_ms = 1735689600000LL + 5000;
    hu_persona_evolver_report_t r;
    HU_ASSERT_EQ(hu_persona_evolver_run(g, "u1", 2, &cfg, &r), HU_OK);
    HU_ASSERT_EQ(r.applied, 1);
    HU_ASSERT_EQ(r.dropped, 0);

    hu_persona_delta_t *out = NULL;
    size_t n = 0;
    hu_persona_delta_list(g, A(), "u1", 2, HU_DELTA_STATUS_APPLIED, 16, &out, &n);
    HU_ASSERT_EQ(n, 1);
    hu_persona_delta_free(A(), out, n);
    hu_graph_close(g, A());
}

/* --- Evolver drops low-confidence deltas --- */
static void test_w5_evolver_drops_low_confidence(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    int64_t id = 0;
    hu_persona_delta_propose(g, "u1", 2, HU_PERSONA_DELTA_TONE, "slack", "rude", 0.3f,
                              "agent-inference", 1735689600000LL, &id);

    hu_persona_evolver_config_t cfg = hu_persona_evolver_default_config();
    cfg.now_ms = 1735689600000LL + 5000;
    hu_persona_evolver_report_t r;
    HU_ASSERT_EQ(hu_persona_evolver_run(g, "u1", 2, &cfg, &r), HU_OK);
    HU_ASSERT_EQ(r.dropped, 1);
    HU_ASSERT_EQ(r.applied, 0);
    hu_graph_close(g, A());
}

/* --- Mid-confidence delta needs corroboration --- */
static void test_w5_evolver_requires_corroboration_for_mid_confidence(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    /* One mid-confidence proposal. */
    int64_t id = 0;
    hu_persona_delta_propose(g, "u1", 2, HU_PERSONA_DELTA_VOCAB_AVOID, "all", "obviously", 0.6f,
                              "agent-inference", 1735689600000LL, &id);

    hu_persona_evolver_config_t cfg = hu_persona_evolver_default_config();
    cfg.now_ms = 1735689600000LL + 5000;
    cfg.corroboration_min = 3;
    hu_persona_evolver_report_t r;
    HU_ASSERT_EQ(hu_persona_evolver_run(g, "u1", 2, &cfg, &r), HU_OK);
    HU_ASSERT_EQ(r.applied, 0);
    HU_ASSERT_EQ(r.still_pending, 1);

    /* Add corroborating evidence. */
    hu_persona_delta_propose(g, "u1", 2, HU_PERSONA_DELTA_VOCAB_AVOID, "all", "obviously", 0.6f,
                              "agent-inference", 1735689600000LL + 1000, NULL);
    hu_persona_delta_propose(g, "u1", 2, HU_PERSONA_DELTA_VOCAB_AVOID, "all", "obviously", 0.6f,
                              "agent-inference", 1735689600000LL + 2000, NULL);

    hu_persona_evolver_report_t r2;
    HU_ASSERT_EQ(hu_persona_evolver_run(g, "u1", 2, &cfg, &r2), HU_OK);
    HU_ASSERT(r2.applied >= 1);
    hu_graph_close(g, A());
}

/* --- ADVERSARIAL: flood from one source is rate-limited --- */
static void test_w5_evolver_quarantines_flood_from_single_source(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    /* 25 proposals in the last hour from one suspicious source. */
    for (int i = 0; i < 25; i++) {
        char val[16];
        snprintf(val, sizeof(val), "trait_%d", i);
        hu_persona_delta_propose(g, "u1", 2, HU_PERSONA_DELTA_VALUE, "all", val, 0.95f,
                                  "rogue-channel", 1735689600000LL + i * 1000LL, NULL);
    }

    hu_persona_evolver_config_t cfg = hu_persona_evolver_default_config();
    cfg.now_ms = 1735689600000LL + 30000;
    cfg.rate_limit_per_hour = 10;
    hu_persona_evolver_report_t r;
    HU_ASSERT_EQ(hu_persona_evolver_run(g, "u1", 2, &cfg, &r), HU_OK);
    /* The flood should result in many quarantined entries; some might apply
     * if rate_count <= 10. We require strict majority to be quarantined. */
    HU_ASSERT(r.quarantined >= 14);
    hu_graph_close(g, A());
}

#endif /* HU_ENABLE_SQLITE */

void run_w5_persona_deltas_tests(void) {
    HU_TEST_SUITE("W5 persona deltas + evolver");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_w5_propose_persists_and_lists);
    HU_RUN_TEST(test_w5_evolver_applies_high_confidence);
    HU_RUN_TEST(test_w5_evolver_drops_low_confidence);
    HU_RUN_TEST(test_w5_evolver_requires_corroboration_for_mid_confidence);
    HU_RUN_TEST(test_w5_evolver_quarantines_flood_from_single_source);
#endif
}
