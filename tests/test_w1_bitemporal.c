/* W1 — Bitemporal foundation: schema migration, conflict resolver, write trust,
 * adversarial poisoning, and round-trip. Every test runs in :memory: via the
 * HU_IS_TEST guard in graph.c, so no real DB or network is touched. */

#include "human/core/allocator.h"
#include "human/memory/conflict_resolver.h"
#include "human/memory/graph.h"
#include "human/memory/write_trust.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

/* Tests use a single shared system allocator so leaks surface via ASan. */
static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) { g_alloc = hu_system_allocator(); return &g_alloc; }

static void open_graph(hu_graph_t **g) {
    hu_error_t rc = hu_graph_open(A(), NULL, 0, g);
    HU_ASSERT_EQ(rc, HU_OK);
    HU_ASSERT_NOT_NULL(*g);
}

/* --- Schema / migration --- */

static void test_w1_schema_legacy_upsert_populates_event_start(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);

    int64_t alice = 0, acme = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u1", 2, "Alice", 5, HU_ENTITY_PERSON, NULL, &alice),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u1", 2, "Acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme),
                 HU_OK);

    HU_ASSERT_EQ(hu_graph_upsert_relation(g, "u1", 2, alice, acme, HU_REL_WORKS_AT, 1.0f, NULL, 0),
                 HU_OK);

    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_list_relations(g, A(), "u1", 2, 32, &rels, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_GT(rels[0].event_start, 0);
    HU_ASSERT_EQ(rels[0].event_end, 0);              /* still true */
    HU_ASSERT_FLOAT_EQ(rels[0].confidence, 1.0f, 1e-3);
    HU_ASSERT_EQ(rels[0].supersedes_id, 0);
    HU_ASSERT_NULL(rels[0].provenance);

    hu_graph_relations_free(A(), rels, n);
    hu_graph_close(g, A());
}

/* --- Conflict resolver classifier --- */

static void test_w1_conflict_classifier_supersede_on_works_at_change(void) {
    hu_graph_relation_t old_ = {0}, new_ = {0};
    old_.id = 1;
    old_.target_id = 100;
    old_.type = HU_REL_WORKS_AT;
    old_.event_end = 0;            /* still true */
    old_.confidence = 1.0f;

    new_.target_id = 200;          /* changed employer */
    new_.type = HU_REL_WORKS_AT;
    new_.confidence = 1.0f;

    HU_ASSERT_EQ(hu_conflict_classify(&new_, &old_), HU_CONFLICT_SUPERSEDE);
}

static void test_w1_conflict_classifier_branch_on_multi_valued(void) {
    hu_graph_relation_t old_ = {0}, new_ = {0};
    old_.id = 1;
    old_.target_id = 100;
    old_.type = HU_REL_KNOWS;
    old_.event_end = 0;
    old_.confidence = 1.0f;

    new_.target_id = 200;
    new_.type = HU_REL_KNOWS;
    new_.confidence = 1.0f;

    HU_ASSERT_EQ(hu_conflict_classify(&new_, &old_), HU_CONFLICT_BRANCH);
}

static void test_w1_conflict_classifier_flag_on_low_conf_vs_high(void) {
    hu_graph_relation_t old_ = {0}, new_ = {0};
    old_.id = 1;
    old_.target_id = 100;
    old_.type = HU_REL_WORKS_AT;
    old_.event_end = 0;
    old_.confidence = 0.95f;

    new_.target_id = 200;
    new_.type = HU_REL_WORKS_AT;
    new_.confidence = 0.30f;

    HU_ASSERT_EQ(hu_conflict_classify(&new_, &old_), HU_CONFLICT_FLAG);
}

static void test_w1_conflict_classifier_none_when_no_existing(void) {
    hu_graph_relation_t new_ = {0};
    new_.target_id = 200;
    new_.type = HU_REL_WORKS_AT;
    new_.confidence = 1.0f;
    HU_ASSERT_EQ(hu_conflict_classify(&new_, NULL), HU_CONFLICT_NONE);
}

/* --- End-to-end supersession through upsert_ex --- */

