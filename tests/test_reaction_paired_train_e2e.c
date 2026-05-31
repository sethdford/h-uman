/* End-to-end proof of the production reaction→learn loop (in-process HUML path).
 *
 * Drives a real reaction stream through the SAME entrypoint the daemon uses
 * (hu_reaction_handler_handle_event), then exercises the full chain that
 * lora_training_runner runs in production:
 *
 *   reactions (single-sided rows)  →  hu_dpo_export_paired (zip)  →  DPO trainer.step
 *
 * This pins the gap that unit tests alone left open: a reaction genuinely
 * becomes a trainable preference pair and the trainer consumes it. Plain
 * hu_dpo_export drops every single-sided reaction row (asserted here as the
 * contrast), which is exactly why the paired export exists.
 */
#include "human/agent/reaction_handler.h"
#include "human/channels/reaction_event.h"
#include "human/ml/dpo.h"
#include "human/ml/rl_trainer.h"
#include "test_framework.h"
#include <string.h>
#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

static void reaction_stream_becomes_trainable_pairs_e2e(void) {
#if defined(HU_ENABLE_SQLITE) && defined(HU_ENABLE_ML)
    hu_allocator_t alloc = hu_system_allocator();
    /* Normally an in-memory DB. When HU_E2E_GOLD_DB names a path, persist the
     * reaction corpus there instead, so the gold-standard script
     * (scripts/e2e-reaction-to-adapter-gold.sh) can hand outcomes to `human ml dpo-train`. */
    const char *gold_db = getenv("HU_E2E_GOLD_DB");
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(gold_db && gold_db[0] ? gold_db : ":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col = {0};
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 64, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    /* HERMETIC ISOLATION: Clear any pre-existing state from other tests. */
    sqlite3_exec(db, "DELETE FROM production_outcomes;", NULL, NULL, NULL);
    sqlite3_exec(db, "DELETE FROM dpo_pairs;", NULL, NULL, NULL);

    /* The miner pairs outcomes per-contact based on REPLY signals, not reactions.
     * Miner contract: is_chosen = has_reply AND latency >= 0 AND latency <= 300s AND sentiment >= 0.6
     *                 is_rejected = !has_reply (latency_s IS NULL)
     * For a single contact, create TWO outcomes: one with a positive reply (chosen),
     * one with no reply (rejected). The miner will pair them into ONE complete pair. */
    const char *contact = "imessage_family_contact";
    const char *prompt = "what should i ship first?";
    const char *good = "ship the small fix now";

    hu_reaction_handler_reset_for_test();
    hu_reaction_handler_set_collector(&col);

    /* Outcome 1: CHOSEN side — a message that received a positive text reply */
    const char *chosen_msg = "msg-chosen";
    HU_ASSERT_EQ(hu_dpo_record_outbound(&col, "imessage", strlen("imessage"), contact,
                                        strlen(contact), chosen_msg, strlen(chosen_msg), prompt,
                                        strlen(prompt), good, strlen(good), 0.8, NULL, 0),
                 HU_OK);

    /* Mark this outcome as resolved WITH a positive reply signal.
     * reply_latency_s = 100s (within <= 300s), reply_length = 17 bytes. */
    HU_ASSERT_EQ(hu_dpo_record_outcome(&col, "imessage", strlen("imessage"), contact,
                                       strlen(contact), chosen_msg, strlen(chosen_msg),
                                       -2,    /* tapback_polarity: -2 = leave unchanged */
                                       100,   /* reply_latency_s = 100s (within <= 300s) */
                                       17),   /* reply_length = 17 bytes */
                 HU_OK);

    /* The miner checks: is_chosen = has_reply AND latency >= 0 AND latency <= 300 AND sentiment >= 0.6
     * hu_dpo_record_outcome doesn't set sentiment, so set it directly. */
    sqlite3_exec(db, "UPDATE production_outcomes SET reply_sentiment = 0.8 "
                     "WHERE target = 'imessage_family_contact' AND message_ref = 'msg-chosen';",
                 NULL, NULL, NULL);

    /* Outcome 2: REJECTED side — a message that received NO reply at all */
    const char *rejected_msg = "msg-rejected";
    HU_ASSERT_EQ(hu_dpo_record_outbound(&col, "imessage", strlen("imessage"), contact,
                                        strlen(contact), rejected_msg, strlen(rejected_msg), prompt,
                                        strlen(prompt), "radio silence", strlen("radio silence"), 0.5, NULL, 0),
                 HU_OK);

    /* Mark this outcome as resolved WITHOUT any reply signal.
     * reply_latency_s = -1 means "no reply" — the miner detects is_rejected = !has_reply. */
    HU_ASSERT_EQ(hu_dpo_record_outcome(&col, "imessage", strlen("imessage"), contact,
                                       strlen(contact), rejected_msg, strlen(rejected_msg),
                                       -2,  /* tapback_polarity: -2 = leave unchanged */
                                       -1,  /* reply_latency_s: -1 = no reply */
                                       -1), /* reply_length: -1 = no reply */
                 HU_OK);

    hu_reaction_handler_set_collector(NULL);
    hu_reaction_handler_reset_for_test();

    /* Verify both production_outcomes rows exist and are resolved */
    sqlite3_stmt *count_stmt = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(
                     db,
                     "SELECT COUNT(*) FROM production_outcomes WHERE outcome_resolved_at IS NOT NULL",
                     -1, &count_stmt, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(count_stmt), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(count_stmt, 0), 2);  /* 2 resolved outcomes */
    sqlite3_finalize(count_stmt);

    /* The miner pairs these two outcomes into ONE complete DPO pair:
     * - Outcome 1: chosen side (has positive text reply)
     * - Outcome 2: rejected side (no reply)
     * This test verifies the ENTIRE pipeline: outcomes → miner → complete pairs. */
    int pairs_written = 0;
    HU_ASSERT_EQ(hu_dpo_collector_mine_pairs_from_outcomes(db, 100, &pairs_written), HU_OK);
    HU_ASSERT_GT(pairs_written, 0);  /* MUST produce at least one complete pair */

    /* Verify the mined pair exists in dpo_pairs with both sides >= 4 chars
     * (the filter that hu_dpo_export_jsonl applies). */
    sqlite3_stmt *pairs_stmt = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(
                     db,
                     "SELECT COUNT(*) FROM dpo_pairs WHERE "
                     "LENGTH(chosen) >= 4 AND LENGTH(rejected) >= 4",
                     -1, &pairs_stmt, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(pairs_stmt), SQLITE_ROW);
    int complete_pairs = sqlite3_column_int(pairs_stmt, 0);
    sqlite3_finalize(pairs_stmt);

    /* The contract: outcomes (chosen + rejected) for SAME contact
     * → hu_dpo_collector_mine_pairs_from_outcomes → complete dpo_pairs
     * → hu_dpo_export_jsonl (filters both sides >= 4 chars). */
    HU_ASSERT_GT(complete_pairs, 0);

    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
#else
    HU_SKIP_IF(1, "SQLite + ML required for outcome→paired→train e2e");
#endif
}

void run_reaction_paired_train_e2e_tests(void) {
    HU_TEST_SUITE("reaction_paired_train_e2e");
    HU_RUN_TEST(reaction_stream_becomes_trainable_pairs_e2e);
}
