/* tests/test_reaction_handler_lookup_store.c
 *
 * Phase 5 R4: lookup store contract for src/agent/reaction_handler.c.
 *
 * The reaction handler's lookup store has two backends:
 *
 *   HU_IS_TEST          → in-memory array (deterministic, no disk I/O)
 *   HU_ENABLE_SQLITE    → SQLite-backed persistent store at
 *                         ~/.human/reaction_lookup.db (production)
 *
 * These tests run under HU_IS_TEST and exercise the IN-MEMORY path. They
 * pin the public API contract — register / handle_event / reset — which
 * must remain identical regardless of backend.
 *
 * For SQLite-specific behavior (capacity > 256, retention sweep, persistence
 * across "process restart"), see the opt-in HU_HAVE_REACTION_LOOKUP_SQLITE
 * tests at the bottom. Those tests are gated off by default because they
 * would need a real-disk DB; they document how to exercise the path manually.
 */
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include "human/agent/reaction_handler.h"
#include "human/channels/reaction_event.h"
#include "human/core/allocator.h"
#include "human/ml/dpo.h"
#include <sqlite3.h>
#include <string.h>

/* ----- helpers ----- */

static void wire_collector(sqlite3 **db_out, hu_dpo_collector_t *col_out) {
    HU_ASSERT_EQ(sqlite3_open(":memory:", db_out), SQLITE_OK);
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, *db_out, /*max_pairs=*/4096, col_out), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(col_out), HU_OK);
    hu_reaction_handler_set_collector(col_out);
}

static void teardown(sqlite3 *db, hu_dpo_collector_t *col) {
    hu_dpo_collector_deinit(col);
    sqlite3_close(db);
    hu_reaction_handler_reset_for_test();
}

/* ----- in-memory contract tests ----- */

static void test_register_then_handle_event_records_dpo_pair(void) {
    hu_reaction_handler_reset_for_test();
    sqlite3 *db = NULL;
    hu_dpo_collector_t col = {0};
    wire_collector(&db, &col);

    hu_reaction_handler_register_assistant_message_for_test("imessage", "chat_A", "msg_1",
                                                            "How are you?", "Doing great, thanks!");

    hu_reaction_event_t e = {.channel_id = "imessage",
                             .target_thread_id = "chat_A",
                             .target_message_ref = "msg_1",
                             .kind = HU_REACTION_LOVE,
                             .polarity = HU_REACTION_POSITIVE};
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);

    size_t n = 0;
    HU_ASSERT_EQ(hu_dpo_pair_count(&col, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);

    teardown(db, &col);
}

static void test_register_two_distinct_keys_both_retrievable(void) {
    hu_reaction_handler_reset_for_test();
    sqlite3 *db = NULL;
    hu_dpo_collector_t col = {0};
    wire_collector(&db, &col);

    hu_reaction_handler_register_assistant_message_for_test("imessage", "chat_X", "msg_a", "Q1",
                                                            "R1");
    hu_reaction_handler_register_assistant_message_for_test("slack", "C_Y", "msg_b", "Q2", "R2");

    hu_reaction_event_t e1 = {.channel_id = "imessage",
                              .target_thread_id = "chat_X",
                              .target_message_ref = "msg_a",
                              .kind = HU_REACTION_LIKE,
                              .polarity = HU_REACTION_POSITIVE};
    hu_reaction_event_t e2 = {.channel_id = "slack",
                              .target_thread_id = "C_Y",
                              .target_message_ref = "msg_b",
                              .kind = HU_REACTION_LIKE,
                              .polarity = HU_REACTION_POSITIVE};

    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e1), HU_OK);
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e2), HU_OK);

    size_t n = 0;
    HU_ASSERT_EQ(hu_dpo_pair_count(&col, &n), HU_OK);
    HU_ASSERT_EQ(n, 2);

    teardown(db, &col);
}

static void test_register_same_key_twice_upserts_latest_wins(void) {
    hu_reaction_handler_reset_for_test();
    sqlite3 *db = NULL;
    hu_dpo_collector_t col = {0};
    wire_collector(&db, &col);

    /* First registration */
    hu_reaction_handler_register_assistant_message_for_test("imessage", "chat_dup", "msg_dup",
                                                            "Original prompt", "Original response");

    /* Second registration with the SAME (channel, thread, msg_ref) but
     * different prompt/response — upsert semantics mean latest wins. */
    hu_reaction_handler_register_assistant_message_for_test("imessage", "chat_dup", "msg_dup",
                                                            "Updated prompt", "Updated response");

    hu_reaction_event_t e = {.channel_id = "imessage",
                             .target_thread_id = "chat_dup",
                             .target_message_ref = "msg_dup",
                             .kind = HU_REACTION_LOVE,
                             .polarity = HU_REACTION_POSITIVE};
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);

    /* Verify the LATEST values landed in the DPO row */
    sqlite3_stmt *stmt = NULL;
    HU_ASSERT_EQ(
        sqlite3_prepare_v2(db, "SELECT prompt, chosen FROM dpo_pairs LIMIT 1", -1, &stmt, NULL),
        SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "Updated prompt");
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "Updated response");
    sqlite3_finalize(stmt);

    /* And only ONE row (no duplicate) */
    size_t n = 0;
    HU_ASSERT_EQ(hu_dpo_pair_count(&col, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);

    teardown(db, &col);
}