static void test_w1_upsert_ex_supersedes_prior_employer(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);

    int64_t alice = 0, acme = 0, globex = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u1", 2, "Alice", 5, HU_ENTITY_PERSON, NULL, &alice),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u1", 2, "Acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme),
                 HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(g, "u1", 2, "Globex", 6, HU_ENTITY_ORGANIZATION, NULL, &globex),
        HU_OK);

    /* Alice works at Acme starting Jan 1, 2024. */
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, acme, HU_REL_WORKS_AT, 1.0f,
                                             1704067200000LL /* 2024-01-01 */, 0, 1.0f, NULL, 0,
                                             "linkedin", 8),
                 HU_OK);

    /* New observation: switched to Globex on Jan 1, 2025. */
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, globex, HU_REL_WORKS_AT, 1.0f,
                                             1735689600000LL /* 2025-01-01 */, 0, 1.0f, NULL, 0,
                                             "imessage:jan-1-2025", 19),
                 HU_OK);

    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_list_relations(g, A(), "u1", 2, 32, &rels, &n), HU_OK);
    HU_ASSERT_EQ(n, 2);

    /* Find the closed (acme) and open (globex) rows. */
    int closed_idx = -1, open_idx = -1;
    for (size_t i = 0; i < n; i++) {
        if (rels[i].target_id == acme && rels[i].event_end != 0)
            closed_idx = (int)i;
        if (rels[i].target_id == globex && rels[i].event_end == 0)
            open_idx = (int)i;
    }
    HU_ASSERT_GT(closed_idx + 1, 0);
    HU_ASSERT_GT(open_idx + 1, 0);
    HU_ASSERT_EQ(rels[closed_idx].event_end, 1735689600000LL); /* cutover */
    HU_ASSERT_EQ(rels[open_idx].supersedes_id, rels[closed_idx].id);
    HU_ASSERT_STR_CONTAINS(rels[open_idx].provenance, "imessage");

    hu_graph_relations_free(A(), rels, n);
    hu_graph_close(g, A());
}

/* --- Window query --- */

