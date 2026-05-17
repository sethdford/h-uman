/* test_dpo_extractor_integration — end-to-end SQL verification for the
 * auto-DPO extractor.
 *
 * The existing tests in test_training_data_extractor.c cover NULL-arg
 * guards but ALL hit the HU_IS_TEST short-circuit at
 * src/ml/training_data_extractor.c::hu_training_data_extract_dpo — they
 * verify the function returns HU_OK in test mode and nothing more.
 *
 * This file exercises the production SQL path via the new
 * hu_training_data_extract_dpo_from_db inner function, which operates
 * on an already-open sqlite3* (typically :memory:) and bypasses the
 * HU_IS_TEST gate by virtue of having a different entry point.
 */

#include "human/core/error.h"
#include "human/ml/training_data_extractor.h"
#include "test_framework.h"
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

static void create_messages_table(sqlite3 *db) {
    const char *ddl =
        "CREATE TABLE messages("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "session_id TEXT NOT NULL,role TEXT NOT NULL,"
        "content TEXT NOT NULL,"
        "created_at TEXT DEFAULT(datetime('now')))";
    char *err = NULL;
    int rc = sqlite3_exec(db, ddl, NULL, NULL, &err);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    if (err)
        sqlite3_free(err);
}

static void insert_msg(sqlite3 *db, const char *session, const char *role, const char *content,
                       const char *created_at) {
    const char *sql =
        "INSERT INTO messages(session_id, role, content, created_at) VALUES(?,?,?,?)";
    sqlite3_stmt *stmt = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, session, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, role, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, content, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, created_at, -1, SQLITE_STATIC);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
}

static int count_dpo_pairs(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM dpo_pairs", -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    int n = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

static bool first_dpo_row(sqlite3 *db, char *prompt, size_t pcap, char *chosen, size_t ccap,
                          char *rejected, size_t rcap) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT prompt, chosen, rejected FROM dpo_pairs ORDER BY id LIMIT 1",
                           -1, &stmt, NULL) != SQLITE_OK)
        return false;
    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *p = sqlite3_column_text(stmt, 0);
        const unsigned char *c = sqlite3_column_text(stmt, 1);
        const unsigned char *r = sqlite3_column_text(stmt, 2);
        if (p) snprintf(prompt, pcap, "%s", (const char *)p);
        if (c) snprintf(chosen, ccap, "%s", (const char *)c);
        if (r) snprintf(rejected, rcap, "%s", (const char *)r);
        ok = true;
    }
    sqlite3_finalize(stmt);
    return ok;
}

static void dpo_extractor_extracts_one_correction_triple(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    create_messages_table(db);
    insert_msg(db, "s1", "user", "What's the capital of France?", "2026-05-16 10:00:00");
    insert_msg(db, "s1", "assistant", "The capital of France is Berlin.", "2026-05-16 10:00:05");
    insert_msg(db, "s1", "user", "No, it's Paris. Try again.", "2026-05-16 10:00:30");

    size_t pairs = 999;
    HU_ASSERT_EQ(hu_training_data_extract_dpo_from_db(db, 300, &pairs), HU_OK);
    HU_ASSERT_EQ((long)pairs, 1L);
    HU_ASSERT_EQ(count_dpo_pairs(db), 1);

    char prompt[256], chosen[256], rejected[256];
    HU_ASSERT_TRUE(first_dpo_row(db, prompt, sizeof(prompt), chosen, sizeof(chosen), rejected,
                                 sizeof(rejected)));
    HU_ASSERT_STR_EQ(prompt, "What's the capital of France?");
    HU_ASSERT_STR_EQ(chosen, "No, it's Paris. Try again.");
    HU_ASSERT_STR_EQ(rejected, "The capital of France is Berlin.");
    sqlite3_close(db);
}

static void dpo_extractor_skips_correction_outside_window(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    create_messages_table(db);
    insert_msg(db, "s1", "user", "first question", "2026-05-16 10:00:00");
    insert_msg(db, "s1", "assistant", "wrong answer", "2026-05-16 10:00:05");
    insert_msg(db, "s1", "user", "still thinking", "2026-05-16 10:10:05");
    size_t pairs = 999;
    HU_ASSERT_EQ(hu_training_data_extract_dpo_from_db(db, 300, &pairs), HU_OK);
    HU_ASSERT_EQ((long)pairs, 0L);
    HU_ASSERT_EQ(count_dpo_pairs(db), 0);
    sqlite3_close(db);
}

