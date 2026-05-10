/* W8 — Belief layer + hyperedge adversarial tests.
 *
 * belief.c: deterministic math (no SQLite dependency).
 * hyperedge.c: SQLite-backed; tests run in-memory via hu_memory_facade_open().
 */

#include "human/core/allocator.h"
#include "human/memory/belief.h"
#include "human/memory/graph.h"
#include "human/memory/hyperedge.h"
#include "human/memory/memory.h"
#include "test_framework.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ---- belief tests (no SQLite required) -------------------------------- */

static void test_w8_belief_init_sets_mean_and_variance(void) {
    hu_belief_t b = hu_belief_init(0.8f, "user-explicit", 1000LL);
    HU_ASSERT_FLOAT_EQ(b.mean, 0.8f, 1e-5f);
    /* variance = mean*(1-mean) = 0.16 */
    HU_ASSERT_FLOAT_EQ(b.variance, 0.16f, 1e-5f);
    HU_ASSERT_EQ(b.prov_count, 1);
    HU_ASSERT_EQ(b.last_updated, 1000LL);
}

static void test_w8_belief_update_converges(void) {
    /* 100 corroborating observations of 1.0 should push mean → near 1.0
     * and variance → near 0. */
    hu_belief_t b = hu_belief_init(1.0f, "src-a", 0LL);
    for (int i = 0; i < 99; i++) {
        b = hu_belief_update(&b, 1.0f, "src-a", (int64_t)(i + 1));
    }
    /* After 100 consistent observations: mean should be very close to 1.0. */
    HU_ASSERT(b.mean > 0.99f);
    /* Variance should be near zero (precision grows with each corroborating obs). */
    HU_ASSERT(b.variance < 0.01f);
}

static void test_w8_belief_diverges_on_contradiction(void) {
    /* Initial belief at 0.5 with high variance. Alternating 0.0 / 1.0
     * observations should keep variance elevated relative to initial. */
    hu_belief_t b = hu_belief_init(0.5f, "src-a", 0LL);
    float initial_variance = b.variance;
    for (int i = 0; i < 20; i++) {
        float obs = (i % 2 == 0) ? 0.0f : 1.0f;
        b = hu_belief_update(&b, obs, "src-b", (int64_t)(i + 1));
    }
    /* Alternating contradictions: variance should not have collapsed to near-zero. */
    HU_ASSERT(b.variance > initial_variance * 0.5f);
}

static void test_w8_belief_combine_weighted(void) {
    /* Inverse-variance pooling: high-precision belief should dominate. */
    /* a: mean=0.9, tight variance (after many corroborating observations) */
    hu_belief_t a = hu_belief_init(0.9f, "src-a", 0LL);
    for (int i = 0; i < 50; i++)
        a = hu_belief_update(&a, 0.9f, "src-a", (int64_t)(i + 1));

    /* b: mean=0.1, wide variance (fresh single observation) */
    hu_belief_t b = hu_belief_init(0.1f, "src-b", 100LL);

    hu_belief_t c = hu_belief_combine(&a, &b);

    /* Combined mean should be closer to a (higher precision). */
    HU_ASSERT(c.mean > 0.5f);
    HU_ASSERT(c.mean <= 0.9f + 1e-3f);
    /* Combined variance should not exceed max of the two. */
    float max_var = a.variance > b.variance ? a.variance : b.variance;
    HU_ASSERT(c.variance <= max_var + 1e-6f);
}

static void test_w8_belief_significantly_disagrees_detects(void) {
    hu_belief_t a = hu_belief_init(0.9f, "src-a", 0LL);
    hu_belief_t b = hu_belief_init(0.1f, "src-b", 0LL);
    /* With sigma_threshold=1.0, |0.9-0.1|=0.8 vs spread=sqrt(0.09+0.09)≈0.42.
     * 0.8 > 1.0*0.42 → true. */
    HU_ASSERT(hu_belief_significantly_disagrees(&a, &b, 1.0f));
}

static void test_w8_belief_significantly_disagrees_agrees(void) {
    hu_belief_t a = hu_belief_init(0.8f, "src-a", 0LL);
    hu_belief_t b = hu_belief_init(0.8f, "src-b", 0LL);
    /* Same beliefs: should not disagree. */
    HU_ASSERT(!hu_belief_significantly_disagrees(&a, &b, 1.0f));
}