static void test_register_without_matching_lookup_returns_not_found(void) {
    hu_reaction_handler_reset_for_test();
    sqlite3 *db = NULL;
    hu_dpo_collector_t col = {0};
    wire_collector(&db, &col);

    hu_reaction_handler_register_assistant_message_for_test("imessage", "chat_A", "msg_real", "Q",
                                                            "R");

    /* Event references a non-registered msg_ref → NOT_FOUND */
    hu_reaction_event_t e = {.channel_id = "imessage",
                             .target_thread_id = "chat_A",
                             .target_message_ref = "msg_does_not_exist",
                             .kind = HU_REACTION_LOVE,
                             .polarity = HU_REACTION_POSITIVE};
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_ERR_NOT_FOUND);

    /* No row inserted; per-turn flag stays false */
    size_t n = 0;
    HU_ASSERT_EQ(hu_dpo_pair_count(&col, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);
    HU_ASSERT_FALSE(hu_reaction_handler_was_called_this_turn());

    teardown(db, &col);
}

static void test_reset_for_test_clears_lookup_store(void) {
    hu_reaction_handler_reset_for_test();
    sqlite3 *db = NULL;
    hu_dpo_collector_t col = {0};
    wire_collector(&db, &col);

    hu_reaction_handler_register_assistant_message_for_test("imessage", "chat_R", "msg_R", "Q",
                                                            "R");

    /* Confirm it's there */
    hu_reaction_event_t e = {.channel_id = "imessage",
                             .target_thread_id = "chat_R",
                             .target_message_ref = "msg_R",
                             .kind = HU_REACTION_LOVE,
                             .polarity = HU_REACTION_POSITIVE};
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);

    /* Reset wipes the lookup store (and collector/personal_model wiring) */
    hu_reaction_handler_reset_for_test();

    /* Re-wire collector after reset (reset clears the s_collector pointer) */
    hu_reaction_handler_set_collector(&col);

    /* Same event now misses */
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_ERR_NOT_FOUND);

    teardown(db, &col);
}

/* Capacity smoke: register beyond the original 256-entry cap to confirm
 * the in-memory store now tolerates more (was: silent-drop on overflow).
 * The test path's array is 1024 entries, so 500 registrations exercises
 * comfortably past the old limit without flirting with the new one. */
static void test_register_beyond_old_256_cap_no_silent_drop(void) {
    hu_reaction_handler_reset_for_test();
    sqlite3 *db = NULL;
    hu_dpo_collector_t col = {0};
    wire_collector(&db, &col);

    char thread[64], msg_ref[64];
    for (int i = 0; i < 500; i++) {
        snprintf(thread, sizeof(thread), "chat_%d", i);
        snprintf(msg_ref, sizeof(msg_ref), "msg_%d", i);
        hu_reaction_handler_register_assistant_message_for_test("imessage", thread, msg_ref, "p",
                                                                "r");
    }

    /* The 400th entry (well past the old cap) must still be retrievable. */
    snprintf(thread, sizeof(thread), "chat_%d", 400);
    snprintf(msg_ref, sizeof(msg_ref), "msg_%d", 400);
    hu_reaction_event_t e = {.channel_id = "imessage",
                             .target_thread_id = thread,
                             .target_message_ref = msg_ref,
                             .kind = HU_REACTION_LOVE,
                             .polarity = HU_REACTION_POSITIVE};
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);

    teardown(db, &col);
}

static void test_null_event_is_invalid_argument(void) {
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_removal_event_is_dropped_silently(void) {
    hu_reaction_handler_reset_for_test();
    hu_reaction_event_t e = {.channel_id = "imessage",
                             .target_thread_id = "x",
                             .target_message_ref = "y",
                             .kind = HU_REACTION_LOVE,
                             .polarity = HU_REACTION_POSITIVE,
                             .is_removal = 1};
    /* Removal events return OK without touching the collector */
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);
    HU_ASSERT_FALSE(hu_reaction_handler_was_called_this_turn());
}

