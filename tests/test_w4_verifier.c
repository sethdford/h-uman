/* W4 — Response verifier + provenance receipts + targeted erasure.
 * Adversarial coverage: hallucination flagging, hedge insertion, attribution,
 * cross-surface erasure cascade, and provenance-scoped redaction. */

#include "human/agent/response_verifier.h"
#include "human/core/allocator.h"
#include "human/memory/cross_graph.h"
#include "human/memory/erasure.h"
#include "human/memory/graph.h"
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

/* Seed two entities and a relation with provenance text the verifier can find. */
static void seed_alice_works_at_acme(hu_graph_t *g, const char *prov,
                                     int64_t event_start_ms) {
    int64_t alice = 0, acme = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u1", 2, "alice", 5, HU_ENTITY_PERSON, NULL, &alice),
                 HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(g, "u1", 2, "acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme), HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, acme, HU_REL_WORKS_AT, 1.0f,
                                              event_start_ms, 0, 1.0f, "context", 7, prov,
                                              prov ? strlen(prov) : 0),
                 HU_OK);
}

/* --- Verifier OFF mode never modifies the draft --- */
static void test_w4_verifier_off_mode_is_passthrough(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    hu_verifier_config_t cfg = hu_verifier_default_config();
    cfg.mode = HU_VERIFY_OFF;
    hu_verifier_report_t r;
    const char *draft = "Alice works at Acme.";
    HU_ASSERT_EQ(hu_response_verify(A(), g, "u1", 2, draft, strlen(draft), &cfg, &r), HU_OK);
    HU_ASSERT(!r.draft_modified);
    HU_ASSERT_EQ(r.claims_extracted, 0);
    hu_graph_close(g, A());
}

/* --- TELEMETRY mode extracts and scores but never modifies the draft.
 * This is the mode the response path uses by default. --- */
static void test_w4_verifier_telemetry_mode_extracts_without_mutation(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    seed_alice_works_at_acme(g, "imessage", 1735689600000LL);

    hu_verifier_config_t cfg = hu_verifier_default_config();
    cfg.mode = HU_VERIFY_TELEMETRY;
    hu_verifier_report_t r;
    const char *draft = "Alice works at Acme.";
    HU_ASSERT_EQ(hu_response_verify(A(), g, "u1", 2, draft, strlen(draft), &cfg, &r), HU_OK);
    HU_ASSERT_EQ(r.claims_extracted, 1);
    HU_ASSERT_EQ(r.claims_supported, 1);
    HU_ASSERT(!r.draft_modified);
    HU_ASSERT_EQ(r.modified_draft[0], '\0');
    hu_graph_close(g, A());
}

/* --- TELEMETRY mode flags hallucinations without rewriting --- */
static void test_w4_verifier_telemetry_mode_flags_unsupported(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    seed_alice_works_at_acme(g, "imessage", 1735689600000LL);

    hu_verifier_config_t cfg = hu_verifier_default_config();
    cfg.mode = HU_VERIFY_TELEMETRY;
    hu_verifier_report_t r;
    const char *draft = "Bob is the CEO of Globex.";
    HU_ASSERT_EQ(hu_response_verify(A(), g, "u1", 2, draft, strlen(draft), &cfg, &r), HU_OK);
    HU_ASSERT_EQ(r.claims_extracted, 1);
    HU_ASSERT_EQ(r.claims_flagged, 1);
    HU_ASSERT(!r.claims[0].supported);
    HU_ASSERT(!r.draft_modified);
    hu_graph_close(g, A());
}

/* --- Supported claim is detected and gets a receipt --- */
static void test_w4_verifier_supports_known_fact(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    seed_alice_works_at_acme(g, "imessage", 1735689600000LL);

    hu_verifier_config_t cfg = hu_verifier_default_config();
    cfg.mode = HU_VERIFY_SOFT;
    hu_verifier_report_t r;
    const char *draft = "Alice works at Acme.";
    HU_ASSERT_EQ(hu_response_verify(A(), g, "u1", 2, draft, strlen(draft), &cfg, &r), HU_OK);
    HU_ASSERT_EQ(r.claims_extracted, 1);
    HU_ASSERT_EQ(r.claims_supported, 1);
    HU_ASSERT_EQ(r.claims_flagged, 0);
    HU_ASSERT(r.claims[0].supported);
    HU_ASSERT(r.claims[0].score >= cfg.confidence_threshold);
    HU_ASSERT(strstr(r.claims[0].receipt.rendered, "imessage") != NULL);
    hu_graph_close(g, A());
}

/* --- Hallucination is flagged with a hedge in SOFT mode --- */
static void test_w4_verifier_flags_unsupported_claim(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    seed_alice_works_at_acme(g, "imessage", 1735689600000LL);

    hu_verifier_config_t cfg = hu_verifier_default_config();
    cfg.mode = HU_VERIFY_SOFT;
    hu_verifier_report_t r;
    /* No memory backs this. */
    const char *draft = "Bob is the CEO of Globex.";
    HU_ASSERT_EQ(hu_response_verify(A(), g, "u1", 2, draft, strlen(draft), &cfg, &r), HU_OK);
    HU_ASSERT_EQ(r.claims_extracted, 1);
    HU_ASSERT_EQ(r.claims_flagged, 1);
    HU_ASSERT(!r.claims[0].supported);
    HU_ASSERT(r.claims[0].suggested_hedge[0] != '\0');
    HU_ASSERT(r.draft_modified);
    HU_ASSERT(strstr(r.modified_draft, "not 100%") != NULL);
    hu_graph_close(g, A());
}