static void test_w8_adversarial_belief_poisoning_grows_variance(void) {
    /* Attacker from 4 different sources sends 50 conflicting observations:
     * alternating 0.0 and 1.0. Variance must rise above 0.05 (threshold). */
    const char *sources[] = {"attacker-1", "attacker-2", "attacker-3", "attacker-4"};
    hu_belief_t b = hu_belief_init(0.5f, "trusted", 0LL);
    for (int i = 0; i < 50; i++) {
        float obs = (i % 2 == 0) ? 0.0f : 1.0f;
        b = hu_belief_update(&b, obs, sources[i % 4], (int64_t)(i + 1));
    }
    /* 50 strongly contradicting observations must elevate variance above 0.05. */
    HU_ASSERT(b.variance > 0.05f);
}

/* ---- semantic conflict tests (no SQLite required) ---------------------- */

static void test_w8_semantic_conflict_identical_strings_no_conflict(void) {
    hu_belief_conflict_t c = hu_belief_semantic_conflict(
        "I like cats", 11, "I like cats", 11);
    HU_ASSERT(c == HU_BELIEF_CONFLICT_PARAPHRASE || c == HU_BELIEF_CONFLICT_NONE);
    HU_ASSERT(c != HU_BELIEF_CONFLICT_CONTRADICT);
}

static void test_w8_semantic_conflict_negation_detected(void) {
    hu_belief_conflict_t c = hu_belief_semantic_conflict(
        "I like cats", 11, "I do not like cats", 18);
    HU_ASSERT_EQ((int)c, (int)HU_BELIEF_CONFLICT_CONTRADICT);
}

static void test_w8_semantic_conflict_different_subjects_no_conflict(void) {
    hu_belief_conflict_t c = hu_belief_semantic_conflict(
        "I like cats", 11, "I like dogs", 11);
    HU_ASSERT(c != HU_BELIEF_CONFLICT_CONTRADICT);
}

static void test_w8_semantic_conflict_null_inputs_safe(void) {
    HU_ASSERT_EQ((int)hu_belief_semantic_conflict(NULL, 0, "hello", 5),
                 (int)HU_BELIEF_CONFLICT_NONE);
    HU_ASSERT_EQ((int)hu_belief_semantic_conflict("hello", 5, NULL, 0),
                 (int)HU_BELIEF_CONFLICT_NONE);
    HU_ASSERT_EQ((int)hu_belief_semantic_conflict(NULL, 0, NULL, 0),
                 (int)HU_BELIEF_CONFLICT_NONE);
}

static void test_w8_semantic_conflict_empty_strings_no_conflict(void) {
    HU_ASSERT_EQ((int)hu_belief_semantic_conflict("", 0, "", 0),
                 (int)HU_BELIEF_CONFLICT_NONE);
    HU_ASSERT_EQ((int)hu_belief_semantic_conflict("hello", 5, "", 0),
                 (int)HU_BELIEF_CONFLICT_NONE);
}

/* ---- hyperedge tests (SQLite required) --------------------------------- */

#ifdef HU_ENABLE_SQLITE

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) {
    g_alloc = hu_system_allocator();
    return &g_alloc;
}

static void open_facade(hu_graph_t **g, hu_memory_facade_t **m) {
    HU_ASSERT_EQ(hu_graph_open(A(), NULL, 0, g), HU_OK);
    HU_ASSERT_NOT_NULL(*g);
    HU_ASSERT_EQ(hu_memory_facade_open(A(), *g, m), HU_OK);
    HU_ASSERT_NOT_NULL(*m);
}

static void close_facade(hu_graph_t *g, hu_memory_facade_t *m) {
    hu_memory_facade_close(m, A());
    hu_graph_close(g, A());
}

static void test_w8_hyperedge_zero_members_rejected(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    hu_hyperedge_t he;
    memset(&he, 0, sizeof(he));
    memcpy(he.relation_label, "met_at", 6);
    he.members = NULL;
    he.members_count = 0;
    he.belief = hu_belief_init(1.0f, "test", 0LL);

    int64_t id = 0;
    HU_ASSERT_EQ(hu_hyperedge_upsert(m, "u1", 2, &he, &id), HU_ERR_INVALID_ARGUMENT);

    close_facade(g, m);
}

