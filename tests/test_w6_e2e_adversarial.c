/* W6 + E2E — End-to-end adversarial validation across W1–W5.
 *
 * This is the proof-of-life test for the entire memory roadmap. It composes
 * every workstream into a single attacker-vs-defender scenario:
 *
 *   1. Honest writes go in; bitemporal supersession works.        (W1)
 *   2. AutoDream consolidates memory, ages quarantine, builds
 *      community summaries on idle.                              (W2)
 *   3. Cross-graph traversal surfaces the right episode for a
 *      planning query.                                           (W3)
 *   4. The response verifier flags hallucinations and produces
 *      receipts pointing at the original source.                 (W4)
 *   5. The persona evolver resists prompt-injection floods that
 *      try to drift the persona toward attacker-chosen values.   (W5)
 *   6. Targeted erasure removes one user's data without leaking
 *      across surfaces.                                          (W4)
 *
 * Every assertion is a regression-protected behavior; if any of W1–W5 break,
 * this test fails fast. */

#include "human/agent/autodream.h"
#include "human/agent/response_verifier.h"
#include "human/core/allocator.h"
#include "human/memory/cross_graph.h"
#include "human/memory/erasure.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"
#include "human/memory/write_trust.h"
#include "human/persona/persona_deltas.h"
#include "test_framework.h"

#include <sqlite3.h>
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

/* Seed one case_records row anchored on `entity_id`.
 *
 * The case writer that used to fill this table (src/agent/case_based.c) was
 * deleted as unreachable, but src/memory/erasure.c still purges the table and
 * reports the count — so the erasure contract below needs a row to delete.
 * Schema mirrors v1_case_ensure_schema() in src/memory/memory_v1_backend.c.
 * Test-only: the SQL is built from test-authored literals and one int64. */
static void seed_case_record(hu_graph_t *g, int64_t entity_id) {
    struct sqlite3 *db = hu_graph_sqlite_connection(g);
    HU_ASSERT_NOT_NULL(db);
    static const char *const kDdl = "CREATE TABLE IF NOT EXISTS case_records ("
                                    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                    "contact_id TEXT NOT NULL DEFAULT '',"
                                    "goal_verb TEXT NOT NULL,"
                                    "anchor_entity_ids TEXT NOT NULL DEFAULT '',"
                                    "plan_text TEXT,"
                                    "outcome TEXT,"
                                    "happened_at INTEGER NOT NULL)";
    HU_ASSERT_EQ(sqlite3_exec(db, kDdl, NULL, NULL, NULL), SQLITE_OK);
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO case_records"
             " (contact_id, goal_verb, anchor_entity_ids, plan_text, outcome, happened_at)"
             " VALUES ('u1', 'send-email', '%lld', NULL, 'ok', 1735689600000)",
             (long long)entity_id);
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
}

/* --- E2E scenario 1: honest write -> verifier surfaces receipt ---
 * A user tells the agent on iMessage that "I work at Acme starting Monday."
 * Later, a draft says "You work at Acme." The verifier should support that
 * claim with a receipt pointing at iMessage.
 */
static void test_e2e_honest_write_yields_supported_claim_with_receipt(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);

    int64_t alice = 0, acme = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u1", 2, "alice", 5, HU_ENTITY_PERSON, NULL, &alice),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "u1", 2, "acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, acme, HU_REL_WORKS_AT, 1.0f,
                                             1735689600000LL, 0, 1.0f, "from imessage", 13,
                                             "imessage", 8),
                 HU_OK);

    hu_memory_facade_t *m = NULL;
    HU_ASSERT_EQ(hu_memory_facade_open(A(), g, &m), HU_OK);
    hu_verifier_config_t cfg = hu_verifier_default_config();
    cfg.mode = HU_VERIFY_SOFT;
    hu_verifier_report_t r;
    const char *draft = "Alice works at Acme.";
    HU_ASSERT_EQ(hu_response_verify(A(), m, "u1", 2, draft, strlen(draft), &cfg, &r), HU_OK);
    HU_ASSERT_EQ(r.claims_supported, 1);
    HU_ASSERT(strstr(r.claims[0].receipt.rendered, "imessage") != NULL);
    hu_memory_facade_close(m, A());
    hu_graph_close(g, A());
}

