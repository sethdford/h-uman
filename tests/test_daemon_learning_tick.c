/**
 * test_daemon_learning_tick.c — US-104 wiring: daemon-side proactive-outcome
 * pipeline (record_send / mark_reply / tick) feeding the humanization bandit.
 *
 * The SQL layer is pinned by tests/test_proactive_outcomes.c; these tests pin
 * the daemon wiring layer end-to-end: a recorded proactive send + an inbound
 * reply + one tick must move the contact's Beta(α,β) arm off the (1,1) prior.
 */

#include "human/daemon_learning_tick.h"
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include "human/agent/contextual_bandit.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/ml/dpo.h"
#include <sqlite3.h>
#include <string.h>
#include <time.h>

/* Fixture: sqlite-backed hu_memory_t with the dpo tables initialized (the
 * daemon gets this via agent sota init; see src/agent/agent.c). */
static hu_memory_t test_make_memory(hu_allocator_t *alloc, hu_dpo_collector_t *collector) {
    hu_memory_t mem = hu_sqlite_memory_create(alloc, ":memory:");
    if (!mem.ctx)
        return mem;
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    if (db && hu_dpo_collector_create(alloc, db, 10000, collector) == HU_OK)
        (void)hu_dpo_init_tables(collector);
    return mem;
}

static void test_record_send_inserts_pending_row(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t collector;
    hu_memory_t mem = test_make_memory(&alloc, &collector);
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_error_t err = hu_daemon_proactive_outcome_record_send(&mem, "imessage", "+15551234567", 12);
    HU_ASSERT_EQ(err, HU_OK);

    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT channel, contact, message_ref, outcome_type "
                                "FROM proactive_sends",
                                -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT(sqlite3_step(stmt) == SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "imessage");
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "+15551234567");
    HU_ASSERT_EQ(sqlite3_column_type(stmt, 2), SQLITE_NULL); /* no per-message ref */
    HU_ASSERT_EQ(sqlite3_column_type(stmt, 3), SQLITE_NULL); /* outcome pending */
    sqlite3_finalize(stmt);

    hu_dpo_collector_deinit(&collector);
    mem.vtable->deinit(mem.ctx);
}

static void test_mark_reply_resolves_pending_row_without_ref(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t collector;
    hu_memory_t mem = test_make_memory(&alloc, &collector);
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_daemon_proactive_outcome_record_send(&mem, "imessage", "+15551234567", 12),
                 HU_OK);
    /* Inbound from the same contact — the NULL-message_ref wildcard must
     * resolve the pending row (a bound NULL in `message_ref = ?` would
     * match nothing; pinned here so the wiring can't silently no-op). */
    HU_ASSERT_EQ(hu_daemon_proactive_outcome_mark_reply(&mem, "imessage", "+15551234567", 12),
                 HU_OK);

    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db, "SELECT outcome_type FROM proactive_sends WHERE contact = '+15551234567'", -1, &stmt,
        NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT(sqlite3_step(stmt) == SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_type(stmt, 0), SQLITE_INTEGER);
    HU_ASSERT_EQ(sqlite3_column_int(stmt, 0), (int)HU_BANDIT_REPLY);
    sqlite3_finalize(stmt);

    hu_dpo_collector_deinit(&collector);
    mem.vtable->deinit(mem.ctx);
}

static void test_mark_reply_other_contact_leaves_row_pending(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t collector;
    hu_memory_t mem = test_make_memory(&alloc, &collector);
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_daemon_proactive_outcome_record_send(&mem, "imessage", "+15551234567", 12),
                 HU_OK);
    HU_ASSERT_EQ(hu_daemon_proactive_outcome_mark_reply(&mem, "imessage", "+19998887777", 12),
                 HU_OK);

    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db, "SELECT COUNT(*) FROM proactive_sends WHERE outcome_type IS NULL", -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT(sqlite3_step(stmt) == SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(stmt, 0), 1); /* still pending */
    sqlite3_finalize(stmt);

    hu_dpo_collector_deinit(&collector);
    mem.vtable->deinit(mem.ctx);
}

/* The headline contract: send → reply → tick moves the contact's arm off
 * the Beta(1,1) prior, addressed by the SAME hash the daemon read path
 * uses (hu_contact_handle_hash of the reply-path session key). */
static void test_tick_moves_bandit_arm_from_default_prior(void) {
    hu_daemon_learning_tick_reset_for_test();

    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t collector;
    hu_memory_t mem = test_make_memory(&alloc, &collector);
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_contextual_bandit_t *bandit = NULL;
    HU_ASSERT_EQ(hu_contextual_bandit_create(&alloc, 16, &bandit), HU_OK);

    uint64_t handle = hu_contact_handle_hash("+15551234567");
    hu_contextual_bandit_arm_t before;
    memset(&before, 0, sizeof(before));
    HU_ASSERT_EQ(hu_contextual_bandit_get_arm(bandit, handle, &before), HU_OK);
    HU_ASSERT(before.alpha == 1.0 && before.beta == 1.0); /* weak prior */

    HU_ASSERT_EQ(hu_daemon_proactive_outcome_record_send(&mem, "imessage", "+15551234567", 12),
                 HU_OK);
    HU_ASSERT_EQ(hu_daemon_proactive_outcome_mark_reply(&mem, "imessage", "+15551234567", 12),
                 HU_OK);
    HU_ASSERT_EQ(hu_daemon_proactive_outcome_tick(&mem, bandit, (int64_t)time(NULL)), HU_OK);

    hu_contextual_bandit_arm_t after;
    memset(&after, 0, sizeof(after));
    HU_ASSERT_EQ(hu_contextual_bandit_get_arm(bandit, handle, &after), HU_OK);
    HU_ASSERT(after.alpha == 2.0); /* REPLY: α 1 → 2 */
    HU_ASSERT(after.beta == 1.0);
    HU_ASSERT(after.updates == 1);

    /* And the row is consumed — a second tick must not double-count. */
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM proactive_sends WHERE processed = 0", -1,
                                &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT(sqlite3_step(stmt) == SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);

    hu_contextual_bandit_destroy(bandit);
    hu_dpo_collector_deinit(&collector);
    mem.vtable->deinit(mem.ctx);
}