static void test_w8_hyperedge_upsert_round_trip(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    /* Insert two entities via the graph API. */
    int64_t alice = 0, bob = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u1", 2, "Alice", 5, HU_ENTITY_PERSON, NULL, &alice),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u1", 2, "Bob", 3, HU_ENTITY_PERSON, NULL, &bob),
                 HU_OK);
    HU_ASSERT_GT(alice, 0);
    HU_ASSERT_GT(bob, 0);

    hu_hyperedge_member_t members[2];
    memset(members, 0, sizeof(members));
    members[0].entity_id = alice;
    memcpy(members[0].role, "subject", 7);
    members[1].entity_id = bob;
    memcpy(members[1].role, "object", 6);

    hu_hyperedge_t he;
    memset(&he, 0, sizeof(he));
    memcpy(he.relation_label, "met_at", 6);
    he.members = members;
    he.members_count = 2;
    he.belief = hu_belief_init(0.9f, "imessage", 5000LL);
    he.event_start = 5000LL;

    int64_t edge_id = 0;
    HU_ASSERT_EQ(hu_hyperedge_upsert(m, "u1", 2, &he, &edge_id), HU_OK);
    HU_ASSERT_GT(edge_id, 0);

    /* Query by alice — should find the edge. */
    hu_hyperedge_t *results = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_hyperedge_query_by_member(m, A(), alice, &results, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_EQ(results[0].id, edge_id);
    HU_ASSERT_EQ(results[0].members_count, 2);
    HU_ASSERT_FLOAT_EQ(results[0].belief.mean, 0.9f, 1e-3f);

    hu_hyperedges_free(A(), results, n);
    close_facade(g, m);
}

static void test_w8_hyperedge_query_by_any_member(void) {
    /* Insert a 4-member hyperedge; verify it's findable from each member. */
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_facade(&g, &m);

    int64_t ids[4] = {0, 0, 0, 0};
    const char *names[] = {"Alice", "Bob", "Acme", "FundingRound"};
    hu_entity_type_t types[] = {HU_ENTITY_PERSON, HU_ENTITY_PERSON,
                                 HU_ENTITY_ORGANIZATION, HU_ENTITY_TOPIC};
    for (int i = 0; i < 4; i++) {
        HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u1", 2, names[i], strlen(names[i]),
                                             types[i], NULL, &ids[i]),
                     HU_OK);
        HU_ASSERT_GT(ids[i], 0);
    }

    const char *roles[] = {"subject", "object", "location", "topic"};
    hu_hyperedge_member_t members[4];
    memset(members, 0, sizeof(members));
    for (int i = 0; i < 4; i++) {
        members[i].entity_id = ids[i];
        memcpy(members[i].role, roles[i], strlen(roles[i]));
    }

    hu_hyperedge_t he;
    memset(&he, 0, sizeof(he));
    memcpy(he.relation_label, "discussed", 9);
    he.members = members;
    he.members_count = 4;
    he.belief = hu_belief_init(1.0f, "feed-web", 9000LL);

    int64_t edge_id = 0;
    HU_ASSERT_EQ(hu_hyperedge_upsert(m, "u1", 2, &he, &edge_id), HU_OK);
    HU_ASSERT_GT(edge_id, 0);

    /* Verify findable from each of the 4 members. */
    for (int i = 0; i < 4; i++) {
        hu_hyperedge_t *results = NULL;
        size_t n = 0;
        HU_ASSERT_EQ(hu_hyperedge_query_by_member(m, A(), ids[i], &results, &n), HU_OK);
        HU_ASSERT_EQ(n, 1);
        HU_ASSERT_EQ(results[0].id, edge_id);
        HU_ASSERT_EQ(results[0].members_count, 4);
        hu_hyperedges_free(A(), results, n);
    }

    close_facade(g, m);
}

#endif /* HU_ENABLE_SQLITE */

/* ---- test runner ------------------------------------------------------- */

void run_w8_belief_layer_tests(void) {
    HU_TEST_SUITE("W8 belief layer - hu_belief_t + hyperedges");

    /* Belief math tests: no SQLite dependency. */
    HU_RUN_TEST(test_w8_belief_init_sets_mean_and_variance);
    HU_RUN_TEST(test_w8_belief_update_converges);
    HU_RUN_TEST(test_w8_belief_diverges_on_contradiction);
    HU_RUN_TEST(test_w8_belief_combine_weighted);
    HU_RUN_TEST(test_w8_belief_significantly_disagrees_detects);
    HU_RUN_TEST(test_w8_belief_significantly_disagrees_agrees);
    HU_RUN_TEST(test_w8_adversarial_belief_poisoning_grows_variance);

    /* Semantic conflict tests. */
    HU_RUN_TEST(test_w8_semantic_conflict_identical_strings_no_conflict);
    HU_RUN_TEST(test_w8_semantic_conflict_negation_detected);
    HU_RUN_TEST(test_w8_semantic_conflict_different_subjects_no_conflict);
    HU_RUN_TEST(test_w8_semantic_conflict_null_inputs_safe);
    HU_RUN_TEST(test_w8_semantic_conflict_empty_strings_no_conflict);

#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_w8_hyperedge_zero_members_rejected);
    HU_RUN_TEST(test_w8_hyperedge_upsert_round_trip);
    HU_RUN_TEST(test_w8_hyperedge_query_by_any_member);
#endif
}