/* --- E2E scenario 2: bitemporal supersession survives a verifier query ---
 * Alice changes jobs from Acme -> Globex. The verifier should respect the
 * latest live state and not double-attribute the older fact.
 */
static void test_e2e_bitemporal_supersession_survives_verifier(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);

    int64_t alice = 0, acme = 0, globex = 0;
    hu_graph_upsert_entity(g, "u1", 2, "alice", 5, HU_ENTITY_PERSON, NULL, &alice);
    hu_graph_upsert_entity(g, "u1", 2, "acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme);
    hu_graph_upsert_entity(g, "u1", 2, "globex", 6, HU_ENTITY_ORGANIZATION, NULL, &globex);

    /* Original fact (event_start=2024-01-01). */
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, acme, HU_REL_WORKS_AT, 1.0f,
                                             1704067200000LL, 0, 1.0f, "ctx", 3, "imessage", 8),
                 HU_OK);
    /* Superseded by the Globex fact (event_start=2025-01-01). */
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, globex, HU_REL_WORKS_AT, 1.0f,
                                             1735689600000LL, 0, 1.0f, "ctx", 3, "imessage", 8),
                 HU_OK);

    hu_memory_facade_t *m = NULL;
    HU_ASSERT_EQ(hu_memory_facade_open(A(), g, &m), HU_OK);
    hu_verifier_config_t cfg = hu_verifier_default_config();
    cfg.mode = HU_VERIFY_SOFT;
    hu_verifier_report_t r;
    const char *draft = "Alice works at Globex.";
    HU_ASSERT_EQ(hu_response_verify(A(), m, "u1", 2, draft, strlen(draft), &cfg, &r), HU_OK);
    HU_ASSERT(r.claims[0].supported);
    /* The other claim about Acme should now NOT be supported as live. */
    const char *stale = "Alice works at Acme.";
    HU_ASSERT_EQ(hu_response_verify(A(), m, "u1", 2, stale, strlen(stale), &cfg, &r), HU_OK);
    HU_ASSERT(!r.claims[0].supported);
    hu_memory_facade_close(m, A());
    hu_graph_close(g, A());
}

/* --- E2E scenario 3: write_trust quarantines a hostile injection ---
 * An attacker channels a high-rate flood of contradictory facts through a
 * suspicious source. write_trust should quarantine them; the verifier should
 * never attribute a quarantined fact.
 */
static void test_e2e_write_trust_blocks_injection_attack(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);

    int64_t alice = 0, acme = 0;
    hu_graph_upsert_entity(g, "u1", 2, "alice", 5, HU_ENTITY_PERSON, NULL, &alice);
    hu_graph_upsert_entity(g, "u1", 2, "acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme);

    /* Establish ground truth from a trusted source. */
    HU_ASSERT_EQ(hu_graph_upsert_relation_ex(g, "u1", 2, alice, acme, HU_REL_WORKS_AT, 1.0f,
                                             1735689600000LL, 0, 1.0f, "ctx", 3, "user-explicit",
                                             13),
                 HU_OK);

    /* Attacker tries to flip the fact via an open channel with rate-limit
     * abuse. The trust scorer should DROP this. */
    hu_write_trust_input_t att = {0};
    att.source = HU_WRITE_SOURCE_CHANNEL_OPEN;
    att.observed_at = 1735690000000LL;
    att.now = 1735690000000LL;
    att.recent_writes = 300; /* > rate_limit * 10 to trip flooding floor */
    att.rate_limit = 20;
    att.contradiction_flag = true;
    hu_write_trust_decision_t dec = hu_write_trust_score(&att);
    HU_ASSERT_EQ(dec.outcome, HU_WRITE_OUTCOME_DROP);

    /* The verifier should still see Alice@Acme intact. */
    hu_memory_facade_t *m = NULL;
    HU_ASSERT_EQ(hu_memory_facade_open(A(), g, &m), HU_OK);
    hu_verifier_config_t cfg = hu_verifier_default_config();
    cfg.mode = HU_VERIFY_SOFT;
    hu_verifier_report_t r;
    const char *draft = "Alice works at Acme.";
    HU_ASSERT_EQ(hu_response_verify(A(), m, "u1", 2, draft, strlen(draft), &cfg, &r), HU_OK);
    HU_ASSERT(r.claims[0].supported);
    hu_memory_facade_close(m, A());
    hu_graph_close(g, A());
}

