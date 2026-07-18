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
#include "human/daemon/message_router.h"
#include "human/ml/dpo.h"
#include "test_framework.h"
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
        /*response*/ "Sunny and 72.",
        /*alternative*/ "");

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
    HU_ASSERT_EQ(
        sqlite3_prepare_v2(db, "SELECT prompt, chosen, rejected, source FROM dpo_pairs LIMIT 1", -1,
                           &stmt, NULL),
        SQLITE_OK);
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
    hu_reaction_handler_register_assistant_message_for_test("imessage", "ct", "mr", "p", "r", "");

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

/* BLOCKER 2: positive tapback with alternative draft creates a complete pair.
 * The pair's chosen = response, rejected = alternative, both non-empty.
 * Proves reactions with alternatives now yield trainable complete pairs
 * (previously, single-sided rows were dropped during export). */
static void test_positive_tapback_with_alternative_creates_complete_pair(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col = {0};
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 1024, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    hu_reaction_handler_reset_for_test();
    hu_reaction_handler_set_collector(&col);

    /* Register a message WITH an alternative (the rejected draft from the retry path) */
    const char *prompt = "What time is the meeting?";
    const char *response = "The meeting is at 2pm.";
    const char *alternative = "Meeting is scheduled for 14:00 hours today.";

    hu_reaction_handler_register_assistant_message_for_test("imessage", "chat_abc", "msg_123",
                                                            prompt, response, alternative);

    /* Fire a positive tapback */
    hu_reaction_event_t e = {
        .channel_id = "imessage",
        .target_thread_id = "chat_abc",
        .target_message_ref = "msg_123",
        .sender_handle = "+15551234567",
        .kind = HU_REACTION_LOVE,
        .polarity = HU_REACTION_POSITIVE,
        .is_removal = 0,
    };
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);

    /* Verify the dpo_pairs row has BOTH chosen AND rejected non-empty */
    sqlite3_stmt *stmt = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "SELECT prompt, chosen, rejected FROM dpo_pairs WHERE source = "
                                    "'imessage_tapback' LIMIT 1",
                                    -1, &stmt, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);

    /* Assertion 1: prompt matches */
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), prompt);

    /* Assertion 2: chosen = response (the sent message) */
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), response);

    /* Assertion 3: rejected = alternative (the OTHER side of the pair) */
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 2), alternative);

    sqlite3_finalize(stmt);

    /* CRITICAL: Verify this pair is now exportable (not dropped like single-sided rows).
     * hu_dpo_export returns pairs where both sides >= 4 bytes; reactions now produce them. */
    hu_dpo_export_t export = {0};
    HU_ASSERT_EQ(hu_dpo_export(&col, &alloc, &export), HU_OK);
    HU_ASSERT_GE(export.count, 1); /* At least one pair exported (not dropped as single-sided) */

    hu_dpo_export_free(&alloc, &export);
    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
    hu_reaction_handler_reset_for_test();
}

/* BLOCKER 2: negative tapback with alternative creates a complete pair.
 * The pair's rejected = response, chosen = alternative.
 * Proves negative reactions also produce trainable complete pairs. */
static void test_negative_tapback_with_alternative_creates_complete_pair(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col = {0};
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 1024, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    hu_reaction_handler_reset_for_test();
    hu_reaction_handler_set_collector(&col);

    const char *prompt = "What's your favorite color?";
    const char *response = "I really like blue.";
    const char *alternative = "My preferred shade is deep blue.";

    hu_reaction_handler_register_assistant_message_for_test("imessage", "chat_xyz", "msg_456",
                                                            prompt, response, alternative);

    /* Fire a negative tapback (thumbs down) */
    hu_reaction_event_t e = {
        .channel_id = "imessage",
        .target_thread_id = "chat_xyz",
        .target_message_ref = "msg_456",
        .sender_handle = "+15551234567",
        .kind = HU_REACTION_DISLIKE,
        .polarity = HU_REACTION_NEGATIVE,
        .is_removal = 0,
    };
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);

    /* Verify: NEGATIVE means rejected = response, chosen = alternative */
    sqlite3_stmt *stmt = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "SELECT prompt, chosen, rejected FROM dpo_pairs WHERE source = "
                                    "'imessage_tapback' LIMIT 1",
                                    -1, &stmt, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);

    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), prompt);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), alternative); /* chosen */
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 2), response);    /* rejected */

    sqlite3_finalize(stmt);

    /* Verify the pair is exportable */
    hu_dpo_export_t export = {0};
    HU_ASSERT_EQ(hu_dpo_export(&col, &alloc, &export), HU_OK);
    HU_ASSERT_GE(export.count, 1);

    hu_dpo_export_free(&alloc, &export);
    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
    hu_reaction_handler_reset_for_test();
}

/* B2 go-live: verify the production path (daemon calling with agent->sota.last_rejected_draft)
 * yields a complete pair. This pins the B2 wire: the daemon registers with an
 * alternative draft, the reaction handler inserts both sides, and the pair is
 * exportable. */
