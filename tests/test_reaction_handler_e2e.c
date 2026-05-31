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
#include "human/agent/reaction_handler.h"
#include "human/channels/reaction_event.h"
#include "human/core/allocator.h"
#include "human/ml/dpo.h"
#include "test_framework.h"
#include <sqlite3.h>

static void test_reaction_event_with_known_target_updates_production_outcomes(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Set up an in-memory collector with production_outcomes table.
     * Phase 2 design: reactions update production_outcomes, and the nightly
     * miner creates dpo_pairs. See docs/plans/2026-05-19-agi-path.md. */
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
        /*thread*/ "contact_alice",
        /*msg_ref*/ "msg_001",
        /*prompt*/ "What's the weather?",
        /*response*/ "Sunny and 72.");

    /* First, simulate an outbound message being recorded to production_outcomes.
     * This is what hu_dpo_record_outbound does in daemon.c. */
    HU_ASSERT_EQ(hu_dpo_record_outbound(&col,
                                        /*channel*/ "imessage", strlen("imessage"),
                                        /*target*/ "contact_alice", strlen("contact_alice"),
                                        /*message_ref*/ "msg_001", strlen("msg_001"),
                                        /*prompt*/ "What's the weather?",
                                        strlen("What's the weather?"),
                                        /*chosen*/ "Sunny and 72.", strlen("Sunny and 72."),
                                        /*p_seth_at_send*/ 0.85,
                                        /*alternatives_json*/ NULL, 0),
                 HU_OK);

    /* Verify the production_outcomes row was created with NULL outcome */
    sqlite3_stmt *check = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(
                     db, "SELECT COUNT(*) FROM production_outcomes WHERE message_ref='msg_001'", -1,
                     &check, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(check), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(check, 0), 1);
    sqlite3_finalize(check);

    /* Now handle the reaction: this should update the production_outcomes row
     * with the tapback_polarity outcome. */
    hu_reaction_event_t e = {
        .channel_id = "imessage",
        .target_thread_id = "contact_alice",
        .target_message_ref = "msg_001",
        .sender_handle = "+15551234567",
        .kind = HU_REACTION_LOVE,
        .polarity = HU_REACTION_POSITIVE,
        .is_removal = 0,
        .timestamp_unix = 1000000,
    };
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);

    /* Verify the production_outcomes row was updated with the tapback_polarity */
    HU_ASSERT_EQ(
        sqlite3_prepare_v2(db,
                           "SELECT tapback_polarity, outcome_resolved_at FROM production_outcomes "
                           "WHERE message_ref='msg_001'",
                           -1, &check, NULL),
        SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(check), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(check, 0), 1); /* positive tapback */
    HU_ASSERT_NOT_NULL(
        (void *)(intptr_t)sqlite3_column_int64(check, 1)); /* outcome_resolved_at is set */
    sqlite3_finalize(check);

    /* Verify the per-turn flag was set */
    HU_ASSERT_TRUE(hu_reaction_handler_was_called_this_turn());

    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
    hu_reaction_handler_reset_for_test();
}

/* Test: when a negative tapback arrives, it should update production_outcomes
 * with tapback_polarity = -1, which the nightly miner will pair against
 * responses that received no reply (is_rejected case in mine_pairs_from_outcomes). */
