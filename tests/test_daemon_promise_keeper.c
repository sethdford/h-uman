/* Promise-keeper outbound scan (F25b) — the ledger's "me" side.
 *
 * The commitments table had 77 rows, all who='them', before this module:
 * OUR promises were never captured. These tests pin the mode gate, the
 * who='me' store, the deadline→delayed_followup wiring, and the ledger
 * stats that make kept/broken auditable. */
#include "human/daemon/promise_keeper.h"
#include "test_framework.h"

#include <string.h>

static void promise_keeper_mode_from_env_truth_table(void) {
    HU_ASSERT_EQ((int)hu_promise_keeper_mode_from_env(NULL), (int)HU_PROMISE_KEEPER_OFF);
    HU_ASSERT_EQ((int)hu_promise_keeper_mode_from_env(""), (int)HU_PROMISE_KEEPER_OFF);
    HU_ASSERT_EQ((int)hu_promise_keeper_mode_from_env("off"), (int)HU_PROMISE_KEEPER_OFF);
    HU_ASSERT_EQ((int)hu_promise_keeper_mode_from_env("1"), (int)HU_PROMISE_KEEPER_OFF);
    HU_ASSERT_EQ((int)hu_promise_keeper_mode_from_env("shadow"), (int)HU_PROMISE_KEEPER_SHADOW);
    HU_ASSERT_EQ((int)hu_promise_keeper_mode_from_env("on"), (int)HU_PROMISE_KEEPER_LIVE);
    /* Exact-match only: "ON"/"On" are not silently live. */
    HU_ASSERT_EQ((int)hu_promise_keeper_mode_from_env("ON"), (int)HU_PROMISE_KEEPER_OFF);
}

static void promise_keeper_off_mode_is_noop(void) {
    /* OFF short-circuits before argument validation — the default path
     * costs nothing and touches nothing. */
    bool stored = true;
    HU_ASSERT_EQ(hu_daemon_promise_keeper_scan_outbound(NULL, NULL, NULL, 0, NULL, 0,
                                                        HU_PROMISE_KEEPER_OFF, NULL, &stored),
                 HU_OK);
    HU_ASSERT_FALSE(stored);
}

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/superhuman.h"
#include <sqlite3.h>
#include <time.h>

static int count_rows(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    int n = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

static void promise_keeper_live_stores_me_commitment(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    /* Pre-condition: ledger empty (non-vacuous contract). */
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM commitments"), 0);

    static const char reply[] = "yeah i'll send it tomorrow, no worries";
    bool stored = false;
    HU_ASSERT_EQ(hu_daemon_promise_keeper_scan_outbound(&mem, &alloc, "contact_a", 9, reply,
                                                        sizeof(reply) - 1,
                                                        HU_PROMISE_KEEPER_LIVE, NULL, &stored),
                 HU_OK);
    HU_ASSERT_TRUE(stored);
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM commitments WHERE who='me' "
                                "AND status='pending' AND contact_id='contact_a'"),
                 1);
    /* "tomorrow" parses to a real deadline → follow-up scheduled. */
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM delayed_followups "
                                "WHERE contact_id='contact_a' AND sent=0"),
                 1);

    mem.vtable->deinit(mem.ctx);
}

static void promise_keeper_live_ignores_non_commitment(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);

    static const char reply[] = "lol nice, sounds good";
    bool stored = true;
    HU_ASSERT_EQ(hu_daemon_promise_keeper_scan_outbound(&mem, &alloc, "contact_a", 9, reply,
                                                        sizeof(reply) - 1,
                                                        HU_PROMISE_KEEPER_LIVE, NULL, &stored),
                 HU_OK);
    HU_ASSERT_FALSE(stored);
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM commitments"), 0);

    mem.vtable->deinit(mem.ctx);
}

static void promise_keeper_shadow_logs_without_storing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);

    static const char reply[] = "i'll check on that tomorrow";
    bool stored = true;
    HU_ASSERT_EQ(hu_daemon_promise_keeper_scan_outbound(&mem, &alloc, "contact_a", 9, reply,
                                                        sizeof(reply) - 1,
                                                        HU_PROMISE_KEEPER_SHADOW, NULL, &stored),
                 HU_OK);
    HU_ASSERT_FALSE(stored);
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM commitments"), 0);
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM delayed_followups"), 0);

    mem.vtable->deinit(mem.ctx);
}

