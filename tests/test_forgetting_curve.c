#include "human/memory/forgetting_curve.h"
#include "test_framework.h"
#include <math.h>
#include <stdint.h>
#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

/* --- Pure computation (no DB, but implementation is in a SQLite-guarded file) --- */
#ifdef HU_ENABLE_SQLITE
static void forgetting_decayed_salience_30_days_rate_01_expected_025(void) {
    int64_t created = 0;
    int64_t now = 30 * 86400; /* 30 days later */
    double s = hu_forgetting_decayed_salience(0.5, 0.1, created, now, false);
    /* 0.5 * exp(-0.1 * 30) = 0.5 * exp(-3) ≈ 0.5 * 0.0498 ≈ 0.025 */
    HU_ASSERT_FLOAT_EQ(s, 0.025, 0.003);
}

static void forgetting_decayed_salience_emotional_anchor_slower_decay(void) {
    int64_t created = 0;
    int64_t now = 30 * 86400;
    double normal = hu_forgetting_decayed_salience(0.5, 0.1, created, now, false);
    double emotional = hu_forgetting_decayed_salience(0.5, 0.1, created, now, true);
    /* Emotional: effective rate 0.03, 0.5 * exp(-0.9) ≈ 0.20 */
    HU_ASSERT_TRUE(emotional > normal);
    HU_ASSERT_FLOAT_EQ(normal, 0.025, 0.003);
    HU_ASSERT_FLOAT_EQ(emotional, 0.20, 0.03);
}

static void forgetting_decayed_salience_zero_days_unchanged(void) {
    int64_t ts = 1700000000;
    double s = hu_forgetting_decayed_salience(0.8, 0.1, ts, ts, false);
    HU_ASSERT_FLOAT_EQ(s, 0.8, 0.001);
}

static void forgetting_decayed_salience_zero_initial_returns_zero(void) {
    double s = hu_forgetting_decayed_salience(0.0, 0.1, 0, 86400, false);
    HU_ASSERT_FLOAT_EQ(s, 0.0, 0.001);
}

static void forgetting_batch_decay_scores_decrease(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT_NOT_NULL(db);

    /* Create episodes table with salience_score, impact_score, created_at */
    const char *create =
        "CREATE TABLE episodes ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "summary TEXT,"
        "impact_score REAL DEFAULT 0.5,"
        "salience_score REAL NOT NULL,"
        "created_at INTEGER NOT NULL)";
    rc = sqlite3_exec(db, create, NULL, NULL, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);

    int64_t now = 100 * 86400; /* day 100 */
    int64_t day30 = now - 30 * 86400;
    int64_t day10 = now - 10 * 86400;

    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(db,
                            "INSERT INTO episodes (summary, impact_score, salience_score, created_at) "
                            "VALUES (?, ?, ?, ?)",
                            -1, &ins, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_bind_text(ins, 1, "ep1", 3, SQLITE_STATIC);
    sqlite3_bind_double(ins, 2, 0.5);
    sqlite3_bind_double(ins, 3, 0.5);
    sqlite3_bind_int64(ins, 4, day30);
    rc = sqlite3_step(ins);
    HU_ASSERT_EQ(rc, SQLITE_DONE);
    sqlite3_reset(ins);

    sqlite3_bind_text(ins, 1, "ep2", 3, SQLITE_STATIC);
    sqlite3_bind_double(ins, 2, 0.5);
    sqlite3_bind_double(ins, 3, 0.5);
    sqlite3_bind_int64(ins, 4, day10);
    rc = sqlite3_step(ins);
    HU_ASSERT_EQ(rc, SQLITE_DONE);
    sqlite3_reset(ins);

    sqlite3_bind_text(ins, 1, "ep3", 3, SQLITE_STATIC);
    sqlite3_bind_double(ins, 2, 0.9); /* emotional anchor */
    sqlite3_bind_double(ins, 3, 0.5);
    sqlite3_bind_int64(ins, 4, day30);
    rc = sqlite3_step(ins);
    HU_ASSERT_EQ(rc, SQLITE_DONE);
    sqlite3_finalize(ins);

    hu_error_t err = hu_forgetting_apply_batch_decay(db, now, 0.1);
    HU_ASSERT_EQ(err, HU_OK);

    sqlite3_stmt *sel = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT id, salience_score FROM episodes ORDER BY id", -1, &sel,
                            NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);

    double s1 = 0, s2 = 0, s3 = 0;
    if (sqlite3_step(sel) == SQLITE_ROW) {
        s1 = sqlite3_column_double(sel, 1);
    }
    if (sqlite3_step(sel) == SQLITE_ROW) {
        s2 = sqlite3_column_double(sel, 1);
    }
    if (sqlite3_step(sel) == SQLITE_ROW) {
        s3 = sqlite3_column_double(sel, 1);
    }
    sqlite3_finalize(sel);

    /* ep1: 30 days, 0.5 * exp(-3) ≈ 0.025 */
    HU_ASSERT_TRUE(s1 < 0.5);
    HU_ASSERT_FLOAT_EQ(s1, 0.025, 0.005);

    /* ep2: 10 days, 0.5 * exp(-1) ≈ 0.184 */
    HU_ASSERT_TRUE(s2 < 0.5);
    HU_ASSERT_TRUE(s2 > s1);

    /* ep3: emotional anchor, 30 days, 0.5 * exp(-0.9) ≈ 0.20 */
    HU_ASSERT_TRUE(s3 < 0.5);
    HU_ASSERT_TRUE(s3 > s1);

    sqlite3_close(db);
}
#endif

void run_forgetting_curve_tests(void) {
    HU_TEST_SUITE("forgetting_curve");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(forgetting_decayed_salience_30_days_rate_01_expected_025);
    HU_RUN_TEST(forgetting_decayed_salience_emotional_anchor_slower_decay);
    HU_RUN_TEST(forgetting_decayed_salience_zero_days_unchanged);
    HU_RUN_TEST(forgetting_decayed_salience_zero_initial_returns_zero);
    HU_RUN_TEST(forgetting_batch_decay_scores_decrease);
#endif
}
