/* src/memory/repos/chatdb_cursor_repo_sqlite.c — hu_chatdb_max_rowid.
 *
 * The doctor's cursor-gap check compares ~/.human/imessage.rowid with
 * MAX(ROWID) of chat.db's message table; this pins the repo call it uses.
 * Landed 2026-09-02 because CI's check-untested.sh refused the source. */
#include "human/core/error.h"
#include "human/memory/chatdb_cursor_repo.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void test_chatdb_max_rowid_rejects_null_and_empty_args(void) {
    int64_t out = -1;
    HU_ASSERT_EQ(hu_chatdb_max_rowid(NULL, &out), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_chatdb_max_rowid("", &out), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_chatdb_max_rowid("/tmp/whatever.db", NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(out, -1); /* untouched on rejection */
}

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

static void make_db_path(char *buf, size_t cap, const char *tag) {
    snprintf(buf, cap, "/tmp/hu_test_chatdb_cursor_%s_%ld.db", tag, (long)getpid());
    (void)unlink(buf);
}

static void test_chatdb_max_rowid_reads_max_of_message_table(void) {
    char path[256];
    make_db_path(path, sizeof(path), "rows");
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(path, &db), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "CREATE TABLE message (ROWID INTEGER PRIMARY KEY, text TEXT);"
                              "INSERT INTO message (ROWID, text) VALUES (5, 'a');"
                              "INSERT INTO message (ROWID, text) VALUES (70508, 'b');"
                              "INSERT INTO message (ROWID, text) VALUES (12, 'c');",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    sqlite3_close(db);

    int64_t out = -1;
    HU_ASSERT_EQ(hu_chatdb_max_rowid(path, &out), HU_OK);
    HU_ASSERT_EQ(out, 70508); /* the max, not the last insert */
    (void)unlink(path);
}

static void test_chatdb_max_rowid_empty_table_is_zero(void) {
    char path[256];
    make_db_path(path, sizeof(path), "empty");
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(path, &db), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_exec(db, "CREATE TABLE message (ROWID INTEGER PRIMARY KEY, text TEXT);",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    sqlite3_close(db);

    int64_t out = -1;
    HU_ASSERT_EQ(hu_chatdb_max_rowid(path, &out), HU_OK);
    HU_ASSERT_EQ(out, 0); /* COALESCE(MAX(ROWID), 0) */
    (void)unlink(path);
}

static void test_chatdb_max_rowid_without_message_table_is_io_error(void) {
    char path[256];
    make_db_path(path, sizeof(path), "notable");
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(path, &db), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_exec(db, "CREATE TABLE other (x INTEGER);", NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(db);

    int64_t out = -1;
    HU_ASSERT_EQ(hu_chatdb_max_rowid(path, &out), HU_ERR_IO);
    HU_ASSERT_EQ(out, -1);
    (void)unlink(path);
}

static void test_chatdb_max_rowid_missing_file_is_io_error(void) {
    int64_t out = -1;
    HU_ASSERT_EQ(hu_chatdb_max_rowid("/tmp/hu_test_chatdb_cursor_does_not_exist.db", &out),
                 HU_ERR_IO);
    HU_ASSERT_EQ(out, -1);
}
#endif /* HU_ENABLE_SQLITE */

void run_chatdb_cursor_repo_sqlite_tests(void) {
    HU_TEST_SUITE("chat.db cursor repo");
    HU_RUN_TEST(test_chatdb_max_rowid_rejects_null_and_empty_args);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_chatdb_max_rowid_reads_max_of_message_table);
    HU_RUN_TEST(test_chatdb_max_rowid_empty_table_is_zero);
    HU_RUN_TEST(test_chatdb_max_rowid_without_message_table_is_io_error);
    HU_RUN_TEST(test_chatdb_max_rowid_missing_file_is_io_error);
#endif
}
