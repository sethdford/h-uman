/* Pins hu_daemon_proactive_record_decline — the attribution added 2026-09-20 so
 * a proactive check-in suppressed BEFORE any send attempt leaves a row saying
 * which gate suppressed it.
 *
 * Why this matters: scripts/eval_when_to_speak.py measures the FIR/MIR
 * calibration of the proactive policy and reported fir_dropped_pre_send=89
 * against 10 delivered sends. Those 89 FIRED proposals died in gates between
 * daemon.c:1573 and :1666 with no row naming the gate — visible only in the
 * service log, invisible to the metric. Without attribution the eval cannot
 * separate "the policy correctly stayed quiet" from "a rate-limiter ate it".
 *
 * Assertions are deliberately NON-VACUOUS (see tests-that-pin-bugs.md): each
 * asserts the row's FIELD VALUES, not merely that a row exists — a recorder
 * that wrote decision='send' or dropped the reason would pass a count check. */
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include "human/agent.h"
#include "human/daemon_proactive.h"
#include "human/memory.h"
#include "human/memory/engines.h"
#include "human/memory/proactive_decisions_repo.h"
#include <sqlite3.h>
#include <string.h>

/* Reads back the single row's text columns so assertions can be about VALUES. */
static int decline_row_matches(sqlite3 *db, const char *contact, const char *trigger,
                               const char *decision, const char *reason, int sent) {
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT contact, trigger, decision, reason, sent "
                      "FROM proactive_decisions ORDER BY id DESC LIMIT 1";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return 0;
    int ok = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *c = sqlite3_column_text(st, 0);
        const unsigned char *t = sqlite3_column_text(st, 1);
        const unsigned char *d = sqlite3_column_text(st, 2);
        const unsigned char *r = sqlite3_column_text(st, 3);
        int s = sqlite3_column_int(st, 4);
        ok = c && t && d && r && strcmp((const char *)c, contact) == 0 &&
             strcmp((const char *)t, trigger) == 0 && strcmp((const char *)d, decision) == 0 &&
             strcmp((const char *)r, reason) == 0 && s == sent;
    }
    sqlite3_finalize(st);
    return ok;
}

static int64_t decline_row_count(sqlite3 *db) {
    int64_t n = -1;
    (void)hu_proactive_decisions_repo_count(db, &n);
    return n;
}

static void test_record_decline_attributes_the_suppressing_gate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_ensure_schema(db), HU_OK);

    struct hu_agent agent = {0};
    agent.memory = &mem;

    /* Precondition: the table is empty, so a post-assert cannot pass vacuously. */
    HU_ASSERT_EQ(decline_row_count(db), 0);

    hu_daemon_proactive_record_decline(&agent, "+15555550100", "rate_limited", 1789000000);

    HU_ASSERT_EQ(decline_row_count(db), 1);
    /* The row must say WHICH gate, as a decline, on the send-outcome trigger,
     * and must NOT claim delivery. */
    HU_ASSERT_TRUE(decline_row_matches(db, "+15555550100", "proactive_send",
                                       HU_PROACTIVE_DECISION_DECLINE, "rate_limited",
                                       /*sent=*/0));
}

static void test_record_decline_distinguishes_two_gates(void) {
    /* Two different gates must produce two distinguishable rows — a recorder
     * that hardcoded one reason would pass the single-row test above. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_ensure_schema(db), HU_OK);

    struct hu_agent agent = {0};
    agent.memory = &mem;

    hu_daemon_proactive_record_decline(&agent, "+15555550100", "sanitize_refused", 1789000001);
    HU_ASSERT_TRUE(decline_row_matches(db, "+15555550100", "proactive_send",
                                       HU_PROACTIVE_DECISION_DECLINE, "sanitize_refused", 0));

    hu_daemon_proactive_record_decline(&agent, "+15555550199", "send_cap", 1789000002);
    HU_ASSERT_TRUE(decline_row_matches(db, "+15555550199", "proactive_send",
                                       HU_PROACTIVE_DECISION_DECLINE, "send_cap", 0));

    HU_ASSERT_EQ(decline_row_count(db), 2);
}

static void test_record_decline_writes_nothing_on_null_inputs(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_ensure_schema(db), HU_OK);

    struct hu_agent agent = {0};
    agent.memory = &mem;

    hu_daemon_proactive_record_decline(NULL, "+1555", "rate_limited", 1789000000);
    hu_daemon_proactive_record_decline(&agent, NULL, "rate_limited", 1789000000);
    hu_daemon_proactive_record_decline(&agent, "+1555", NULL, 1789000000);

    /* Telemetry must never invent a row it cannot describe. */
    HU_ASSERT_EQ(decline_row_count(db), 0);
}

void run_daemon_proactive_decline_tests(void) {
    HU_TEST_SUITE("daemon_proactive_decline");
    HU_RUN_TEST(test_record_decline_attributes_the_suppressing_gate);
    HU_RUN_TEST(test_record_decline_distinguishes_two_gates);
    HU_RUN_TEST(test_record_decline_writes_nothing_on_null_inputs);
}

#else

void run_daemon_proactive_decline_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
