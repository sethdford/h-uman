/**
 * test_proactive_outcomes.c — US-104: Proactive outcome signal processing
 *
 * Tests the proactive_sends table, outcome insertion/update, and async
 * processor integration with the contextual bandit for learning.
 */

#include "human/ml/dpo.h"
#include "test_framework.h"
#ifdef HU_ENABLE_SQLITE
#include "human/agent/contextual_bandit.h"
#include <sqlite3.h>
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Test fixture: create an in-memory SQLite database for proactive_sends testing. */
static sqlite3 *test_create_db(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK || !db) {
        return NULL;
    }
    return db;
}

static void test_proactive_sends_table_created(void) {
    sqlite3 *db = test_create_db();
    if (!db)
        return;

    hu_allocator_t alloc;
    alloc = hu_system_allocator();

    hu_dpo_collector_t collector;
    hu_error_t err = hu_dpo_collector_create(&alloc, db, 10000, &collector);
    HU_ASSERT(err == HU_OK);

    err = hu_dpo_init_tables(&collector);
    HU_ASSERT(err == HU_OK);

    /* Query to verify table exists and has expected columns. */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT sql FROM sqlite_master WHERE type='table' "
                                "AND name='proactive_sends'",
                                -1, &stmt, NULL);
    HU_ASSERT(rc == SQLITE_OK);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = true;
    }
    sqlite3_finalize(stmt);
    HU_ASSERT(found);

    hu_dpo_collector_deinit(&collector);
    sqlite3_close(db);
}

/* Test inserting a proactive send row. */
static void test_proactive_insert_send(void) {
    sqlite3 *db = test_create_db();
    if (!db)
        return;

    hu_allocator_t alloc;
    alloc = hu_system_allocator();

    hu_dpo_collector_t collector;
    hu_error_t err = hu_dpo_collector_create(&alloc, db, 10000, &collector);
    HU_ASSERT(err == HU_OK);
    err = hu_dpo_init_tables(&collector);
    HU_ASSERT(err == HU_OK);

    /* Insert a proactive send. */
    err = hu_dpo_collector_insert_proactive_send(db, "imessage", 8, "alice@example.com", 17,
                                                 "msg-ref-123", 11);
    HU_ASSERT(err == HU_OK);

    /* Verify row was inserted. */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM proactive_sends", -1, &stmt, NULL);
    HU_ASSERT(rc == SQLITE_OK);
    HU_ASSERT(sqlite3_step(stmt) == SQLITE_ROW);
    int count = sqlite3_column_int(stmt, 0);
    HU_ASSERT(count == 1);
    sqlite3_finalize(stmt);

    /* Verify row contents: outcome_type should be NULL, processed should be 0. */
    rc = sqlite3_prepare_v2(
        db, "SELECT channel, contact, message_ref, outcome_type, processed FROM proactive_sends",
        -1, &stmt, NULL);
    HU_ASSERT(rc == SQLITE_OK);
    HU_ASSERT(sqlite3_step(stmt) == SQLITE_ROW);

    const char *channel = (const char *)sqlite3_column_text(stmt, 0);
    const char *contact = (const char *)sqlite3_column_text(stmt, 1);
    const char *msg_ref = (const char *)sqlite3_column_text(stmt, 2);
    int outcome_type = sqlite3_column_int(stmt, 3);
    int processed = sqlite3_column_int(stmt, 4);

    HU_ASSERT(strcmp(channel, "imessage") == 0);
    HU_ASSERT(strcmp(contact, "alice@example.com") == 0);
    HU_ASSERT(strcmp(msg_ref, "msg-ref-123") == 0);
    HU_ASSERT(outcome_type == 0); /* outcome_type should be NULL (mapped to 0) */
    HU_ASSERT(processed == 0);

    sqlite3_finalize(stmt);

    hu_dpo_collector_deinit(&collector);
    sqlite3_close(db);
}