static void test_w1_relations_in_window_returns_overlapping(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);

    int64_t alice = 0, acme = 0, globex = 0;
    hu_graph_upsert_entity(g, "u1", 2, "Alice", 5, HU_ENTITY_PERSON, NULL, &alice);
    hu_graph_upsert_entity(g, "u1", 2, "Acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme);
    hu_graph_upsert_entity(g, "u1", 2, "Globex", 6, HU_ENTITY_ORGANIZATION, NULL, &globex);

    hu_graph_upsert_relation_ex(g, "u1", 2, alice, acme, HU_REL_WORKS_AT, 1.0f,
                                1704067200000LL /* 2024-01-01 */, 0, 1.0f, NULL, 0, NULL, 0);
    hu_graph_upsert_relation_ex(g, "u1", 2, alice, globex, HU_REL_WORKS_AT, 1.0f,
                                1735689600000LL /* 2025-01-01 */, 0, 1.0f, NULL, 0, NULL, 0);

    /* Q: who did Alice work for during 2024? Should return only Acme. */
    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_relations_in_window(g, A(), "u1", 2,
                                              1704067200000LL /* 2024-01-01 */,
                                              1735603200000LL /* 2024-12-31 */, 32, &rels, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_EQ(rels[0].target_id, acme);
    hu_graph_relations_free(A(), rels, n);

    /* Q: who did Alice work for during 2025? Should return only Globex. */
    rels = NULL;
    n = 0;
    HU_ASSERT_EQ(hu_graph_relations_in_window(g, A(), "u1", 2,
                                              1735689600000LL /* 2025-01-01 */,
                                              1767139200000LL /* 2025-12-30 */, 32, &rels, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_EQ(rels[0].target_id, globex);
    hu_graph_relations_free(A(), rels, n);

    hu_graph_close(g, A());
}

/* --- Write trust --- */

static void test_w1_trust_user_typed_lands_live(void) {
    hu_write_trust_input_t in = {0};
    in.source = HU_WRITE_SOURCE_USER;
    in.observed_at = 1735689600000LL;
    in.now = 1735689600000LL;
    in.contradiction_flag = false;
    in.supersession = false;
    in.recent_writes = 1;
    in.rate_limit = 100;
    hu_write_trust_decision_t d = hu_write_trust_score(&in);
    HU_ASSERT_EQ(d.outcome, HU_WRITE_OUTCOME_LIVE);
    HU_ASSERT(d.score >= 0.6f);
}

static void test_w1_trust_open_channel_with_contradiction_quarantines(void) {
    hu_write_trust_input_t in = {0};
    in.source = HU_WRITE_SOURCE_CHANNEL_OPEN;
    in.observed_at = 1735689600000LL;
    in.now = 1735689600000LL + 24LL * 3600 * 1000;     /* 24h old */
    in.contradiction_flag = true;
    in.recent_writes = 1;
    in.rate_limit = 100;
    hu_write_trust_decision_t d = hu_write_trust_score(&in);
    HU_ASSERT_EQ(d.outcome, HU_WRITE_OUTCOME_QUARANTINE);
    HU_ASSERT_STR_CONTAINS(d.reason, "contradiction");
}

static void test_w1_trust_rate_limit_trips_drop(void) {
    hu_write_trust_input_t in = {0};
    in.source = HU_WRITE_SOURCE_FEED_WEB;
    in.observed_at = 1735689600000LL;
    in.now = 1735689600000LL + 7LL * 24 * 3600 * 1000; /* 7d old */
    in.recent_writes = 9000;
    in.rate_limit = 100;
    hu_write_trust_decision_t d = hu_write_trust_score(&in);
    HU_ASSERT_EQ(d.outcome, HU_WRITE_OUTCOME_DROP);
    HU_ASSERT_STR_CONTAINS(d.reason, "rate-limit");
}

static void test_w1_quarantine_table_round_trip(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    int64_t a = 0, b = 0;
    hu_graph_upsert_entity(g, "u1", 2, "A", 1, HU_ENTITY_PERSON, NULL, &a);
    hu_graph_upsert_entity(g, "u1", 2, "B", 1, HU_ENTITY_ORGANIZATION, NULL, &b);

    hu_write_trust_decision_t d = {0};
    d.score = 0.42f;
    d.outcome = HU_WRITE_OUTCOME_QUARANTINE;
    snprintf(d.reason, sizeof(d.reason), "low-trust-source:feed-web");

    HU_ASSERT_EQ(hu_write_trust_quarantine_relation(g, "u1", 2, a, b, HU_REL_WORKS_AT, 1.0f,
                                                    1735689600000LL, 0, 0.6f, NULL, 0,
                                                    "https://example.com/blog", 24, &d),
                 HU_OK);

    size_t count = 0;
    HU_ASSERT_EQ(hu_write_trust_quarantine_count(g, "u1", 2, &count), HU_OK);
    HU_ASSERT_EQ(count, 1);

    /* Live graph must NOT contain the quarantined fact. */
    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_list_relations(g, A(), "u1", 2, 32, &rels, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);
    hu_graph_relations_free(A(), rels, n);

    hu_graph_close(g, A());
}

/* --- MINJA-style adversarial poisoning ---
 * A high-volume web source attempts to overwrite a high-confidence user fact
 * with a low-confidence contradiction. Live graph must be unchanged; the
 * malicious row must end up in quarantine (or be dropped). */
static void test_w1_minja_poisoning_does_not_overwrite_user_truth(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);

    int64_t alice = 0, acme = 0, evilcorp = 0;
    hu_graph_upsert_entity(g, "u1", 2, "Alice", 5, HU_ENTITY_PERSON, NULL, &alice);
    hu_graph_upsert_entity(g, "u1", 2, "Acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme);
    hu_graph_upsert_entity(g, "u1", 2, "EvilCorp", 8, HU_ENTITY_ORGANIZATION, NULL, &evilcorp);

    /* User typed: "Alice works at Acme." High confidence. */
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, acme, HU_REL_WORKS_AT, 1.0f,
                                             1735689600000LL, 0, 1.0f, NULL, 0, "user-typed", 10),
                 HU_OK);

    /* Adversarial web feed says: "Alice works at EvilCorp." Low confidence,
     * high recent_write rate from same source. Caller scores BEFORE write. */
    hu_write_trust_input_t adv = {0};
    adv.source = HU_WRITE_SOURCE_FEED_WEB;
    adv.observed_at = 1735689600000LL;
    adv.now = 1735689600000LL;
    adv.contradiction_flag = true; /* would be flagged by classifier */
    adv.recent_writes = 5000;
    adv.rate_limit = 50;
    hu_write_trust_decision_t d = hu_write_trust_score(&adv);
    HU_ASSERT(d.outcome != HU_WRITE_OUTCOME_LIVE);

    /* Caller follows trust verdict — diverts to quarantine, never to live. */
    HU_ASSERT_EQ(hu_write_trust_quarantine_relation(g, "u1", 2, alice, evilcorp, HU_REL_WORKS_AT,
                                                    1.0f, 1735689600000LL, 0, 0.30f, NULL, 0,
                                                    "https://evil.example/spam", 25, &d),
                 HU_OK);

    /* Live graph: Alice still works at Acme, only one open relation. */
    hu_graph_relation_t *rels = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_list_relations(g, A(), "u1", 2, 32, &rels, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_EQ(rels[0].target_id, acme);
    HU_ASSERT_EQ(rels[0].event_end, 0);
    hu_graph_relations_free(A(), rels, n);

    /* Quarantine has the rejected row. */
    size_t qcount = 0;
    HU_ASSERT_EQ(hu_write_trust_quarantine_count(g, "u1", 2, &qcount), HU_OK);
    HU_ASSERT_EQ(qcount, 1);

    hu_graph_close(g, A());
}

#endif /* HU_ENABLE_SQLITE */

void run_w1_bitemporal_tests(void) {
    HU_TEST_SUITE("W1 bitemporal foundation");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_w1_schema_legacy_upsert_populates_event_start);
    HU_RUN_TEST(test_w1_conflict_classifier_supersede_on_works_at_change);
    HU_RUN_TEST(test_w1_conflict_classifier_branch_on_multi_valued);
    HU_RUN_TEST(test_w1_conflict_classifier_flag_on_low_conf_vs_high);
    HU_RUN_TEST(test_w1_conflict_classifier_none_when_no_existing);
    HU_RUN_TEST(test_w1_upsert_ex_supersedes_prior_employer);
    HU_RUN_TEST(test_w1_relations_in_window_returns_overlapping);
    HU_RUN_TEST(test_w1_trust_user_typed_lands_live);
    HU_RUN_TEST(test_w1_trust_open_channel_with_contradiction_quarantines);
    HU_RUN_TEST(test_w1_trust_rate_limit_trips_drop);
    HU_RUN_TEST(test_w1_quarantine_table_round_trip);
    HU_RUN_TEST(test_w1_minja_poisoning_does_not_overwrite_user_truth);
#endif
}
