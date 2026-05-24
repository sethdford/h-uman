/* Spec 2026-05-24-action-layers — directive-emitter tests.
 *
 * Verifies AC-AL-1, AC-AL-2, AC-AL-3, AC-AL-5 from the spec. */

#include "test_framework.h"

#if defined(HU_ENABLE_SQLITE) && defined(HU_ENABLE_ACTION_LAYERS)

#include "human/agent/action_directives.h"
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static sqlite3 *make_db_with_schema(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "CREATE TABLE agent_self_concerns ("
                              "id INTEGER PRIMARY KEY,"
                              "observation_id INTEGER,"
                              "dimension TEXT,"
                              "magnitude_sigma REAL,"
                              "window_n_turns INTEGER,"
                              "created_ts_ms INTEGER)",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "CREATE TABLE tom_user_expectations ("
                              "id INTEGER PRIMARY KEY,"
                              "contact_id TEXT NOT NULL,"
                              "topic TEXT NOT NULL,"
                              "expected_knowledge_type INTEGER NOT NULL,"
                              "session_key TEXT,"
                              "turn_number INTEGER,"
                              "created_ts_ms INTEGER NOT NULL,"
                              "resolved_ts_ms INTEGER)",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    return db;
}

static int64_t fixed_now_ms(void) {
    return 1779600000000LL;
}

static void test_drift_fires_for_recent_high_sigma_concern(void) {
    sqlite3 *db = make_db_with_schema();
    char sql[256];
    snprintf(sql, sizeof(sql),
             "INSERT INTO agent_self_concerns(observation_id,dimension,magnitude_sigma,"
             "window_n_turns,created_ts_ms) VALUES(1,'response_length',2.5,100,%lld)",
             (long long)(fixed_now_ms() - 3600000));
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);

    char buf[HU_ACTION_DIRECTIVE_MAX_LEN];
    size_t n = hu_action_directive_drift(db, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "response_length") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "drift") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Lean toward") != NULL);
    sqlite3_close(db);
}

static void test_drift_skips_below_threshold(void) {
    sqlite3 *db = make_db_with_schema();
    char sql[256];
    snprintf(sql, sizeof(sql),
             "INSERT INTO agent_self_concerns(observation_id,dimension,magnitude_sigma,"
             "window_n_turns,created_ts_ms) VALUES(1,'response_length',1.5,100,%lld)",
             (long long)(fixed_now_ms() - 3600000));
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);

    char buf[HU_ACTION_DIRECTIVE_MAX_LEN];
    size_t n = hu_action_directive_drift(db, buf, sizeof(buf));
    HU_ASSERT_EQ(n, 0u);
    HU_ASSERT_EQ(buf[0], '\0');
    sqlite3_close(db);
}

static void test_drift_skips_empty_table(void) {
    sqlite3 *db = make_db_with_schema();
    char buf[HU_ACTION_DIRECTIVE_MAX_LEN];
    size_t n = hu_action_directive_drift(db, buf, sizeof(buf));
    HU_ASSERT_EQ(n, 0u);
    HU_ASSERT_EQ(buf[0], '\0');
    sqlite3_close(db);
}

static void test_clarify_fires_for_stale_expectation(void) {
    sqlite3 *db = make_db_with_schema();
    char sql[400];
    snprintf(sql, sizeof(sql),
             "INSERT INTO tom_user_expectations("
             "contact_id,topic,expected_knowledge_type,session_key,turn_number,"
             "created_ts_ms,resolved_ts_ms) "
             "VALUES('seth','sourdough starter',0,'old_session',3,%lld,NULL)",
             (long long)(fixed_now_ms() - 1800000));
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);

    char buf[HU_ACTION_DIRECTIVE_MAX_LEN];
    size_t n = hu_action_directive_clarify(db, "seth", 4, "new_session", 11, fixed_now_ms(), buf,
                                           sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "sourdough starter") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Consider asking") != NULL);
    sqlite3_close(db);
}

