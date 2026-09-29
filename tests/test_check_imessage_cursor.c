/* tests/test_doctor_imessage_cursor.c — the 2026-09-01 replay shape (cursor
 * 69288 vs chat.db 70484) must be a FAIL before any restart. */
#include "human/doctor/check_ops.h"
#include "human/memory/chatdb_cursor_repo.h"
#include "test_framework.h"
#include "test_tmpdir.h"
#include <stdio.h>
#include <string.h>
#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

static void write_text(const char *path, const char *s) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(s, f);
        fclose(f);
    }
}

#ifdef HU_ENABLE_SQLITE
static void make_chatdb(const char *path, long long max_rowid) {
    sqlite3 *db = NULL;
    sqlite3_open(path, &db);
    sqlite3_exec(db, "CREATE TABLE message(ROWID INTEGER PRIMARY KEY, text TEXT);", NULL, NULL,
                 NULL);
    char sql[128];
    snprintf(sql, sizeof(sql), "INSERT INTO message(ROWID,text) VALUES(%lld,'x');", max_rowid);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_close(db);
}
#endif

static void test_no_cursor_is_na(void) {
    char d[512];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu_cursor", d, sizeof(d)));
    char rp[600], cp[600];
    snprintf(rp, sizeof(rp), "%s/imessage.rowid", d);
    snprintf(cp, sizeof(cp), "%s/chat.db", d);
    hu_doctor_imessage_cursor_ctx_t ctx = {rp, cp, 50};
    hu_doctor_check_t c = hu_doctor_check_imessage_cursor;
    hu_doctor_check_result_t r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_NA);
    hu_test_rm_rf(d);
}

#ifdef HU_ENABLE_SQLITE
static void test_replay_gap_fails_small_gap_passes(void) {
    char d[512];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu_cursor", d, sizeof(d)));
    char rp[600], cp[600];
    snprintf(rp, sizeof(rp), "%s/imessage.rowid", d);
    snprintf(cp, sizeof(cp), "%s/chat.db", d);
    make_chatdb(cp, 70484);
    int64_t direct = 0;
    HU_ASSERT_EQ(hu_chatdb_max_rowid(cp, &direct), HU_OK);
    HU_ASSERT_EQ((int)direct, 70484);
    write_text(rp, "69288\n");
    hu_doctor_imessage_cursor_ctx_t ctx = {rp, cp, 50};
    hu_doctor_check_t c = hu_doctor_check_imessage_cursor;
    hu_doctor_check_result_t r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_FAIL);
    HU_ASSERT_STR_CONTAINS(r.reason, "lags chat.db by 1196 rows");
    HU_ASSERT_STR_CONTAINS(r.detail_json, "\"gap\":1196");
    write_text(rp, "70470");
    r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_PASS);
    HU_ASSERT_STR_CONTAINS(r.detail_json, "\"gap\":14");
    /* unreadable chat.db → NA, never a false alarm */
    write_text(cp, "not a database");
    r = c.run(&c, &ctx);
    HU_ASSERT_EQ(r.verdict, HU_DOCTOR_NA);
    hu_test_rm_rf(d);
}
#endif

void run_doctor_imessage_cursor_tests(void) {
    HU_TEST_SUITE("doctor_imessage_cursor");
    HU_RUN_TEST(test_no_cursor_is_na);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_replay_gap_fails_small_gap_passes);
#endif
}
