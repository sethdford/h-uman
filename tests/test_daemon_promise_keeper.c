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

static void courtesy_predicate_rejects_bare_invitations(void) {
    /* The 5 courtesy phrases actually captured by the SHADOW stream
     * (service-loop-error.log, 2026-07-19) — bare invitations, no deliverable,
     * no deadline. These must be filtered or shadow precision stays ~38%. */
    static const char *const courtesy_phrases[] = {
        "let me know",
        "let me know how I can better assist you",
        "let me know how I can help you here",
        "let me know when you're available",
        "let me know which part was offensive so I can correct it",
    };
    for (size_t i = 0; i < sizeof(courtesy_phrases) / sizeof(courtesy_phrases[0]); i++)
        HU_ASSERT_TRUE(hu_promise_keeper_is_courtesy_invitation(courtesy_phrases[i],
                                                                strlen(courtesy_phrases[i]), 0));
}

static void courtesy_predicate_accepts_genuine_commitments(void) {
    /* The genuine commitments from the same SHADOW stream — concrete
     * first-person deliverables that must keep flowing to the ledger. */
    static const char *const genuine_phrases[] = {
        "I'll send that list over in a bit",
        "gonna try and make some rn",
        "I will call you as soon as I finish",
    };
    for (size_t i = 0; i < sizeof(genuine_phrases) / sizeof(genuine_phrases[0]); i++)
        HU_ASSERT_FALSE(hu_promise_keeper_is_courtesy_invitation(genuine_phrases[i],
                                                                 strlen(genuine_phrases[i]), 0));
}

static void courtesy_predicate_word_boundary_and_escape_hatches(void) {
    /* Word boundary: "know" must not match inside a longer word
     * (substring-classifier-pitfalls rule). */
    static const char wb[] = "let me knowledge-share the plan";
    HU_ASSERT_FALSE(hu_promise_keeper_is_courtesy_invitation(wb, sizeof(wb) - 1, 0));
    /* Prefix-anchored: an invitation tail after a real commitment is fine. */
    static const char tail[] = "I'll send it over, let me know if it works";
    HU_ASSERT_FALSE(hu_promise_keeper_is_courtesy_invitation(tail, sizeof(tail) - 1, 0));
    /* A parsed deadline makes it concrete, not bare. */
    static const char dl[] = "let me know and we can sync tomorrow";
    HU_ASSERT_FALSE(hu_promise_keeper_is_courtesy_invitation(dl, sizeof(dl) - 1, 1752900000));
    /* An embedded first-person deliverable makes it concrete. */
    static const char emb[] = "let me know your address and i'll ship the package";
    HU_ASSERT_FALSE(hu_promise_keeper_is_courtesy_invitation(emb, sizeof(emb) - 1, 0));
    /* Case-insensitive on the prefix. */
    static const char caps[] = "Let Me Know whenever";
    HU_ASSERT_TRUE(hu_promise_keeper_is_courtesy_invitation(caps, sizeof(caps) - 1, 0));
    /* NULL / empty are not courtesy (nothing to reject). */
    HU_ASSERT_FALSE(hu_promise_keeper_is_courtesy_invitation(NULL, 0, 0));
    HU_ASSERT_FALSE(hu_promise_keeper_is_courtesy_invitation("", 0, 0));
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
                                                        sizeof(reply) - 1, HU_PROMISE_KEEPER_LIVE,
                                                        NULL, &stored),
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
                                                        sizeof(reply) - 1, HU_PROMISE_KEEPER_LIVE,
                                                        NULL, &stored),
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
                                                        sizeof(reply) - 1, HU_PROMISE_KEEPER_SHADOW,
                                                        NULL, &stored),
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
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "contact_a", 9, "call back", 9, "me",
                                                2, now_ts - 3600),
                 HU_OK);
    /* pending: future deadline + no deadline */
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "contact_a", 9, "plan trip", 9, "me",
                                                2, now_ts + 86400),
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
                                                        sizeof(reply) - 1, HU_PROMISE_KEEPER_LIVE,
                                                        NULL, &stored),
                 HU_OK);
    /* The commitment MUST be stored even if schedule encounters an error. */
    HU_ASSERT_TRUE(stored);
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM commitments WHERE who='me' "
                                "AND status='pending' AND contact_id='contact_a'"),
                 1);
    /* The scan returns HU_OK even if schedule fails (error is logged, not propagated). */

    mem.vtable->deinit(mem.ctx);
}

static void promise_keeper_live_rejects_courtesy_invitation(void) {
    /* End-to-end through the real scan path: a reply whose only detected
     * "commitment" is a courtesy invitation must not reach the ledger.
     * Non-vacuous: detect_commitment DOES fire on "let me" (see the shadow
     * log), so without the filter this reply would store a row. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM commitments"), 0);

    static const char reply[] = "let me know how I can better assist you";
    bool stored = true;
    HU_ASSERT_EQ(hu_daemon_promise_keeper_scan_outbound(&mem, &alloc, "contact_a", 9, reply,
                                                        sizeof(reply) - 1, HU_PROMISE_KEEPER_LIVE,
                                                        NULL, &stored),
                 HU_OK);
    HU_ASSERT_FALSE(stored);
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM commitments"), 0);
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM delayed_followups"), 0);

    mem.vtable->deinit(mem.ctx);
}

#endif /* HU_ENABLE_SQLITE */

void run_daemon_promise_keeper_tests(void) {
    HU_TEST_SUITE("DaemonPromiseKeeper");
    HU_RUN_TEST(promise_keeper_mode_from_env_truth_table);
    HU_RUN_TEST(courtesy_predicate_rejects_bare_invitations);
    HU_RUN_TEST(courtesy_predicate_accepts_genuine_commitments);
    HU_RUN_TEST(courtesy_predicate_word_boundary_and_escape_hatches);
    HU_RUN_TEST(promise_keeper_off_mode_is_noop);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(promise_keeper_live_stores_me_commitment);
    HU_RUN_TEST(promise_keeper_live_ignores_non_commitment);
    HU_RUN_TEST(promise_keeper_shadow_logs_without_storing);
    HU_RUN_TEST(promise_keeper_ledger_stats_buckets);
    HU_RUN_TEST(promise_keeper_live_handles_schedule_error_gracefully);
    HU_RUN_TEST(promise_keeper_live_rejects_courtesy_invitation);
#endif
}
