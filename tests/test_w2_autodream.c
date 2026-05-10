/* W2 — AutoDream: quarantine review, community summaries, edge reweight,
 * adversarial quarantine-bomb. All tests run in :memory:. */

#include "human/agent/autodream.h"
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"
#include "human/memory/write_trust.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) { g_alloc = hu_system_allocator(); return &g_alloc; }

static void open_graph(hu_graph_t **g) {
    hu_error_t rc = hu_graph_open(A(), NULL, 0, g);
    HU_ASSERT_EQ(rc, HU_OK);
}

/* Helper: stuff one quarantine row with a custom trust score. */
static void quarantine(hu_graph_t *g, int64_t source_id, int64_t target_id, float trust,
                       int64_t quarantined_at_ms) {
    hu_write_trust_decision_t d = {0};
    d.score = trust;
    d.outcome = trust < 0.30f ? HU_WRITE_OUTCOME_DROP : HU_WRITE_OUTCOME_QUARANTINE;
    snprintf(d.reason, sizeof(d.reason), "test:%.2f", (double)trust);
    /* The quarantine writer uses event_start as the quarantined_at; pass our
     * synthetic timestamp through that field so AutoDream's age check works. */
    HU_ASSERT_EQ(hu_write_trust_quarantine_relation(g, "u1", 2, source_id, target_id,
                                                    HU_REL_WORKS_AT, 1.0f, quarantined_at_ms, 0,
                                                    0.6f, NULL, 0, "test", 4, &d),
                 HU_OK);
}