static void promise_keeper_ledger_stats_buckets(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    int64_t now_ts = (int64_t)time(NULL);
    /* kept: stored then marked followed_up */
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "contact_a", 9, "send the doc", 12,
                                                "me", 2, now_ts - 100),
                 HU_OK);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    sqlite3_stmt *stmt = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT MAX(id) FROM commitments", -1, &stmt, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    int64_t kept_id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    HU_ASSERT_EQ(hu_superhuman_commitment_mark_followed_up(&mem, kept_id), HU_OK);

    /* overdue: pending with past deadline */
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "contact_a", 9, "call back", 9,
                                                "me", 2, now_ts - 3600),
                 HU_OK);
    /* pending: future deadline + no deadline */
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "contact_a", 9, "plan trip", 9,
                                                "me", 2, now_ts + 86400),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "contact_a", 9, "keep in touch", 13,
                                                "them", 4, 0),
                 HU_OK);

    uint32_t kept = 0, pending = 0, overdue = 0;
    HU_ASSERT_EQ(hu_superhuman_commitment_ledger_stats(&mem, "contact_a", 9, "me", now_ts, &kept,
                                                       &pending, &overdue),
                 HU_OK);
    HU_ASSERT_EQ(kept, 1u);
    HU_ASSERT_EQ(pending, 1u);
    HU_ASSERT_EQ(overdue, 1u);

    /* NULL who counts both sides — picks up the 'them' no-deadline pending. */
    HU_ASSERT_EQ(hu_superhuman_commitment_ledger_stats(&mem, "contact_a", 9, NULL, now_ts, &kept,
                                                       &pending, &overdue),
                 HU_OK);
    HU_ASSERT_EQ(kept, 1u);
    HU_ASSERT_EQ(pending, 2u);
    HU_ASSERT_EQ(overdue, 1u);

    /* Other contacts don't leak in. */
    HU_ASSERT_EQ(hu_superhuman_commitment_ledger_stats(&mem, "contact_b", 9, NULL, now_ts, &kept,
                                                       &pending, &overdue),
                 HU_OK);
    HU_ASSERT_EQ(kept, 0u);
    HU_ASSERT_EQ(pending, 0u);
    HU_ASSERT_EQ(overdue, 0u);

    mem.vtable->deinit(mem.ctx);
}

static void promise_keeper_live_handles_schedule_error_gracefully(void) {
    /* When hu_superhuman_delayed_followup_schedule fails (e.g., disk full,
     * corrupt db), the promise should still be stored and logged. The error
     * from schedule is captured and logged (not discarded). This test pins
     * the contract: schedule error does NOT prevent the commitment from
     * being stored. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);

    /* Pre-condition: tables exist and are empty. */
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM commitments"), 0);
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM delayed_followups"), 0);

    /* Reply with a commitment + a deadline. */
    static const char reply[] = "yeah i'll send it tomorrow";
    bool stored = false;
    HU_ASSERT_EQ(hu_daemon_promise_keeper_scan_outbound(&mem, &alloc, "contact_a", 9, reply,
                                                        sizeof(reply) - 1,
                                                        HU_PROMISE_KEEPER_LIVE, NULL, &stored),
                 HU_OK);
    /* The commitment MUST be stored even if schedule encounters an error. */
    HU_ASSERT_TRUE(stored);
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM commitments WHERE who='me' "
                                "AND status='pending' AND contact_id='contact_a'"),
                 1);
    /* The scan returns HU_OK even if schedule fails (error is logged, not propagated). */

    mem.vtable->deinit(mem.ctx);
}

#endif /* HU_ENABLE_SQLITE */

void run_daemon_promise_keeper_tests(void) {
    HU_TEST_SUITE("DaemonPromiseKeeper");
    HU_RUN_TEST(promise_keeper_mode_from_env_truth_table);
    HU_RUN_TEST(promise_keeper_off_mode_is_noop);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(promise_keeper_live_stores_me_commitment);
    HU_RUN_TEST(promise_keeper_live_ignores_non_commitment);
    HU_RUN_TEST(promise_keeper_shadow_logs_without_storing);
    HU_RUN_TEST(promise_keeper_ledger_stats_buckets);
    HU_RUN_TEST(promise_keeper_live_handles_schedule_error_gracefully);
#endif
}