/* Test updating a proactive send with an outcome. */
static void test_proactive_update_outcome_reply(void) {
    sqlite3 *db = test_create_db();
    if (!db)
        return;

    hu_allocator_t alloc;
    alloc = hu_system_allocator();

    hu_dpo_collector_t collector;
    hu_error_t err = hu_dpo_collector_create(&alloc, db, 10000, &collector);
    HU_ASSERT(err == HU_OK);
    err = hu_dpo_init_tables(&collector);
    HU_ASSERT(err == HU_OK);

    /* Insert a proactive send. */
    err = hu_dpo_collector_insert_proactive_send(db, "slack", 5, "bob", 3, "msg-ref-456", 11);
    HU_ASSERT(err == HU_OK);

    /* Update with REPLY outcome (0 per contextual_bandit.h). */
    err = hu_dpo_collector_update_proactive_outcome(db, "slack", 5, "bob", 3, "msg-ref-456", 11,
                                                    0); /* HU_BANDIT_REPLY = 0 */
    HU_ASSERT(err == HU_OK);

    /* Verify the update. */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT outcome_type, processed FROM proactive_sends "
                                "WHERE contact = 'bob'",
                                -1, &stmt, NULL);
    HU_ASSERT(rc == SQLITE_OK);
    HU_ASSERT(sqlite3_step(stmt) == SQLITE_ROW);

    int outcome_type = sqlite3_column_int(stmt, 0);
    int processed = sqlite3_column_int(stmt, 1);

    HU_ASSERT(outcome_type == 0); /* HU_BANDIT_REPLY */
    HU_ASSERT(processed == 0);    /* Not yet processed */

    sqlite3_finalize(stmt);

    hu_dpo_collector_deinit(&collector);
    sqlite3_close(db);
}

