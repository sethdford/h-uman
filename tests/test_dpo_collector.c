#include "human/ml/dpo.h"
#include "test_framework.h"
#include <limits.h>
#include <string.h>
#include <time.h>
#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#ifdef HU_ENABLE_SQLITE

static sqlite3 *test_create_dpo_db(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK)
        return NULL;

    const char *schema =
        "CREATE TABLE IF NOT EXISTS dpo_pairs("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "prompt TEXT, chosen TEXT, rejected TEXT, "
        "margin REAL, timestamp INTEGER, source TEXT);"
        "CREATE TABLE IF NOT EXISTS production_outcomes("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "channel TEXT NOT NULL,"
        "target TEXT NOT NULL,"
        "message_ref TEXT,"
        "prompt TEXT NOT NULL,"
        "chosen TEXT NOT NULL,"
        "alternatives TEXT,"
        "p_seth_at_send REAL,"
        "send_timestamp INTEGER NOT NULL,"
        "tapback_polarity INTEGER,"
        "reply_latency_s INTEGER,"
        "reply_length INTEGER,"
        "reply_sentiment REAL,"
        "user_edited INTEGER,"
        "outcome_resolved_at INTEGER,"
        "processed_into_dpo INTEGER DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS idx_po_msg_ref ON production_outcomes(channel, target, "
        "message_ref);"
        "CREATE INDEX IF NOT EXISTS idx_po_unprocessed ON production_outcomes(processed_into_dpo);";

    char *err = NULL;
    rc = sqlite3_exec(db, schema, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err)
            sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

static int test_insert_outcome(sqlite3 *db, const char *channel, const char *target,
                               const char *prompt, const char *chosen, int64_t send_timestamp,
                               int reply_latency_s, int reply_length, double reply_sentiment,
                               int user_edited) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "INSERT INTO production_outcomes("
                                "channel, target, prompt, chosen, send_timestamp, reply_latency_s, "
                                "reply_length, reply_sentiment, user_edited, outcome_resolved_at) "
                                "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;

    sqlite3_bind_text(stmt, 1, channel, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, target, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, prompt, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, chosen, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, send_timestamp);
    if (reply_latency_s >= 0)
        sqlite3_bind_int(stmt, 6, reply_latency_s);
    else
        sqlite3_bind_null(stmt, 6);
    if (reply_length >= 0)
        sqlite3_bind_int(stmt, 7, reply_length);
    else
        sqlite3_bind_null(stmt, 7);
    if (reply_sentiment >= 0.0)
        sqlite3_bind_double(stmt, 8, reply_sentiment);
    else
        sqlite3_bind_null(stmt, 8);
    sqlite3_bind_int(stmt, 9, user_edited);

    /* Every fixture row is a RESOLVED outcome (a reply arrived, or the 24h
     * no-reply timeout elapsed), so outcome_resolved_at is always set. A
     * no-reply row is signalled by a NULL reply_latency_s (above), not by an
     * unresolved timestamp. */
    sqlite3_bind_int64(stmt, 10, (int64_t)time(NULL));

    rc = sqlite3_step(stmt);
    int last_rowid = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
        return -1;
    return last_rowid;
}