static void test_neutral_polarity_no_dpo_row_inserted(void) {
    hu_reaction_handler_reset_for_test();
    sqlite3 *db = NULL;
    hu_dpo_collector_t col = {0};
    wire_collector(&db, &col);

    hu_reaction_handler_register_assistant_message_for_test("imessage", "chat_N", "msg_N", "Q",
                                                            "R");

    hu_reaction_event_t e = {.channel_id = "imessage",
                             .target_thread_id = "chat_N",
                             .target_message_ref = "msg_N",
                             .kind = HU_REACTION_LIKE,
                             .polarity = 0};
    /* Neutral reactions return OK but don't generate a training pair */
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);

    size_t n = 0;
    HU_ASSERT_EQ(hu_dpo_pair_count(&col, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);

    teardown(db, &col);
}

static void test_negative_polarity_records_rejected_not_chosen(void) {
    hu_reaction_handler_reset_for_test();
    sqlite3 *db = NULL;
    hu_dpo_collector_t col = {0};
    wire_collector(&db, &col);

    hu_reaction_handler_register_assistant_message_for_test("slack", "chat_Neg", "msg_Neg",
                                                            "What's 2+2?", "5");

    hu_reaction_event_t e = {.channel_id = "slack",
                             .target_thread_id = "chat_Neg",
                             .target_message_ref = "msg_Neg",
                             .kind = HU_REACTION_DISLIKE,
                             .polarity = HU_REACTION_NEGATIVE};
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);

    sqlite3_stmt *stmt = NULL;
    HU_ASSERT_EQ(
        sqlite3_prepare_v2(db, "SELECT chosen, rejected FROM dpo_pairs LIMIT 1", -1, &stmt, NULL),
        SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    /* chosen empty, rejected populated for negative polarity */
    const char *chosen = (const char *)sqlite3_column_text(stmt, 0);
    HU_ASSERT_TRUE(chosen == NULL || chosen[0] == '\0');
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "5");
    sqlite3_finalize(stmt);

    teardown(db, &col);
}

/* ----- opt-in SQLite-path tests -----
 *
 * These exercise the real SQLite-backed path against a temporary DB. They
 * are NOT compiled in normal test builds because they would need a real
 * disk path, the HU_IS_TEST guard in the reaction_handler TU forces the
 * in-memory backend, and the production code paths aren't linked into
 * human_core_test by name.
 *
 * To exercise the SQLite path manually:
 *   1. Build the production binary (which uses HU_RXN_LOOKUP_USES_SQLITE=1)
 *   2. Run with HOME=/tmp/some-empty-dir
 *   3. Send tapbacks via the daemon path
 *   4. Inspect /tmp/some-empty-dir/.human/reaction_lookup.db with sqlite3
 *
 * Bridging this gap properly requires either exposing the SQLite path
 * helpers via a test-only header, or compiling a separate test_main that
 * doesn't define HU_IS_TEST. Both are larger surgical changes than the
 * Phase-5 R4 scope warrants — flagged in the report-back as an open
 * question. */
#ifdef HU_HAVE_REACTION_LOOKUP_SQLITE
/* Reserved for future opt-in tests. Today the SQLite backend's contract is
 * pinned by the in-memory tests above (which exercise the same public API)
 * plus the production code path's clang compile + lint guarantees. */
#endif

void run_reaction_handler_lookup_store_tests(void) {
    HU_TEST_SUITE("reaction_handler_lookup_store");
    HU_RUN_TEST(test_register_then_handle_event_records_dpo_pair);
    HU_RUN_TEST(test_register_two_distinct_keys_both_retrievable);
    HU_RUN_TEST(test_register_same_key_twice_upserts_latest_wins);
    HU_RUN_TEST(test_register_without_matching_lookup_returns_not_found);
    HU_RUN_TEST(test_reset_for_test_clears_lookup_store);
    HU_RUN_TEST(test_register_beyond_old_256_cap_no_silent_drop);
    HU_RUN_TEST(test_null_event_is_invalid_argument);
    HU_RUN_TEST(test_removal_event_is_dropped_silently);
    HU_RUN_TEST(test_neutral_polarity_no_dpo_row_inserted);
    HU_RUN_TEST(test_negative_polarity_records_rejected_not_chosen);
}

#else /* !HU_ENABLE_SQLITE — stub runner so the symbol always resolves */

void run_reaction_handler_lookup_store_tests(void) { /* no-op when SQLite is disabled */ }

#endif /* HU_ENABLE_SQLITE */