/* Test async processor with 3 outcomes: REPLY, IGNORED, BLOCKED. */
static void test_proactive_outcomes_process_async_three_types(void) {
#ifdef HU_ENABLE_SQLITE
    sqlite3 *db = test_create_db();
    if (!db)
        return;

    hu_allocator_t alloc;
    alloc = hu_system_allocator();

    hu_dpo_collector_t collector;
    hu_error_t err = hu_dpo_collector_create(&alloc, db, 10000, &collector);
    HU_ASSERT(err == HU_OK);
    err = hu_dpo_init_tables(&collector);
    HU_ASSERT(err == HU_OK);

    /* Create a mock bandit to track updates. */
    hu_contextual_bandit_t *bandit = NULL;
    err = hu_contextual_bandit_create(&alloc, 100, &bandit);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(bandit != NULL);

    /* Insert 3 proactive sends with different outcomes. */
    /* Row 1: alice, REPLY outcome (0). */
    err = hu_dpo_collector_insert_proactive_send(db, "imessage", 8, "alice", 5, "ref1", 4);
    HU_ASSERT(err == HU_OK);
    err = hu_dpo_collector_update_proactive_outcome(db, "imessage", 8, "alice", 5, "ref1", 4, 0);
    HU_ASSERT(err == HU_OK);

    /* Row 2: bob, IGNORED outcome (1). */
    err = hu_dpo_collector_insert_proactive_send(db, "imessage", 8, "bob", 3, "ref2", 4);
    HU_ASSERT(err == HU_OK);
    err = hu_dpo_collector_update_proactive_outcome(db, "imessage", 8, "bob", 3, "ref2", 4, 1);
    HU_ASSERT(err == HU_OK);

    /* Row 3: charlie, BLOCKED outcome (2). */
    err = hu_dpo_collector_insert_proactive_send(db, "imessage", 8, "charlie", 7, "ref3", 4);
    HU_ASSERT(err == HU_OK);
    err = hu_dpo_collector_update_proactive_outcome(db, "imessage", 8, "charlie", 7, "ref3", 4, 2);
    HU_ASSERT(err == HU_OK);

    /* Get bandit state BEFORE processing. */
    hu_contextual_bandit_arm_t arm_alice_before, arm_bob_before, arm_charlie_before;
    memset(&arm_alice_before, 0, sizeof(arm_alice_before));
    memset(&arm_bob_before, 0, sizeof(arm_bob_before));
    memset(&arm_charlie_before, 0, sizeof(arm_charlie_before));

    uint64_t alice_handle = 0;
    for (const char *p = "alice"; *p; p++) {
        alice_handle = alice_handle * 31 + (unsigned char)*p;
    }
    uint64_t bob_handle = 0;
    for (const char *p = "bob"; *p; p++) {
        bob_handle = bob_handle * 31 + (unsigned char)*p;
    }
    uint64_t charlie_handle = 0;
    for (const char *p = "charlie"; *p; p++) {
        charlie_handle = charlie_handle * 31 + (unsigned char)*p;
    }

    hu_contextual_bandit_get_arm(bandit, alice_handle, &arm_alice_before);
    hu_contextual_bandit_get_arm(bandit, bob_handle, &arm_bob_before);
    hu_contextual_bandit_get_arm(bandit, charlie_handle, &arm_charlie_before);

    /* Process outcomes. */
    err = hu_proactive_outcomes_process_async(db, bandit);
    HU_ASSERT(err == HU_OK);

    /* Get bandit state AFTER processing. */
    hu_contextual_bandit_arm_t arm_alice_after, arm_bob_after, arm_charlie_after;
    hu_contextual_bandit_get_arm(bandit, alice_handle, &arm_alice_after);
    hu_contextual_bandit_get_arm(bandit, bob_handle, &arm_bob_after);
    hu_contextual_bandit_get_arm(bandit, charlie_handle, &arm_charlie_after);

    /* Verify bandit was updated correctly:
     *   - REPLY (alice): α++ (successes increase)
     *   - IGNORED (bob): β++ (failures increase)
     *   - BLOCKED (charlie): β += 3 (strong penalty) */
    HU_ASSERT(arm_alice_after.alpha > arm_alice_before.alpha);
    HU_ASSERT(arm_bob_after.beta > arm_bob_before.beta);
    HU_ASSERT(arm_charlie_after.beta >= arm_charlie_before.beta + 3);

    /* Verify all rows were marked processed = 1. */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM proactive_sends WHERE processed = 1", -1,
                                &stmt, NULL);
    HU_ASSERT(rc == SQLITE_OK);
    HU_ASSERT(sqlite3_step(stmt) == SQLITE_ROW);
    int processed_count = sqlite3_column_int(stmt, 0);
    HU_ASSERT(processed_count == 3);
    sqlite3_finalize(stmt);

    hu_contextual_bandit_destroy(bandit);
    hu_dpo_collector_deinit(&collector);
    sqlite3_close(db);
#endif /* HU_ENABLE_SQLITE */
}