static void dpo_extractor_handles_multiple_sessions_independently(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    create_messages_table(db);
    insert_msg(db, "alpha", "user", "q-alpha-1", "2026-05-16 10:00:00");
    insert_msg(db, "alpha", "assistant", "a-alpha-1-wrong", "2026-05-16 10:00:05");
    insert_msg(db, "alpha", "user", "correction-alpha", "2026-05-16 10:00:20");
    insert_msg(db, "beta", "user", "q-beta-1", "2026-05-16 11:00:00");
    insert_msg(db, "beta", "assistant", "a-beta-1-wrong", "2026-05-16 11:00:05");
    insert_msg(db, "beta", "user", "correction-beta", "2026-05-16 11:00:20");
    size_t pairs = 0;
    HU_ASSERT_EQ(hu_training_data_extract_dpo_from_db(db, 300, &pairs), HU_OK);
    HU_ASSERT_EQ((long)pairs, 2L);
    HU_ASSERT_EQ(count_dpo_pairs(db), 2);
    sqlite3_close(db);
}

static void dpo_extractor_is_idempotent_across_runs(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    create_messages_table(db);
    insert_msg(db, "s1", "user", "q1", "2026-05-16 10:00:00");
    insert_msg(db, "s1", "assistant", "a1-wrong", "2026-05-16 10:00:05");
    insert_msg(db, "s1", "user", "correction", "2026-05-16 10:00:20");
    size_t first = 0, second = 0;
    HU_ASSERT_EQ(hu_training_data_extract_dpo_from_db(db, 300, &first), HU_OK);
    HU_ASSERT_EQ((long)first, 1L);
    HU_ASSERT_EQ(hu_training_data_extract_dpo_from_db(db, 300, &second), HU_OK);
    HU_ASSERT_EQ((long)second, 0L);
    HU_ASSERT_EQ(count_dpo_pairs(db), 1);
    sqlite3_close(db);
}

static void dpo_extractor_empty_database_returns_zero(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    create_messages_table(db);
    size_t pairs = 999;
    HU_ASSERT_EQ(hu_training_data_extract_dpo_from_db(db, 300, &pairs), HU_OK);
    HU_ASSERT_EQ((long)pairs, 0L);
    sqlite3_close(db);
}

static void dpo_extractor_missing_messages_table_returns_io_error(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    size_t pairs = 999;
    HU_ASSERT_EQ(hu_training_data_extract_dpo_from_db(db, 300, &pairs), HU_ERR_IO);
    sqlite3_close(db);
}

static void dpo_extractor_rejects_null_args(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    size_t pairs = 0;
    HU_ASSERT_EQ(hu_training_data_extract_dpo_from_db(NULL, 300, &pairs), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_training_data_extract_dpo_from_db(db, 300, NULL), HU_ERR_INVALID_ARGUMENT);
    sqlite3_close(db);
}

static void dpo_extractor_default_window_applied_for_nonpositive(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    create_messages_table(db);
    insert_msg(db, "s1", "user", "q1", "2026-05-16 10:00:00");
    insert_msg(db, "s1", "assistant", "a1", "2026-05-16 10:00:05");
    insert_msg(db, "s1", "user", "corr", "2026-05-16 10:01:05");
    size_t pairs = 0;
    HU_ASSERT_EQ(hu_training_data_extract_dpo_from_db(db, 0, &pairs), HU_OK);
    HU_ASSERT_EQ((long)pairs, 1L);
    sqlite3_close(db);
}
#endif /* HU_ENABLE_SQLITE */

void run_dpo_extractor_integration_tests(void) {
    HU_TEST_SUITE("dpo_extractor_integration");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(dpo_extractor_extracts_one_correction_triple);
    HU_RUN_TEST(dpo_extractor_skips_correction_outside_window);
    HU_RUN_TEST(dpo_extractor_handles_multiple_sessions_independently);
    HU_RUN_TEST(dpo_extractor_is_idempotent_across_runs);
    HU_RUN_TEST(dpo_extractor_empty_database_returns_zero);
    HU_RUN_TEST(dpo_extractor_missing_messages_table_returns_io_error);
    HU_RUN_TEST(dpo_extractor_rejects_null_args);
    HU_RUN_TEST(dpo_extractor_default_window_applied_for_nonpositive);
#endif
}