static void test_clarify_skips_same_session_recent(void) {
    sqlite3 *db = make_db_with_schema();
    char sql[400];
    snprintf(sql, sizeof(sql),
             "INSERT INTO tom_user_expectations("
             "contact_id,topic,expected_knowledge_type,session_key,turn_number,"
             "created_ts_ms,resolved_ts_ms) "
             "VALUES('seth','dinner plans',0,'current_session',1,%lld,NULL)",
             (long long)(fixed_now_ms() - 60000));
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);

    char buf[HU_ACTION_DIRECTIVE_MAX_LEN];
    size_t n = hu_action_directive_clarify(db, "seth", 4, "current_session", 15, fixed_now_ms(),
                                           buf, sizeof(buf));
    HU_ASSERT_EQ(n, 0u);
    sqlite3_close(db);
}

static void test_clarify_skips_resolved(void) {
    sqlite3 *db = make_db_with_schema();
    char sql[400];
    snprintf(sql, sizeof(sql),
             "INSERT INTO tom_user_expectations("
             "contact_id,topic,expected_knowledge_type,session_key,turn_number,"
             "created_ts_ms,resolved_ts_ms) "
             "VALUES('seth','vacation dates',0,'old_session',2,%lld,%lld)",
             (long long)(fixed_now_ms() - 1800000), (long long)(fixed_now_ms() - 60000));
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);

    char buf[HU_ACTION_DIRECTIVE_MAX_LEN];
    size_t n = hu_action_directive_clarify(db, "seth", 4, "new_session", 11, fixed_now_ms(), buf,
                                           sizeof(buf));
    HU_ASSERT_EQ(n, 0u);
    sqlite3_close(db);
}

static void test_directives_are_independent(void) {
    sqlite3 *db = make_db_with_schema();
    char sql[400];
    snprintf(sql, sizeof(sql),
             "INSERT INTO tom_user_expectations("
             "contact_id,topic,expected_knowledge_type,session_key,turn_number,"
             "created_ts_ms,resolved_ts_ms) "
             "VALUES('seth','meeting time',1,'old',1,%lld,NULL)",
             (long long)(fixed_now_ms() - 1800000));
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);

    char drift_buf[HU_ACTION_DIRECTIVE_MAX_LEN];
    size_t drift_n = hu_action_directive_drift(db, drift_buf, sizeof(drift_buf));
    HU_ASSERT_EQ(drift_n, 0u);

    char clarify_buf[HU_ACTION_DIRECTIVE_MAX_LEN];
    size_t clarify_n = hu_action_directive_clarify(db, "seth", 4, "new", 3, fixed_now_ms(),
                                                   clarify_buf, sizeof(clarify_buf));
    HU_ASSERT_TRUE(clarify_n > 0);
    sqlite3_close(db);
}

static void test_buffers_cleared_on_null_db(void) {
    char buf[HU_ACTION_DIRECTIVE_MAX_LEN];
    memset(buf, 'x', sizeof(buf));
    size_t n = hu_action_directive_drift(NULL, buf, sizeof(buf));
    HU_ASSERT_EQ(n, 0u);
    HU_ASSERT_EQ(buf[0], '\0');

    memset(buf, 'y', sizeof(buf));
    n = hu_action_directive_clarify(NULL, "seth", 4, "s", 1, fixed_now_ms(), buf, sizeof(buf));
    HU_ASSERT_EQ(n, 0u);
    HU_ASSERT_EQ(buf[0], '\0');
}

void run_action_directives_tests(void) {
    HU_TEST_SUITE("action_directives");
    HU_RUN_TEST(test_drift_fires_for_recent_high_sigma_concern);
    HU_RUN_TEST(test_drift_skips_below_threshold);
    HU_RUN_TEST(test_drift_skips_empty_table);
    HU_RUN_TEST(test_clarify_fires_for_stale_expectation);
    HU_RUN_TEST(test_clarify_skips_same_session_recent);
    HU_RUN_TEST(test_clarify_skips_resolved);
    HU_RUN_TEST(test_directives_are_independent);
    HU_RUN_TEST(test_buffers_cleared_on_null_db);
}

#else

void run_action_directives_tests(void) {
    (void)0;
}

#endif