/* Test that unprocessed rows without outcomes are not touched. */
static void test_proactive_outcomes_skip_unresolved(void) {
#ifdef HU_ENABLE_SQLITE
    sqlite3 *db = test_create_db();
    if (!db)
        return;

    hu_allocator_t alloc;
    alloc = hu_system_allocator();

    hu_dpo_collector_t collector;
    hu_error_t err = hu_dpo_collector_create(&alloc, db, 10000, &collector);
    HU_ASSERT(err == HU_OK);
    err = hu_dpo_init_tables(&collector);
    HU_ASSERT(err == HU_OK);

    hu_contextual_bandit_t *bandit = NULL;
    err = hu_contextual_bandit_create(&alloc, 100, &bandit);
    HU_ASSERT(err == HU_OK);

    /* Insert 2 rows: one unresolved (outcome_type=NULL), one resolved. */
    err = hu_dpo_collector_insert_proactive_send(db, "imessage", 8, "alice", 5, "ref1", 4);
    HU_ASSERT(err == HU_OK);

    err = hu_dpo_collector_insert_proactive_send(db, "imessage", 8, "bob", 3, "ref2", 4);
    HU_ASSERT(err == HU_OK);
    err = hu_dpo_collector_update_proactive_outcome(db, "imessage", 8, "bob", 3, "ref2", 4, 0);
    HU_ASSERT(err == HU_OK);

    /* Process outcomes. */
    err = hu_proactive_outcomes_process_async(db, bandit);
    HU_ASSERT(err == HU_OK);

    /* Verify: only bob's row should be marked processed=1. */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT COUNT(*) FROM proactive_sends WHERE contact = 'bob' "
                                "AND processed = 1",
                                -1, &stmt, NULL);
    HU_ASSERT(rc == SQLITE_OK);
    HU_ASSERT(sqlite3_step(stmt) == SQLITE_ROW);
    int bob_processed = sqlite3_column_int(stmt, 0);
    HU_ASSERT(bob_processed == 1);
    sqlite3_finalize(stmt);

    /* Verify: alice's row should remain processed=0. */
    rc = sqlite3_prepare_v2(db,
                            "SELECT COUNT(*) FROM proactive_sends WHERE contact = 'alice' "
                            "AND processed = 0",
                            -1, &stmt, NULL);
    HU_ASSERT(rc == SQLITE_OK);
    HU_ASSERT(sqlite3_step(stmt) == SQLITE_ROW);
    int alice_unprocessed = sqlite3_column_int(stmt, 0);
    HU_ASSERT(alice_unprocessed == 1);
    sqlite3_finalize(stmt);

    hu_contextual_bandit_destroy(bandit);
    hu_dpo_collector_deinit(&collector);
    sqlite3_close(db);
#endif /* HU_ENABLE_SQLITE */
}

/* Test that double-updates (outcome already set) are rejected. */
static void test_proactive_outcomes_prevent_double_update(void) {
#ifdef HU_ENABLE_SQLITE
    sqlite3 *db = test_create_db();
    if (!db)
        return;

    hu_allocator_t alloc;
    alloc = hu_system_allocator();

    hu_dpo_collector_t collector;
    hu_error_t err = hu_dpo_collector_create(&alloc, db, 10000, &collector);
    HU_ASSERT(err == HU_OK);
    err = hu_dpo_init_tables(&collector);
    HU_ASSERT(err == HU_OK);

    /* Insert a proactive send and set outcome to REPLY. */
    err = hu_dpo_collector_insert_proactive_send(db, "imessage", 8, "alice", 5, "ref1", 4);
    HU_ASSERT(err == HU_OK);
    err = hu_dpo_collector_update_proactive_outcome(db, "imessage", 8, "alice", 5, "ref1", 4, 0);
    HU_ASSERT(err == HU_OK);

    /* Try to update again with a different outcome (e.g., IGNORED). */
    /* This should be silently rejected by the WHERE clause. */
    err = hu_dpo_collector_update_proactive_outcome(db, "imessage", 8, "alice", 5, "ref1", 4, 1);
    HU_ASSERT(err == HU_OK); /* Still returns HU_OK, but the row isn't updated */

    /* Verify: outcome_type should still be 0 (REPLY). */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db, "SELECT outcome_type FROM proactive_sends WHERE contact = 'alice'", -1, &stmt, NULL);
    HU_ASSERT(rc == SQLITE_OK);
    HU_ASSERT(sqlite3_step(stmt) == SQLITE_ROW);
    int outcome_type = sqlite3_column_int(stmt, 0);
    HU_ASSERT(outcome_type == 0); /* Still REPLY, not updated to IGNORED */
    sqlite3_finalize(stmt);

    hu_dpo_collector_deinit(&collector);
    sqlite3_close(db);
#endif /* HU_ENABLE_SQLITE */
}

void run_proactive_outcomes_tests(void) {
    HU_TEST_SUITE("proactive_outcomes");
    HU_RUN_TEST(test_proactive_sends_table_created);
    HU_RUN_TEST(test_proactive_insert_send);
    HU_RUN_TEST(test_proactive_update_outcome_reply);
    HU_RUN_TEST(test_proactive_outcomes_process_async_three_types);
    HU_RUN_TEST(test_proactive_outcomes_skip_unresolved);
    HU_RUN_TEST(test_proactive_outcomes_prevent_double_update);
}