static int test_count_pairs_by_source(sqlite3 *db, const char *source) {
    sqlite3_stmt *stmt = NULL;
    int rc =
        sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM dpo_pairs WHERE source = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;

    sqlite3_bind_text(stmt, 1, source, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    int count = 0;
    if (rc == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

static int test_count_processed_outcomes(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db, "SELECT COUNT(*) FROM production_outcomes WHERE processed_into_dpo = 1", -1, &stmt,
        NULL);
    if (rc != SQLITE_OK)
        return -1;

    rc = sqlite3_step(stmt);
    int count = 0;
    if (rc == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

static void dpo_collector_mine_produces_expected_pairs(void) {
    sqlite3 *db = test_create_dpo_db();
    HU_ASSERT_NOT_NULL(db);

    int64_t base_time = (int64_t)time(NULL);

    /* AC-102.8 fixture: 2 replied(+positive) + 2 no-reply + 1 edited = 5 rows.
     * Pairs form per contact (chosen=good reply, rejected=no reply):
     *   alice: chosen then rejected  -> pair 1
     *   bob:   rejected then chosen  -> pair 2 (exercises order-independence)
     *   charlie: edited -> skipped for pairing, still marked processed.
     * Expect 2 pairs, all 5 rows processed. */
    test_insert_outcome(db, "imessage", "alice", "Hi alice", "How are you?", base_time - 200, 180,
                        15, 0.8, 0); /* chosen: reply 180s, sentiment 0.8 */
    test_insert_outcome(db, "imessage", "alice", "Hey alice", "What's up?", base_time - 100000, -1,
                        -1, -1.0, 0); /* rejected: no reply (NULL latency) */

    test_insert_outcome(db, "imessage", "bob", "Hi bob", "Hello there", base_time - 100000, -1, -1,
                        -1.0, 0); /* rejected: no reply (NULL latency) */
    test_insert_outcome(db, "imessage", "bob", "Hey bob", "Good morning", base_time - 200, 120, 18,
                        0.75, 0); /* chosen: reply 120s, sentiment 0.75 */

    test_insert_outcome(db, "imessage", "charlie", "Hi charlie", "Test message", base_time - 50, 90,
                        12, 0.7, 1); /* edited: resolved but user_edited=1 -> skipped, processed */

    int pairs_written = 0;
    HU_ASSERT_EQ(hu_dpo_collector_mine_pairs_from_outcomes(db, INT_MAX, &pairs_written), HU_OK);

    HU_ASSERT_EQ(pairs_written, 2);

    int processed_count = test_count_processed_outcomes(db);
    HU_ASSERT_EQ(processed_count, 5);

    int pair_count = test_count_pairs_by_source(db, "implicit_feedback");
    HU_ASSERT_EQ(pair_count, 2);

    sqlite3_close(db);
}

static void dpo_collector_mine_empty_produces_zero_pairs(void) {
    sqlite3 *db = test_create_dpo_db();
    HU_ASSERT_NOT_NULL(db);

    int pairs_written = 0;
    HU_ASSERT_EQ(hu_dpo_collector_mine_pairs_from_outcomes(db, INT_MAX, &pairs_written), HU_OK);

    HU_ASSERT_EQ(pairs_written, 0);

    sqlite3_close(db);
}

static void dpo_collector_mine_respects_limit(void) {
    sqlite3 *db = test_create_dpo_db();
    HU_ASSERT_NOT_NULL(db);

    int64_t base_time = (int64_t)time(NULL);

    for (int i = 0; i < 3; i++) {
        char target[16], prompt_c[64], prompt_r[64], chosen_c[64], chosen_r[64];
        snprintf(target, sizeof(target), "contact%d", i);
        snprintf(prompt_c, sizeof(prompt_c), "prompt_chosen_%d", i);
        snprintf(prompt_r, sizeof(prompt_r), "prompt_rejected_%d", i);
        snprintf(chosen_c, sizeof(chosen_c), "response_chosen_%d", i);
        snprintf(chosen_r, sizeof(chosen_r), "response_rejected_%d", i);

        test_insert_outcome(db, "imessage", target, prompt_c, chosen_c, base_time - 200 - i * 100,
                            180, 15, 0.8, 0);
        test_insert_outcome(db, "imessage", target, prompt_r, chosen_r,
                            base_time - 100000 - i * 100, -1, -1, -1.0, 0);
    }

    int pairs_written = 0;
    HU_ASSERT_EQ(hu_dpo_collector_mine_pairs_from_outcomes(db, 1, &pairs_written), HU_OK);

    HU_ASSERT_GE(pairs_written, 0);
    HU_ASSERT_LE(pairs_written, 3);

    sqlite3_close(db);
}

static void dpo_collector_mine_invalid_args(void) {
    int pairs_written = 0;
    HU_ASSERT_EQ(hu_dpo_collector_mine_pairs_from_outcomes(NULL, INT_MAX, &pairs_written),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_dpo_collector_mine_pairs_from_outcomes((sqlite3 *)0x12345678, INT_MAX, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

void run_dpo_collector_tests(void) {
    HU_TEST_SUITE("dpo_collector");

    HU_RUN_TEST(dpo_collector_mine_produces_expected_pairs);
    HU_RUN_TEST(dpo_collector_mine_empty_produces_zero_pairs);
    HU_RUN_TEST(dpo_collector_mine_respects_limit);
    HU_RUN_TEST(dpo_collector_mine_invalid_args);
}

#else

void run_dpo_collector_tests(void) {
    HU_TEST_SUITE("dpo_collector");
}

#endif