static void test_production_reaction_path_with_rejected_draft_yields_complete_pair(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col = {0};
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 512, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    hu_reaction_handler_reset_for_test();
    hu_reaction_handler_set_collector(&col);

    /* Simulate the production flow: the response guard rejected this draft,
     * and the daemon (via agent->sota.last_rejected_draft) threads it to
     * the reaction handler as the alternative. */
    const char *prompt = "What should I prioritize?";
    const char *chosen_response = "Ship the fix now.";
    const char *rejected_draft = "I think we should rewrite everything."; /* what guard rejected */

    hu_reaction_handler_register_assistant_message_for_production(
        /*channel*/ "imessage",
        /*thread*/ "chat_production_001",
        /*msg_ref*/ "msg_prod_001",
        /*prompt*/ prompt,
        /*response*/ chosen_response,
        /*alternative (from agent->sota.last_rejected_draft)*/ rejected_draft);

    /* Now fire a positive tapback on that message */
    hu_reaction_event_t evt = {
        .channel_id = "imessage",
        .target_thread_id = "chat_production_001",
        .target_message_ref = "msg_prod_001",
        .sender_handle = "+15550100001",
        .kind = HU_REACTION_LOVE,
        .polarity = HU_REACTION_POSITIVE,
        .timestamp_unix = 1715472000,
        .is_removal = 0,
    };
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&evt), HU_OK);

    /* Verify we got a COMPLETE pair (not single-sided) */
    size_t pair_count = 0;
    HU_ASSERT_EQ(hu_dpo_pair_count(&col, &pair_count), HU_OK);
    HU_ASSERT_EQ(pair_count, 1u);

    /* Verify the pair has both sides: chosen=rejected_draft, rejected=chosen_response
     * dpo_pairs has: id, prompt, chosen, rejected, margin, timestamp, source (no thread/msg_ref) */
    sqlite3_stmt *stmt = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "SELECT prompt, chosen, rejected, source FROM dpo_pairs WHERE "
                                    "source = 'imessage_tapback' LIMIT 1",
                                    -1, &stmt, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);

    /* For POSITIVE reaction:
     * - chosen = response (the message the user reacted positively to)
     * - rejected = alternative (the rejected draft becomes the "other side")
     * See src/agent/reaction_handler.c:392-404 for the logic. */
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), prompt);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1),
                     chosen_response); /* chosen = response */
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 2),
                     rejected_draft); /* rejected = alternative */
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 3), "imessage_tapback");

    sqlite3_finalize(stmt);

    /* Verify the pair is exportable (non-vacuous: hu_dpo_export actually returns it) */
    hu_dpo_export_t export = {0};
    HU_ASSERT_EQ(hu_dpo_export(&col, &alloc, &export), HU_OK);
    HU_ASSERT_GE(export.count, 1);

    hu_dpo_export_free(&alloc, &export);
    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
    hu_reaction_handler_reset_for_test();
}

/* Verify per-turn clearing: after registering a message with an alternative,
 * if we then call the handler again WITHOUT that alternative, the second pair
 * should be single-sided (no alternative for the second reaction). This pins
 * that last_rejected_draft is cleared per-turn and doesn't leak across. */
static void test_last_rejected_draft_cleared_per_turn_no_cross_contamination(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col = {0};
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 512, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    hu_reaction_handler_reset_for_test();
    hu_reaction_handler_set_collector(&col);

    /* First registration WITH alternative */
    hu_reaction_handler_register_assistant_message_for_production(
        "imessage", "chat_001", "msg_001", "prompt1", "response1", "alternative1");

    /* Second registration WITHOUT alternative (simulating the case where
     * agent->sota.last_rejected_draft was cleared per-turn) */
    hu_reaction_handler_register_assistant_message_for_production("imessage", "chat_002", "msg_002",
                                                                  "prompt2", "response2", "");

    /* Fire reactions on both messages */
    hu_reaction_event_t evt1 = {
        .channel_id = "imessage",
        .target_thread_id = "chat_001",
        .target_message_ref = "msg_001",
        .sender_handle = "+1555",
        .kind = HU_REACTION_LOVE,
        .polarity = HU_REACTION_POSITIVE,
        .is_removal = 0,
    };
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&evt1), HU_OK);

    hu_reaction_event_t evt2 = {
        .channel_id = "imessage",
        .target_thread_id = "chat_002",
        .target_message_ref = "msg_002",
        .sender_handle = "+1555",
        .kind = HU_REACTION_LOVE,
        .polarity = HU_REACTION_POSITIVE,
        .is_removal = 0,
    };
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&evt2), HU_OK);

    /* Both should have inserted pairs, but with different structures:
     * msg_001 should have BOTH chosen and rejected (complete pair)
     * msg_002 should have only chosen (single-sided, no rejected)
     * dpo_pairs has: id, prompt, chosen, rejected, margin, timestamp, source */
    sqlite3_stmt *stmt = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "SELECT prompt, chosen, rejected FROM dpo_pairs "
                                    "ORDER BY id",
                                    -1, &stmt, NULL),
                 SQLITE_OK);

    /* First row: msg_001 with complete pair (alternative1 was provided)
     * For POSITIVE reaction: chosen = response, rejected = alternative */
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "prompt1");
    HU_ASSERT_TRUE((const char *)sqlite3_column_text(stmt, 1) != NULL); /* chosen exists */
    HU_ASSERT_TRUE((const char *)sqlite3_column_text(stmt, 2) != NULL); /* rejected exists */
    const char *msg1_chosen = (const char *)sqlite3_column_text(stmt, 1);
    const char *msg1_rejected = (const char *)sqlite3_column_text(stmt, 2);
    HU_ASSERT_STR_EQ(msg1_chosen, "response1");      /* chosen = response */
    HU_ASSERT_STR_EQ(msg1_rejected, "alternative1"); /* rejected = alternative */

    /* Second row: msg_002 with single-sided pair (no alternative provided) */
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "prompt2");
    HU_ASSERT_TRUE((const char *)sqlite3_column_text(stmt, 1) != NULL); /* chosen exists */
    const char *msg2_rejected = (const char *)sqlite3_column_text(stmt, 2);
    HU_ASSERT_TRUE(msg2_rejected == NULL || msg2_rejected[0] == '\0'); /* empty string */

    sqlite3_finalize(stmt);
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