/* --- ADVERSARIAL: poisoned claim about a real entity is still flagged --- */
static void test_w4_verifier_adversarial_poisoning_is_flagged(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    seed_alice_works_at_acme(g, "imessage", 1735689600000LL);

    hu_verifier_config_t cfg = hu_verifier_default_config();
    cfg.mode = HU_VERIFY_SOFT;
    cfg.confidence_threshold = 0.6f;
    hu_verifier_report_t r;
    /* Real names mixed with fabricated nonsense - should not score above threshold. */
    const char *draft = "Alice secretly was kidnapped by martians during her vacation.";
    HU_ASSERT_EQ(hu_response_verify(A(), g, "u1", 2, draft, strlen(draft), &cfg, &r), HU_OK);
    HU_ASSERT_EQ(r.claims_extracted, 1);
    HU_ASSERT(!r.claims[0].supported);
    HU_ASSERT(r.claims_flagged >= 1);
    hu_graph_close(g, A());
}

/* --- Provenance render handles missing fields without crashing --- */
static void test_w4_provenance_render_handles_missing(void) {
    char buf[160] = {0};
    hu_provenance_render(NULL, buf, sizeof(buf));
    HU_ASSERT(strstr(buf, "no source") != NULL);
    hu_graph_relation_t r = {0};
    hu_provenance_render(&r, buf, sizeof(buf));
    HU_ASSERT(strstr(buf, "from memory") != NULL);
    HU_ASSERT(strstr(buf, "unknown") != NULL);
}

/* --- Erasure: invalid id returns NOT_FOUND --- */
static void test_w4_erase_unknown_entity_returns_not_found(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    hu_erase_report_t r;
    HU_ASSERT_EQ(hu_memory_erase_entity(g, 9999, &r), HU_ERR_NOT_FOUND);
    hu_graph_close(g, A());
}

/* --- Erasure cascades across relations + cross_edges + quarantine --- */
static void test_w4_erase_entity_cascades_across_surfaces(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);

    int64_t alice = 0, acme = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u1", 2, "alice", 5, HU_ENTITY_PERSON, NULL, &alice),
                 HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(g, "u1", 2, "acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme), HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, acme, HU_REL_WORKS_AT, 1.0f,
                                              1735689600000LL, 0, 1.0f, "ctx", 3, "imessage", 8),
                 HU_OK);
    /* Cross-edge from alice to a synthetic episode 100. */
    HU_ASSERT_EQ(hu_cross_edge_upsert(g, "u1", 2, "entity", alice, "episode", 100, "ABOUT", 1.0f,
                                       1735689600000LL, 0, 1.0f),
                 HU_OK);

    hu_erase_report_t r;
    HU_ASSERT_EQ(hu_memory_erase_entity(g, alice, &r), HU_OK);
    HU_ASSERT(r.entity_deleted);
    HU_ASSERT(r.relations_deleted >= 1);
    HU_ASSERT(r.cross_edges_deleted >= 1);
    HU_ASSERT_EQ(r.entity_id, alice);

    /* The cross_edges table should now have no row for alice. */
    hu_cross_edge_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(
        hu_cross_graph_traverse(g, A(), "u1", 2, "entity", alice, 1, 32, 0, 0, &out, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);
    if (out)
        hu_cross_edges_free(A(), out, n);
    hu_graph_close(g, A());
}

/* --- Erasure by provenance prefix --- */
static void test_w4_erase_by_provenance_redacts_only_matching_rows(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);

    seed_alice_works_at_acme(g, "imessage:thread-7", 1735689600000LL);
    int64_t bob = 0, globex = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u1", 2, "bob", 3, HU_ENTITY_PERSON, NULL, &bob),
                 HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(g, "u1", 2, "globex", 6, HU_ENTITY_ORGANIZATION, NULL, &globex),
        HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, bob, globex, HU_REL_WORKS_AT, 1.0f,
                                              1735689600000LL, 0, 1.0f, "ctx", 3, "discord", 7),
                 HU_OK);

    hu_erase_report_t r;
    HU_ASSERT_EQ(hu_memory_erase_by_provenance(g, "imessage", 8, &r), HU_OK);
    HU_ASSERT(r.relations_deleted >= 1);

    /* The discord-sourced row should remain. */
    hu_graph_relation_t *list = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_list_relations(g, A(), "u1", 2, 32, &list, &n), HU_OK);
    HU_ASSERT(n >= 1);
    bool found_discord = false;
    for (size_t i = 0; i < n; i++)
        if (list[i].provenance && strstr(list[i].provenance, "discord"))
            found_discord = true;
    HU_ASSERT(found_discord);
    hu_graph_relations_free(A(), list, n);
    hu_graph_close(g, A());
}

#endif /* HU_ENABLE_SQLITE */

void run_w4_verifier_tests(void) {
    HU_TEST_SUITE("W4 verifier + provenance + erasure");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_w4_verifier_off_mode_is_passthrough);
    HU_RUN_TEST(test_w4_verifier_telemetry_mode_extracts_without_mutation);
    HU_RUN_TEST(test_w4_verifier_telemetry_mode_flags_unsupported);
    HU_RUN_TEST(test_w4_verifier_supports_known_fact);
    HU_RUN_TEST(test_w4_verifier_flags_unsupported_claim);
    HU_RUN_TEST(test_w4_verifier_adversarial_poisoning_is_flagged);
    HU_RUN_TEST(test_w4_provenance_render_handles_missing);
    HU_RUN_TEST(test_w4_erase_unknown_entity_returns_not_found);
    HU_RUN_TEST(test_w4_erase_entity_cascades_across_surfaces);
    HU_RUN_TEST(test_w4_erase_by_provenance_redacts_only_matching_rows);
#endif
}