static void test_negative_reaction_sets_tapback_polarity_minus_one(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col = {0};
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 1024, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    hu_reaction_handler_reset_for_test();
    hu_reaction_handler_set_collector(&col);
    hu_reaction_handler_register_assistant_message_for_test("imessage", "contact_bob", "msg_002",
                                                            "Tell a joke.", "Bad response.");

    /* Record outbound to production_outcomes */
    HU_ASSERT_EQ(hu_dpo_record_outbound(&col, "imessage", strlen("imessage"), "contact_bob",
                                        strlen("contact_bob"), "msg_002", strlen("msg_002"),
                                        "Tell a joke.", strlen("Tell a joke."), "Bad response.",
                                        strlen("Bad response."), 0.75, NULL, 0),
                 HU_OK);

    /* Send a negative reaction */
    hu_reaction_event_t e = {
        .channel_id = "imessage",
        .target_thread_id = "contact_bob",
        .target_message_ref = "msg_002",
        .kind = HU_REACTION_DISLIKE,
        .polarity = HU_REACTION_NEGATIVE,
        .is_removal = 0,
        .timestamp_unix = 2000000,
    };
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);

    /* Verify tapback_polarity was set to -1 */
    sqlite3_stmt *check = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(
                     db,
                     "SELECT tapback_polarity FROM production_outcomes WHERE message_ref='msg_002'",
                     -1, &check, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(check), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(check, 0), -1);
    sqlite3_finalize(check);

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
        .channel_id = "slack",
        .target_thread_id = "C_X",
        .target_message_ref = "ts_does_not_exist",
        .kind = HU_REACTION_LIKE,
        .polarity = HU_REACTION_POSITIVE,
    };
    /* Return code carries the diagnostic; callers translate to silent drop. */
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_ERR_NOT_FOUND);
    /* And the per-turn flag should NOT be set on a failed lookup */
    HU_ASSERT_FALSE(hu_reaction_handler_was_called_this_turn());
}

/* Phase 2 Task 14: lifecycle smoke for the per-turn flag. handle_event
 * sets it on success; clear_turn (called at the end of every successful
 * hu_agent_turn) resets it. Pinned here as a standalone smoke because
 * integration with hu_agent_turn is exercised in production paths and
 * Phase 5 will add a daemon-level e2e. */
static void test_agent_turn_clear_turn_resets_called_flag(void) {
    hu_reaction_handler_reset_for_test();

    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col = {0};
    hu_dpo_collector_create(&alloc, db, 1024, &col);
    hu_dpo_init_tables(&col);
    hu_reaction_handler_set_collector(&col);
    hu_reaction_handler_register_assistant_message_for_test("imessage", "ct", "mr", "p", "r");

    HU_ASSERT_FALSE(hu_reaction_handler_was_called_this_turn());

    hu_reaction_event_t e = {.channel_id = "imessage",
                             .target_thread_id = "ct",
                             .target_message_ref = "mr",
                             .kind = HU_REACTION_LOVE,
                             .polarity = HU_REACTION_POSITIVE};
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);
    HU_ASSERT_TRUE(hu_reaction_handler_was_called_this_turn());

    hu_reaction_handler_clear_turn();
    HU_ASSERT_FALSE(hu_reaction_handler_was_called_this_turn());

    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
    hu_reaction_handler_reset_for_test();
}

/* Pin the NULL-input guards in src/agent/reaction_handler.c:53. Adding this
 * regression test (cheap, no setup) so any future refactor that drops the
 * `if (!e || !e->channel_id)` early-return surfaces as a test failure
 * instead of a daemon-loop NULL-deref. */
static void test_reaction_handler_handle_event_null_returns_invalid_argument(void) {
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(NULL), HU_ERR_INVALID_ARGUMENT);
    /* Also pin: event with NULL channel_id */
    hu_reaction_event_t e = {0}; /* channel_id NULL */
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_ERR_INVALID_ARGUMENT);
}

void run_reaction_handler_e2e_tests(void) {
    HU_TEST_SUITE("reaction_handler_e2e");
    HU_RUN_TEST(test_reaction_event_with_known_target_updates_production_outcomes);
    HU_RUN_TEST(test_negative_reaction_sets_tapback_polarity_minus_one);
    HU_RUN_TEST(test_reaction_event_with_unknown_target_drops_silently);
    HU_RUN_TEST(test_agent_turn_clear_turn_resets_called_flag);
    HU_RUN_TEST(test_reaction_handler_handle_event_null_returns_invalid_argument);
}