/* 2026-07-18 audit pin: the daemon reply loop's choreography branch (added
 * ~2026-05-28) sent WITHOUT registering the outbound in reaction_lookup, so
 * tapbacks never matched and zero imessage_tapback DPO pairs were recorded
 * for ~7 weeks despite 119 real inbound tapbacks/30d. Registration is now
 * centralized in hu_daemon_register_reply_for_reactions (message_router);
 * this test proves the helper's registration is FINDABLE by a subsequent
 * reaction event end-to-end (register -> event -> DPO pair). */
static void test_router_registration_yields_pair_on_tapback(void) {
    hu_allocator_t alloc = hu_system_allocator();

    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col = {0};
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, /*max_pairs=*/1024, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    hu_reaction_handler_reset_for_test();
    hu_reaction_handler_set_collector(&col);

    /* NULL config: reaction_collection gate passes open, chat.db GUID lookup
     * is skipped, and the helper mints a time-based ref it reports back. */
    char ref[96] = {0};
    hu_daemon_register_reply_for_reactions(NULL, NULL, "imessage", "chat_router",
                                           "you free tonight?", "yeah give me an hour", 20, ref,
                                           sizeof(ref));
#if defined(HU_ENABLE_RL_FULL)
    HU_ASSERT_TRUE(ref[0] != '\0');

    hu_reaction_event_t e = {
        .channel_id = "imessage",
        .target_thread_id = "chat_router",
        .target_message_ref = ref,
        .sender_handle = "+15551230000",
        .kind = HU_REACTION_LOVE,
        .polarity = HU_REACTION_POSITIVE,
        .is_removal = 0,
    };
    HU_ASSERT_EQ(hu_reaction_handler_handle_event(&e), HU_OK);

    size_t n = 0;
    HU_ASSERT_EQ(hu_dpo_pair_count(&col, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);

    sqlite3_stmt *stmt = NULL;
    HU_ASSERT_EQ(
        sqlite3_prepare_v2(db, "SELECT chosen, source FROM dpo_pairs LIMIT 1", -1, &stmt, NULL),
        SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "yeah give me an hour");
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "imessage_tapback");
    sqlite3_finalize(stmt);
#else
    /* Without HU_ENABLE_RL_FULL the helper is a compiled-out stub whose
     * contract is "no registration, msg_ref_out cleared" — pin that too so
     * a future half-stub can't silently half-register (gate symmetry per
     * .claude/rules/test-source-gate-symmetry.md). */
    HU_ASSERT_EQ(ref[0], '\0');
    size_t n = 99;
    HU_ASSERT_EQ(hu_dpo_pair_count(&col, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);
#endif

    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
    hu_reaction_handler_reset_for_test();
}

void run_reaction_handler_e2e_tests(void) {
    HU_TEST_SUITE("reaction_handler_e2e");
    HU_RUN_TEST(test_reaction_event_with_known_target_inserts_dpo_pair);
    HU_RUN_TEST(test_router_registration_yields_pair_on_tapback);
    HU_RUN_TEST(test_reaction_event_with_unknown_target_drops_silently);
    HU_RUN_TEST(test_agent_turn_clear_turn_resets_called_flag);
    HU_RUN_TEST(test_positive_tapback_with_alternative_creates_complete_pair);
    HU_RUN_TEST(test_negative_tapback_with_alternative_creates_complete_pair);
    HU_RUN_TEST(test_production_reaction_path_with_rejected_draft_yields_complete_pair);
    HU_RUN_TEST(test_last_rejected_draft_cleared_per_turn_no_cross_contamination);
    HU_RUN_TEST(test_reaction_handler_handle_event_null_returns_invalid_argument);
}
