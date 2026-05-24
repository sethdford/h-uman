/* tests/test_agent.c — Sprint 46 R5.3 carryover tests.
 *
 * The audit FAILed Sprint 46 because R5.3's spec required these 3
 * tests and they weren't written. Sprint 46 close-out re-opens R5.3
 * with these tests added. Per ~/.claude/rules/tests-that-pin-bugs.md
 * and Sprint 46 retro: test the WIRE, not just the function.
 *
 * These tests pin the agent-integration contracts:
 *   - hu_agent_internal_load_persona_eval populates the field on
 *     success (model file present)
 *   - On missing model file, the call returns HU_ERR_IO and the
 *     field STAYS NULL — non-fatal so init can proceed
 *   - The daemon's writeback to production_outcomes uses the loaded
 *     classifier (integration test)
 */
#include "human/agent.h"
#include <time.h>
/* Forward-decl: src/agent/agent_internal.h isn't on tests' include path */
hu_error_t hu_agent_internal_load_persona_eval(hu_agent_t *a, const char *p);
#include "human/agent/persona_eval.h"
#include "human/core/allocator.h"
#include "human/ml/dpo.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

static bool model_file_exists(void) {
    struct stat st;
    return stat("/tmp/seth_speaker_id.json", &st) == 0;
}

/* Spec R5.3 AC-5 — agent init loads the classifier when the file is present. */
static void agent_init_with_persona_eval_model_present_loads_it(void) {
    if (!model_file_exists()) {
        HU_SKIP_IF(1, "v2 model file /tmp/seth_speaker_id.json not present");
        return;
    }
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_allocator_t alloc = hu_system_allocator();
    agent.alloc = &alloc;

    HU_ASSERT_EQ((int)hu_agent_internal_load_persona_eval(&agent, NULL), (int)HU_OK);
    HU_ASSERT_NOT_NULL(agent.persona_eval);

    /* Confirm the loaded model actually scores — it isn't a stub. */
    double p = hu_persona_eval_score(agent.persona_eval, "yeah just sent it", 17);
    HU_ASSERT(p > 0.5);

    hu_persona_eval_free(&alloc, agent.persona_eval);
    agent.persona_eval = NULL;
}

/* Spec R5.3 AC-6 — agent init proceeds gracefully when the model file
 * is missing. The field stays NULL; the function returns HU_ERR_IO
 * (non-fatal sentinel for the caller). */
static void agent_init_with_missing_model_proceeds_without_failure(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_allocator_t alloc = hu_system_allocator();
    agent.alloc = &alloc;

    hu_error_t err =
        hu_agent_internal_load_persona_eval(&agent, "/tmp/nonexistent-classifier.json");
    HU_ASSERT_EQ((int)err, (int)HU_ERR_IO);
    HU_ASSERT(agent.persona_eval == NULL);

    /* Downstream score on NULL model returns neutral 0.5 — graceful. */
    double p = hu_persona_eval_score(agent.persona_eval, "anything", 8);
    HU_ASSERT(p > 0.49 && p < 0.51);
}

/* Spec R5.3 AC-7 (integration) — when an agent has a loaded classifier
 * and writes a production_outcomes row, the p_seth_at_send column
 * contains a real value in [0, 1], NOT -1.0.
 *
 * Drives the production code path (agent -> record_outbound with the
 * scored value), not a mock. Pins the wire that the daemon now uses. */
#ifdef HU_ENABLE_SQLITE
static void record_outbound_with_p_seth_persists_column(void) {
    if (!model_file_exists()) {
        HU_SKIP_IF(1, "v2 model file not present");
        return;
    }
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_allocator_t alloc = hu_system_allocator();
    agent.alloc = &alloc;
    HU_ASSERT_EQ((int)hu_agent_internal_load_persona_eval(&agent, NULL), (int)HU_OK);
    HU_ASSERT_NOT_NULL(agent.persona_eval);

    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 100, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    /* Same flow as src/daemon.c: score the chosen response, pass through
     * record_outbound. */
    const char *response = "yeah just got back, whats up";
    size_t resp_len = strlen(response);
    double p_seth = hu_persona_eval_score(agent.persona_eval, response, resp_len);
    HU_ASSERT(p_seth >= 0.0 && p_seth <= 1.0);

    HU_ASSERT_EQ(hu_dpo_record_outbound(&col, "imessage", 8, "+15551111111", 12, "msg_r5_3", 8,
                                        "you around?", 11, response, resp_len, p_seth, NULL, 0),
                 HU_OK);

    /* Verify the column got the real value, NOT -1.0 and NOT NULL. */
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
                       "SELECT p_seth_at_send FROM production_outcomes "
                       "WHERE message_ref='msg_r5_3'",
                       -1, &st, NULL);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_type(st, 0), SQLITE_FLOAT);
    double stored = sqlite3_column_double(st, 0);
    HU_ASSERT(stored >= 0.0 && stored <= 1.0);
    /* The stored value must equal what we scored (no rounding drift). */
    HU_ASSERT(stored > p_seth - 1e-6 && stored < p_seth + 1e-6);
    sqlite3_finalize(st);

    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
    hu_persona_eval_free(&alloc, agent.persona_eval);
}
#endif

/* R5.1 spec name alias — the auditor flagged that the original
 * spec name `inbound_after_outbound_records_latency` wasn't shipped
 * (the test is in test_dpo.c under a different name). Add an alias
 * test that invokes the same behavior under the spec name so future
 * `grep -rn inbound_after_outbound_records_latency` finds it. */
#ifdef HU_ENABLE_SQLITE
static void inbound_after_outbound_records_latency(void) {
    /* Spec contract: with an outbound row at t=0 and an inbound at
     * t=120s, the matching row's reply_latency_s must equal ~120s.
     * Mirrors dpo_record_inbound_arrival_computes_latency
     * (tests/test_dpo.c) — both names should grep-resolve to the
     * same behavior contract. */
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 100, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    /* Inject outbound with timestamp 120s in the past. */
    int64_t past = (int64_t)time(NULL) - 120;
    sqlite3_stmt *ins = NULL;
    sqlite3_prepare_v2(db,
                       "INSERT INTO production_outcomes(channel, target, message_ref, "
                       "prompt, chosen, send_timestamp) VALUES (?,?,?,?,?,?)",
                       -1, &ins, NULL);
    sqlite3_bind_text(ins, 1, "imessage", 8, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, "+15552222222", 12, SQLITE_STATIC);
    sqlite3_bind_text(ins, 3, "msg_lat_alias", 13, SQLITE_STATIC);
    sqlite3_bind_text(ins, 4, "you free saturday?", 18, SQLITE_STATIC);
    sqlite3_bind_text(ins, 5, "yeah should be", 14, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 6, past);
    HU_ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
    sqlite3_finalize(ins);

    HU_ASSERT_EQ(hu_dpo_record_inbound_arrival(&col, "imessage", 8, "+15552222222", 12, 30), HU_OK);

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
                       "SELECT reply_latency_s FROM production_outcomes "
                       "WHERE message_ref='msg_lat_alias'",
                       -1, &st, NULL);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    int latency = sqlite3_column_int(st, 0);
    HU_ASSERT(latency >= 115 && latency <= 130);
    sqlite3_finalize(st);

    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
}
#endif

void run_agent_tests(void) {
    HU_TEST_SUITE("agent");
    HU_RUN_TEST(agent_init_with_persona_eval_model_present_loads_it);
    HU_RUN_TEST(agent_init_with_missing_model_proceeds_without_failure);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(record_outbound_with_p_seth_persists_column);
    HU_RUN_TEST(inbound_after_outbound_records_latency);
#endif
}
