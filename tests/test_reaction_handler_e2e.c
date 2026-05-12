/* tests/test_reaction_handler_e2e.c
 *
 * Phase 2 Task 13 (RL SOTA): E2E coverage for hu_reaction_handler_handle_event.
 *
 * Pins the contract documented in src/agent/reaction_handler.c:
 *
 *   1. A known-target reaction event with positive polarity inserts a
 *      dpo_pairs row into the wired collector. The row's `chosen` column
 *      is the assistant response, `rejected` is empty, and `source` is
 *      the channel-specific tag (imessage_tapback / slack_reactji).
 *      The per-turn flag is set on success.
 *
 *   2. A reaction event whose (channel, thread, msg_ref) triple does not
 *      resolve via the in-memory lookup returns HU_ERR_NOT_FOUND so the
 *      caller can log a metric. Phase 2 callers (slack_reactions.c +
 *      Task 14's daemon dispatch) silently absorb this code, making the
 *      drop user-invisible. The per-turn flag is NOT set on a miss.
 */
#include "test_framework.h"
#include "human/agent/reaction_handler.h"
#include "human/channels/reaction_event.h"
#include "human/ml/dpo.h"
#include "human/core/allocator.h"
#include <sqlite3.h>

static void test_reaction_event_with_known_target_inserts_dpo_pair(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Set up an in-memory dpo_pairs SQLite store via the actual API in
     * include/human/ml/dpo.h:39-46 */
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col = {0};
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, /*max_pairs=*/1024, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    /* Wire the reaction handler to write into this collector */
    hu_reaction_handler_reset_for_test();
    hu_reaction_handler_set_collector(&col);

    /* Pre-populate a chat history record so the handler has something to lookup */
    hu_reaction_handler_register_assistant_message_for_test(
        /*channel*/ "imessage",
        /*thread*/ "chat_xyz",
        /*msg_ref*/ "msg_abc",
        /*prompt*/ "What's the weather?",
        /*response*/ "Sunny and 72."
    );

    hu_reaction_event_t e = {
        .channel_id = "imessage",
        .target_thread_id = "chat_xyz",
        .target_message_ref = "msg_abc",
        .sender_handle = "+15551234567",
        .kind = HU_REACTION_LOVE,
        .polarity = HU_REACTION_POSITIVE,
        .is_removal = 0,
    };
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);

    /* Verify a row was inserted via the public count API */
    size_t n = 0;
    HU_ASSERT_EQ(hu_dpo_pair_count(&col, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);

    /* Verify content via direct SQL query (collector has no read-back API today) */
    sqlite3_stmt *stmt = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT prompt, chosen, rejected, source FROM dpo_pairs LIMIT 1",
        -1, &stmt, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "What's the weather?");
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "Sunny and 72.");
    /* rejected column is empty string for positive-polarity row */
    const char *rej = (const char *)sqlite3_column_text(stmt, 2);
    HU_ASSERT_TRUE(rej == NULL || rej[0] == '\0');
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 3), "imessage_tapback");
    sqlite3_finalize(stmt);

    /* Verify the per-turn flag was set */
    HU_ASSERT_TRUE(hu_reaction_handler_was_called_this_turn());

    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
    hu_reaction_handler_reset_for_test();
}

/* "Silently drops" here means: no DPO row inserted, no agent_turn side
 * effect (clear_turn flag stays false). The function still returns
 * HU_ERR_NOT_FOUND so callers can log the metric — but ALL callers in
 * Phase 2 (imessage poll, slack webhook handler) ignore the return code,
 * making the drop silent in practice. Future callers that surface this
 * to users MUST treat HU_ERR_NOT_FOUND as a no-op, NOT a hard error. */
static void test_reaction_event_with_unknown_target_drops_silently(void) {
    hu_reaction_handler_reset_for_test();
    hu_reaction_event_t e = {
        .channel_id = "slack", .target_thread_id = "C_X",
        .target_message_ref = "ts_does_not_exist",
        .kind = HU_REACTION_LIKE, .polarity = HU_REACTION_POSITIVE,
    };
    /* Return code carries the diagnostic; callers translate to silent drop. */
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_ERR_NOT_FOUND);
    /* And the per-turn flag should NOT be set on a failed lookup */
    HU_ASSERT_FALSE(hu_reaction_handler_was_called_this_turn());
}

void run_reaction_handler_e2e_tests(void) {
    HU_TEST_SUITE("reaction_handler_e2e");
    HU_RUN_TEST(test_reaction_event_with_known_target_inserts_dpo_pair);
    HU_RUN_TEST(test_reaction_event_with_unknown_target_drops_silently);
}