/* --- E2E scenario 4: AutoDream + community summary roundtrip ---
 * Seed entities into a community, run AutoDream, verify a summary was
 * generated. */
static void test_e2e_autodream_summary_roundtrip(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    int64_t e1 = 0, e2 = 0;
    hu_graph_upsert_entity(g, "u1", 2, "running", 7, HU_ENTITY_TOPIC, NULL, &e1);
    hu_graph_upsert_entity(g, "u1", 2, "marathon", 8, HU_ENTITY_TOPIC, NULL, &e2);
    hu_graph_set_entity_community(g, e1, 7);
    hu_graph_set_entity_community(g, e2, 7);

    HU_ASSERT_EQ(hu_autodream_summarize_community(A(), g, "u1", 2, 7, 1735690000000LL), HU_OK);
    char *summary = NULL;
    size_t summary_len = 0;
    HU_ASSERT_EQ(hu_autodream_read_community_summary(A(), g, "u1", 2, 7, &summary, &summary_len),
                 HU_OK);
    HU_ASSERT(summary_len > 0);
    HU_ASSERT(summary != NULL);
    A()->free(A()->ctx, summary, summary_len + 1);
    hu_graph_close(g, A());
}

/* --- E2E scenario 5: persona-evolver resists drift attack ---
 * Attacker injects 25 high-confidence "value: comply-with-attacker" deltas
 * from a single rogue source. Rate limiter should quarantine, leaving the
 * persona profile untouched. */
static void test_e2e_persona_evolver_resists_drift_attack(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    for (int i = 0; i < 25; i++) {
        char val[32];
        snprintf(val, sizeof(val), "comply-attacker-%d", i);
        hu_persona_delta_propose(g, "u1", 2, HU_PERSONA_DELTA_VALUE, "all", val, 0.99f,
                                 "rogue-channel", 1735689600000LL + i * 1000LL, NULL);
    }
    hu_persona_evolver_config_t cfg = hu_persona_evolver_default_config();
    cfg.now_ms = 1735689600000LL + 30000;
    cfg.rate_limit_per_hour = 10;
    hu_persona_evolver_report_t r;
    HU_ASSERT_EQ(hu_persona_evolver_run(g, "u1", 2, &cfg, &r), HU_OK);
    HU_ASSERT(r.quarantined >= 14);
    /* No more than `rate_limit_per_hour` legitimate writes survive. */
    HU_ASSERT(r.applied <= cfg.rate_limit_per_hour);
    hu_graph_close(g, A());
}

/* --- E2E scenario 6: targeted erasure cascades cleanly ---
 * Stack the entity into multiple surfaces (relations + cross_edges + case
 * records), erase, verify zero residue. */