static void test_tick_rate_limits_to_interval(void) {
    hu_daemon_learning_tick_reset_for_test();

    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t collector;
    hu_memory_t mem = test_make_memory(&alloc, &collector);
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_contextual_bandit_t *bandit = NULL;
    HU_ASSERT_EQ(hu_contextual_bandit_create(&alloc, 16, &bandit), HU_OK);

    int64_t now = (int64_t)time(NULL);
    HU_ASSERT_EQ(hu_daemon_proactive_outcome_tick(&mem, bandit, now), HU_OK);

    /* Resolve an outcome AFTER the first tick; a tick 30s later must be a
     * cadence no-op (row stays unprocessed), 61s later it must run. */
    HU_ASSERT_EQ(hu_daemon_proactive_outcome_record_send(&mem, "imessage", "+15551234567", 12),
                 HU_OK);
    HU_ASSERT_EQ(hu_daemon_proactive_outcome_mark_reply(&mem, "imessage", "+15551234567", 12),
                 HU_OK);

    HU_ASSERT_EQ(hu_daemon_proactive_outcome_tick(&mem, bandit, now + 30), HU_OK);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM proactive_sends WHERE processed = 0", -1,
                                &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT(sqlite3_step(stmt) == SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(stmt, 0), 1); /* untouched within cadence */
    sqlite3_finalize(stmt);

    HU_ASSERT_EQ(hu_daemon_proactive_outcome_tick(&mem, bandit, now + 61), HU_OK);
    rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM proactive_sends WHERE processed = 0", -1,
                            &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT(sqlite3_step(stmt) == SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);

    hu_contextual_bandit_destroy(bandit);
    hu_dpo_collector_deinit(&collector);
    mem.vtable->deinit(mem.ctx);
}

static void test_tick_without_bandit_is_safe_noop(void) {
    hu_daemon_learning_tick_reset_for_test();

    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t collector;
    hu_memory_t mem = test_make_memory(&alloc, &collector);
    HU_ASSERT_NOT_NULL(mem.ctx);

    /* NULL bandit (sota off / creation failed) must not crash or error —
     * the silent-config rule's log-once path. */
    HU_ASSERT_EQ(hu_daemon_proactive_outcome_tick(&mem, NULL, (int64_t)time(NULL)), HU_OK);
    HU_ASSERT_EQ(hu_daemon_proactive_outcome_tick(NULL, NULL, (int64_t)time(NULL)), HU_OK);

    hu_dpo_collector_deinit(&collector);
    mem.vtable->deinit(mem.ctx);
}

static void test_record_send_null_inputs_are_noop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t collector;
    hu_memory_t mem = test_make_memory(&alloc, &collector);
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_daemon_proactive_outcome_record_send(NULL, "imessage", "x", 1), HU_OK);
    HU_ASSERT_EQ(hu_daemon_proactive_outcome_record_send(&mem, NULL, "x", 1), HU_OK);
    HU_ASSERT_EQ(hu_daemon_proactive_outcome_record_send(&mem, "imessage", NULL, 0), HU_OK);
    HU_ASSERT_EQ(hu_daemon_proactive_outcome_mark_reply(&mem, "imessage", NULL, 0), HU_OK);

    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM proactive_sends", -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT(sqlite3_step(stmt) == SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);

    hu_dpo_collector_deinit(&collector);
    mem.vtable->deinit(mem.ctx);
}

static void test_dpo_judge_tick_null_agent_is_safe(void) {
    /* First call arms the deferral timer; the second (still NULL agent)
     * must skip without touching a provider. Crash-free is the contract. */
    hu_daemon_dpo_judge_tick(NULL, NULL, 1000);
    hu_daemon_dpo_judge_tick(NULL, NULL, 1000 + 25 * 3600);
}

void run_daemon_learning_tick_tests(void) {
    HU_TEST_SUITE("daemon_learning_tick");
    HU_RUN_TEST(test_record_send_inserts_pending_row);
    HU_RUN_TEST(test_mark_reply_resolves_pending_row_without_ref);
    HU_RUN_TEST(test_mark_reply_other_contact_leaves_row_pending);
    HU_RUN_TEST(test_tick_moves_bandit_arm_from_default_prior);
    HU_RUN_TEST(test_tick_rate_limits_to_interval);
    HU_RUN_TEST(test_tick_without_bandit_is_safe_noop);
    HU_RUN_TEST(test_record_send_null_inputs_are_noop);
    HU_RUN_TEST(test_dpo_judge_tick_null_agent_is_safe);
}

#else /* !HU_ENABLE_SQLITE */

void run_daemon_learning_tick_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