/* --- Quarantine review: drop low-trust --- */
static void test_w2_autodream_drops_low_trust_quarantine_entries(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    int64_t alice = 0, acme = 0;
    hu_graph_upsert_entity(g, "u1", 2, "Alice", 5, HU_ENTITY_PERSON, NULL, &alice);
    hu_graph_upsert_entity(g, "u1", 2, "Acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme);

    /* Three quarantine rows: low trust, low trust, mid trust. AutoDream
     * should drop the low-trust pair, keep the mid-trust one for review. */
    quarantine(g, alice, acme, 0.10f, 1735689600000LL);
    quarantine(g, alice, acme, 0.20f, 1735689600000LL);
    quarantine(g, alice, acme, 0.45f, 1735689600000LL);

    hu_autodream_config_t cfg = hu_autodream_default_config();
    cfg.now_ms = 1735689600000LL + 1000;       /* very fresh */
    cfg.enable_community_summaries = false;    /* isolate */
    cfg.enable_edge_reweight = false;
    hu_autodream_report_t r = {0};
    HU_ASSERT_EQ(hu_autodream_run(A(), g, &cfg, &r), HU_OK);

    HU_ASSERT_EQ(r.quarantine_reviewed, 3);
    HU_ASSERT_EQ(r.quarantine_dropped, 2);
    /* Mid-trust 0.45 doesn't pass the 0.50 release floor and isn't aged
     * enough to drop -> remains in quarantine. */
    HU_ASSERT_EQ(r.quarantine_released, 0);

    size_t qcount = 0;
    hu_write_trust_quarantine_count(g, "u1", 2, &qcount);
    HU_ASSERT_EQ(qcount, 1);

    hu_graph_close(g, A());
}

/* --- Quarantine review: aged drops, even with mid trust --- */
static void test_w2_autodream_drops_aged_quarantine_entries(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    int64_t alice = 0, acme = 0;
    hu_graph_upsert_entity(g, "u1", 2, "Alice", 5, HU_ENTITY_PERSON, NULL, &alice);
    hu_graph_upsert_entity(g, "u1", 2, "Acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme);

    quarantine(g, alice, acme, 0.40f, 1700000000000LL); /* old entry */

    hu_autodream_config_t cfg = hu_autodream_default_config();
    cfg.now_ms = 1700000000000LL + 30LL * 24 * 3600 * 1000; /* 30d later */
    cfg.enable_community_summaries = false;
    cfg.enable_edge_reweight = false;
    hu_autodream_report_t r = {0};
    HU_ASSERT_EQ(hu_autodream_run(A(), g, &cfg, &r), HU_OK);

    HU_ASSERT_EQ(r.quarantine_reviewed, 1);
    HU_ASSERT_EQ(r.quarantine_dropped, 1);
    hu_graph_close(g, A());
}

/* --- Quarantine review: release on high trust without contradiction --- */
static void test_w2_autodream_releases_high_trust_when_no_contradiction(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    int64_t alice = 0, acme = 0;
    hu_graph_upsert_entity(g, "u1", 2, "Alice", 5, HU_ENTITY_PERSON, NULL, &alice);
    hu_graph_upsert_entity(g, "u1", 2, "Acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme);

    quarantine(g, alice, acme, 0.65f, 1735689600000LL);

    hu_autodream_config_t cfg = hu_autodream_default_config();
    cfg.now_ms = 1735689600000LL + 1000;
    cfg.enable_community_summaries = false;
    cfg.enable_edge_reweight = false;
    hu_autodream_report_t r = {0};
    HU_ASSERT_EQ(hu_autodream_run(A(), g, &cfg, &r), HU_OK);

    HU_ASSERT_EQ(r.quarantine_released, 1);

    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    hu_graph_list_relations(g, A(), "u1", 2, 32, &rels, &n);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_STR_CONTAINS(rels[0].provenance, "released:autodream:");
    hu_graph_relations_free(A(), rels, n);

    hu_graph_close(g, A());
}

/* --- Quarantine release through W7 facade (same observable graph as graph-only run) --- */
static void test_w2_autodream_quarantine_release_uses_facade_write(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    HU_ASSERT_EQ(hu_graph_open(A(), NULL, 0, &g), HU_OK);
    HU_ASSERT_EQ(hu_memory_facade_open(A(), g, &m), HU_OK);

    int64_t alice = 0, acme = 0;
    hu_graph_upsert_entity(g, "u1", 2, "Alice", 5, HU_ENTITY_PERSON, NULL, &alice);
    hu_graph_upsert_entity(g, "u1", 2, "Acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme);
    quarantine(g, alice, acme, 0.65f, 1735689600000LL);

    hu_autodream_config_t cfg = hu_autodream_default_config();
    cfg.now_ms = 1735689600000LL + 1000;
    cfg.enable_community_summaries = false;
    cfg.enable_edge_reweight = false;
    hu_autodream_report_t r = {0};
    HU_ASSERT_EQ(hu_autodream_run_on_facade(A(), m, &cfg, &r), HU_OK);
    HU_ASSERT_EQ(r.quarantine_released, 1);

    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    hu_graph_list_relations(g, A(), "u1", 2, 32, &rels, &n);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_STR_CONTAINS(rels[0].provenance, "released:autodream:");
    hu_graph_relations_free(A(), rels, n);

    hu_memory_facade_close(m, A());
    hu_graph_close(g, A());
}

/* --- Quarantine review: do NOT release when live high-confidence contradiction --- */
static void test_w2_autodream_does_not_release_when_contradicted(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    int64_t alice = 0, acme = 0, evil = 0;
    hu_graph_upsert_entity(g, "u1", 2, "Alice", 5, HU_ENTITY_PERSON, NULL, &alice);
    hu_graph_upsert_entity(g, "u1", 2, "Acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme);
    hu_graph_upsert_entity(g, "u1", 2, "EvilCorp", 8, HU_ENTITY_ORGANIZATION, NULL, &evil);

    /* Live high-confidence: Alice works at Acme. */
    hu_graph_upsert_relation_ex(g, "u1", 2, alice, acme, HU_REL_WORKS_AT, 1.0f,
                                1735689600000LL, 0, 1.0f, NULL, 0, "user", 4);

    /* Quarantined low-confidence different employer (target = EvilCorp). */
    quarantine(g, alice, evil, 0.55f, 1735689600000LL);

    hu_autodream_config_t cfg = hu_autodream_default_config();
    cfg.now_ms = 1735689600000LL + 1000;
    cfg.enable_community_summaries = false;
    cfg.enable_edge_reweight = false;
    hu_autodream_report_t r = {0};
    HU_ASSERT_EQ(hu_autodream_run(A(), g, &cfg, &r), HU_OK);

    HU_ASSERT_EQ(r.quarantine_released, 0); /* contradiction blocks release */

    /* Live graph still only has the user-typed Acme relation. */
    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    hu_graph_list_relations(g, A(), "u1", 2, 32, &rels, &n);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_EQ(rels[0].target_id, acme);
    hu_graph_relations_free(A(), rels, n);

    hu_graph_close(g, A());
}

/* --- Community summaries: smoke test --- */
static void test_w2_autodream_writes_community_summary(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    int64_t a = 0, b = 0, c = 0;
    hu_graph_upsert_entity(g, "u1", 2, "Alice", 5, HU_ENTITY_PERSON, NULL, &a);
    hu_graph_upsert_entity(g, "u1", 2, "Bob", 3, HU_ENTITY_PERSON, NULL, &b);
    hu_graph_upsert_entity(g, "u1", 2, "Carol", 5, HU_ENTITY_PERSON, NULL, &c);
    /* Manually assign a community_id so summarizer has something to work on. */
    hu_graph_set_entity_community(g, a, 100);
    hu_graph_set_entity_community(g, b, 100);
    hu_graph_set_entity_community(g, c, 100);

    HU_ASSERT_EQ(hu_autodream_summarize_community(A(), g, "u1", 2, 100, 1735689600000LL), HU_OK);

    char *summary = NULL;
    size_t slen = 0;
    HU_ASSERT_EQ(hu_autodream_read_community_summary(A(), g, "u1", 2, 100, &summary, &slen), HU_OK);
    HU_ASSERT_NOT_NULL(summary);
    HU_ASSERT_STR_CONTAINS(summary, "Community 100");
    HU_ASSERT_STR_CONTAINS(summary, "Alice");
    A()->free(g_alloc.ctx, summary, slen + 1);

    hu_graph_close(g, A());
}

/* --- Edge reweight: stale edges decay --- */
static void test_w2_autodream_decays_stale_edges(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    int64_t a = 0, b = 0;
    hu_graph_upsert_entity(g, "u1", 2, "A", 1, HU_ENTITY_PERSON, NULL, &a);
    hu_graph_upsert_entity(g, "u1", 2, "B", 1, HU_ENTITY_ORGANIZATION, NULL, &b);
    hu_graph_upsert_relation(g, "u1", 2, a, b, HU_REL_KNOWS, 0.8f, NULL, 0);

    hu_autodream_config_t cfg = hu_autodream_default_config();
    /* Place "now" 60 days after the relation's last_seen so it's stale. */
    cfg.now_ms = (int64_t)time(NULL) * 1000 + 60LL * 24 * 3600 * 1000;
    cfg.enable_quarantine_review = false;
    cfg.enable_community_summaries = false;
    hu_autodream_report_t r = {0};
    HU_ASSERT_EQ(hu_autodream_run(A(), g, &cfg, &r), HU_OK);

    HU_ASSERT_EQ(r.edges_reweighted, 1);

    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    hu_graph_list_relations(g, A(), "u1", 2, 32, &rels, &n);
    HU_ASSERT_EQ(n, 1);
    /* 0.8 * 0.95 = 0.76 (with a small float epsilon). */
    HU_ASSERT_FLOAT_EQ(rels[0].weight, 0.76f, 0.01f);
    hu_graph_relations_free(A(), rels, n);

    hu_graph_close(g, A());
}

/* --- Adversarial: quarantine bomb. 50 low-trust entries should all be dropped
 * by AutoDream regardless of their per-fact confidence. Live graph stays clean. */
static void test_w2_autodream_handles_quarantine_bomb(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    int64_t alice = 0, target = 0;
    hu_graph_upsert_entity(g, "u1", 2, "Alice", 5, HU_ENTITY_PERSON, NULL, &alice);
    hu_graph_upsert_entity(g, "u1", 2, "Target", 6, HU_ENTITY_ORGANIZATION, NULL, &target);

    for (int i = 0; i < 50; i++)
        quarantine(g, alice, target, 0.10f + 0.001f * (float)i, 1735689600000LL);

    hu_autodream_config_t cfg = hu_autodream_default_config();
    cfg.now_ms = 1735689600000LL + 1000;
    cfg.enable_community_summaries = false;
    cfg.enable_edge_reweight = false;
    hu_autodream_report_t r = {0};
    HU_ASSERT_EQ(hu_autodream_run(A(), g, &cfg, &r), HU_OK);

    HU_ASSERT_EQ(r.quarantine_reviewed, 50);
    HU_ASSERT_EQ(r.quarantine_dropped, 50);
    HU_ASSERT_EQ(r.quarantine_released, 0);

    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    hu_graph_list_relations(g, A(), "u1", 2, 128, &rels, &n);
    HU_ASSERT_EQ(n, 0); /* live graph never touched */
    hu_graph_relations_free(A(), rels, n);

    hu_graph_close(g, A());
}

#endif /* HU_ENABLE_SQLITE */

void run_w2_autodream_tests(void) {
    HU_TEST_SUITE("W2 autodream");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_w2_autodream_drops_low_trust_quarantine_entries);
    HU_RUN_TEST(test_w2_autodream_drops_aged_quarantine_entries);
    HU_RUN_TEST(test_w2_autodream_releases_high_trust_when_no_contradiction);
    HU_RUN_TEST(test_w2_autodream_quarantine_release_uses_facade_write);
    HU_RUN_TEST(test_w2_autodream_does_not_release_when_contradicted);
    HU_RUN_TEST(test_w2_autodream_writes_community_summary);
    HU_RUN_TEST(test_w2_autodream_decays_stale_edges);
    HU_RUN_TEST(test_w2_autodream_handles_quarantine_bomb);
#endif
}