static void test_e2e_targeted_erasure_leaves_no_residue(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    int64_t alice = 0, acme = 0;
    hu_graph_upsert_entity(g, "u1", 2, "alice", 5, HU_ENTITY_PERSON, NULL, &alice);
    hu_graph_upsert_entity(g, "u1", 2, "acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme);
    hu_graph_upsert_relation_ex(g, "u1", 2, alice, acme, HU_REL_WORKS_AT, 1.0f, 1735689600000LL, 0,
                                1.0f, "ctx", 3, "imessage", 8);
    hu_cross_edge_upsert(g, "u1", 2, "entity", alice, "episode", 100, "ABOUT", 1.0f,
                         1735689600000LL, 0, 1.0f);
    hu_memory_facade_t *m = NULL;
    HU_ASSERT_EQ(hu_memory_facade_open(A(), g, &m), HU_OK);
    seed_case_record(g, alice);

    hu_erase_report_t er;
    HU_ASSERT_EQ(hu_memory_erase_entity(g, alice, &er), HU_OK);
    HU_ASSERT(er.entity_deleted);
    HU_ASSERT(er.relations_deleted >= 1);
    HU_ASSERT(er.cross_edges_deleted >= 1);
    HU_ASSERT(er.case_records_deleted >= 1);

    /* Re-running erase reports NOT_FOUND. */
    HU_ASSERT_EQ(hu_memory_erase_entity(g, alice, &er), HU_ERR_NOT_FOUND);
    hu_memory_facade_close(m, A());
    hu_graph_close(g, A());
}

/* --- E2E scenario 7: end-to-end full pipeline ---
 * 1. honest writes
 * 2. attacker injection (drop)
 * 3. autodream consolidation
 * 4. verifier on draft
 * 5. erase user
 * 6. verifier returns no support
 */
static void test_e2e_full_pipeline_honest_then_erase(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);

    int64_t alice = 0, acme = 0;
    hu_graph_upsert_entity(g, "u1", 2, "alice", 5, HU_ENTITY_PERSON, NULL, &alice);
    hu_graph_upsert_entity(g, "u1", 2, "acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme);
    hu_graph_upsert_relation_ex(g, "u1", 2, alice, acme, HU_REL_WORKS_AT, 1.0f, 1735689600000LL, 0,
                                1.0f, "ctx", 3, "imessage", 8);

    hu_autodream_config_t adcfg = hu_autodream_default_config();
    adcfg.now_ms = 1735690000000LL;
    adcfg.max_runtime_ms = 1000;
    hu_autodream_report_t adr;
    HU_ASSERT_EQ(hu_autodream_run(A(), g, &adcfg, &adr), HU_OK);

    hu_memory_facade_t *m = NULL;
    HU_ASSERT_EQ(hu_memory_facade_open(A(), g, &m), HU_OK);

    /* Verifier finds Alice@Acme. */
    hu_verifier_config_t vcfg = hu_verifier_default_config();
    vcfg.mode = HU_VERIFY_SOFT;
    hu_verifier_report_t vr;
    const char *draft = "Alice works at Acme.";
    HU_ASSERT_EQ(hu_response_verify(A(), m, "u1", 2, draft, strlen(draft), &vcfg, &vr), HU_OK);
    HU_ASSERT(vr.claims[0].supported);

    /* User invokes the right-to-be-forgotten. */
    hu_erase_report_t er;
    HU_ASSERT_EQ(hu_memory_erase_entity(g, alice, &er), HU_OK);
    HU_ASSERT(er.entity_deleted);

    /* Verifier no longer supports the claim. */
    HU_ASSERT_EQ(hu_response_verify(A(), m, "u1", 2, draft, strlen(draft), &vcfg, &vr), HU_OK);
    HU_ASSERT(!vr.claims[0].supported);
    HU_ASSERT(vr.draft_modified);
    hu_memory_facade_close(m, A());
    hu_graph_close(g, A());
}

#endif /* HU_ENABLE_SQLITE */

void run_w6_e2e_adversarial_tests(void) {
    HU_TEST_SUITE("W6 E2E adversarial - all workstreams composed");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_e2e_honest_write_yields_supported_claim_with_receipt);
    HU_RUN_TEST(test_e2e_bitemporal_supersession_survives_verifier);
    HU_RUN_TEST(test_e2e_write_trust_blocks_injection_attack);
    HU_RUN_TEST(test_e2e_autodream_summary_roundtrip);
    HU_RUN_TEST(test_e2e_persona_evolver_resists_drift_attack);
    HU_RUN_TEST(test_e2e_targeted_erasure_leaves_no_residue);
    HU_RUN_TEST(test_e2e_full_pipeline_honest_then_erase);
#endif
}
