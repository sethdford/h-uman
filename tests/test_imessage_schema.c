/* tests/test_imessage_schema.c
 *
 * Phase 6 of docs/plans/2026-05-18-imessage-sota.md: schema probe + drift
 * canary regression tests.
 *
 * Build three in-memory chat.db's representing three macOS eras (Catalina,
 * Ventura, Sonoma+), probe each, assert the caps flags match. Plus a drift
 * canary (Apple adds a column we don't know) and a cache-hit verification.
 *
 * Internally guarded by HU_HAS_IMESSAGE + HU_ENABLE_SQLITE per the
 * test-source-gate-symmetry.md stub-runner pattern. */

#if HU_HAS_IMESSAGE && defined(HU_ENABLE_SQLITE)

#include "human/channels/imessage_schema.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------- fixture schemas ----------------------------------------------- */

/* Catalina (macOS 10.15): no date_retracted, no thread_originator_guid,
 * no associated_message_emoji. */
static const char *catalina_schema_sql = "CREATE TABLE message ("
                                         "  ROWID INTEGER PRIMARY KEY AUTOINCREMENT,"
                                         "  guid TEXT UNIQUE,"
                                         "  text TEXT,"
                                         "  handle_id INTEGER,"
                                         "  date INTEGER DEFAULT 0,"
                                         "  is_from_me INTEGER DEFAULT 0,"
                                         "  associated_message_type INTEGER DEFAULT 0,"
                                         "  associated_message_guid TEXT,"
                                         "  attributedBody BLOB,"
                                         "  date_delivered INTEGER DEFAULT 0,"
                                         "  date_read INTEGER DEFAULT 0"
                                         ");";

/* Ventura (macOS 13): adds date_retracted, thread_originator_guid, etc.
 * NOT associated_message_emoji. */
static const char *ventura_schema_sql = "CREATE TABLE message ("
                                        "  ROWID INTEGER PRIMARY KEY AUTOINCREMENT,"
                                        "  guid TEXT UNIQUE,"
                                        "  text TEXT,"
                                        "  handle_id INTEGER,"
                                        "  date INTEGER DEFAULT 0,"
                                        "  is_from_me INTEGER DEFAULT 0,"
                                        "  associated_message_type INTEGER DEFAULT 0,"
                                        "  associated_message_guid TEXT,"
                                        "  attributedBody BLOB,"
                                        "  date_delivered INTEGER DEFAULT 0,"
                                        "  date_read INTEGER DEFAULT 0,"
                                        "  date_edited INTEGER DEFAULT 0,"
                                        "  date_retracted INTEGER DEFAULT 0,"
                                        "  thread_originator_guid TEXT,"
                                        "  thread_originator_part TEXT,"
                                        "  message_summary_info BLOB,"
                                        "  balloon_bundle_id TEXT,"
                                        "  expressive_send_style_id TEXT,"
                                        "  payload_data BLOB,"
                                        "  group_action_type INTEGER DEFAULT 0,"
                                        "  group_title TEXT"
                                        ");";

/* Sonoma+ (macOS 14, iOS 17): adds associated_message_emoji. */
static const char *sonoma_schema_sql = "CREATE TABLE message ("
                                       "  ROWID INTEGER PRIMARY KEY AUTOINCREMENT,"
                                       "  guid TEXT UNIQUE,"
                                       "  text TEXT,"
                                       "  handle_id INTEGER,"
                                       "  date INTEGER DEFAULT 0,"
                                       "  is_from_me INTEGER DEFAULT 0,"
                                       "  associated_message_type INTEGER DEFAULT 0,"
                                       "  associated_message_guid TEXT,"
                                       "  associated_message_emoji TEXT,"
                                       "  attributedBody BLOB,"
                                       "  date_delivered INTEGER DEFAULT 0,"
                                       "  date_read INTEGER DEFAULT 0,"
                                       "  date_edited INTEGER DEFAULT 0,"
                                       "  date_retracted INTEGER DEFAULT 0,"
                                       "  thread_originator_guid TEXT,"
                                       "  thread_originator_part TEXT,"
                                       "  message_summary_info BLOB,"
                                       "  balloon_bundle_id TEXT,"
                                       "  expressive_send_style_id TEXT,"
                                       "  payload_data BLOB,"
                                       "  group_action_type INTEGER DEFAULT 0,"
                                       "  group_title TEXT"
                                       ");";

/* Drift canary: Ventura schema + a fake future column. */
static const char *drift_schema_sql = "CREATE TABLE message ("
                                      "  ROWID INTEGER PRIMARY KEY AUTOINCREMENT,"
                                      "  guid TEXT UNIQUE,"
                                      "  text TEXT,"
                                      "  handle_id INTEGER,"
                                      "  date INTEGER DEFAULT 0,"
                                      "  is_from_me INTEGER DEFAULT 0,"
                                      "  date_retracted INTEGER DEFAULT 0,"
                                      "  thread_originator_guid TEXT,"
                                      "  apple_new_field_3000 TEXT,"
                                      "  some_other_future_col INTEGER DEFAULT 0"
                                      ");";

/* ---------- helpers ----------------------------------------------------- */

static int g_tmp_counter = 0;

static void make_tmp_db_path(char *out, size_t out_size) {
    snprintf(out, out_size, "/tmp/hu_test_imessage_schema_%d_%d.db", (int)getpid(),
             ++g_tmp_counter);
    (void)unlink(out);
}

static int build_db(const char *path, const char *schema_sql) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK || !db)
        return -1;
    char *err = NULL;
    rc = sqlite3_exec(db, schema_sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        sqlite3_free(err);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_close(db);
    return 0;
}

/* ---------- tests ------------------------------------------------------- */

static void test_schema_probe_catalina_lacks_modern_columns(void) {
    char path[256];
    make_tmp_db_path(path, sizeof(path));
    HU_ASSERT_EQ(build_db(path, catalina_schema_sql), 0);
    hu_imessage_schema_reset_cache();

    hu_imessage_schema_caps_t caps;
    HU_ASSERT_EQ(hu_imessage_schema_probe(path, &caps), HU_OK);

    HU_ASSERT_TRUE(caps.probed);
    HU_ASSERT_FALSE(caps.has_date_retracted);
    HU_ASSERT_FALSE(caps.has_thread_originator_guid);
    HU_ASSERT_FALSE(caps.has_associated_message_emoji);
    HU_ASSERT_FALSE(caps.has_message_summary_info);
    HU_ASSERT_EQ((int)caps.unknown_column_count, 0);
    HU_ASSERT_TRUE(caps.schema_fingerprint[0] != '\0');

    (void)unlink(path);
}

static void test_schema_probe_ventura_has_thread_and_retract_not_emoji(void) {
    char path[256];
    make_tmp_db_path(path, sizeof(path));
    HU_ASSERT_EQ(build_db(path, ventura_schema_sql), 0);
    hu_imessage_schema_reset_cache();

    hu_imessage_schema_caps_t caps;
    HU_ASSERT_EQ(hu_imessage_schema_probe(path, &caps), HU_OK);

    HU_ASSERT_TRUE(caps.probed);
    HU_ASSERT_TRUE(caps.has_date_retracted);
    HU_ASSERT_TRUE(caps.has_thread_originator_guid);
    HU_ASSERT_TRUE(caps.has_message_summary_info);
    HU_ASSERT_TRUE(caps.has_balloon_bundle_id);
    HU_ASSERT_TRUE(caps.has_expressive_send_style_id);
    HU_ASSERT_TRUE(caps.has_payload_data);
    HU_ASSERT_TRUE(caps.has_group_action_type);
    HU_ASSERT_TRUE(caps.has_group_title);
    HU_ASSERT_FALSE(caps.has_associated_message_emoji);
    HU_ASSERT_EQ((int)caps.unknown_column_count, 0);

    (void)unlink(path);
}

static void test_schema_probe_sonoma_has_associated_message_emoji(void) {
    char path[256];
    make_tmp_db_path(path, sizeof(path));
    HU_ASSERT_EQ(build_db(path, sonoma_schema_sql), 0);
    hu_imessage_schema_reset_cache();

    hu_imessage_schema_caps_t caps;
    HU_ASSERT_EQ(hu_imessage_schema_probe(path, &caps), HU_OK);

    HU_ASSERT_TRUE(caps.probed);
    HU_ASSERT_TRUE(caps.has_associated_message_emoji);
    HU_ASSERT_TRUE(caps.has_thread_originator_guid);
    HU_ASSERT_TRUE(caps.has_date_retracted);
    HU_ASSERT_EQ((int)caps.unknown_column_count, 0);

    (void)unlink(path);
}

static void test_schema_probe_drift_canary_captures_unknown_columns(void) {
    char path[256];
    make_tmp_db_path(path, sizeof(path));
    HU_ASSERT_EQ(build_db(path, drift_schema_sql), 0);
    hu_imessage_schema_reset_cache();

    hu_imessage_schema_caps_t caps;
    HU_ASSERT_EQ(hu_imessage_schema_probe(path, &caps), HU_OK);

    HU_ASSERT_TRUE(caps.probed);
    HU_ASSERT_EQ((int)caps.unknown_column_count, 2);
    HU_ASSERT_EQ((int)caps.unknown_column_total, 2);

    bool found_apple_new = false;
    bool found_other_future = false;
    for (size_t i = 0; i < caps.unknown_column_count; i++) {
        if (strcmp(caps.unknown_columns[i], "apple_new_field_3000") == 0)
            found_apple_new = true;
        if (strcmp(caps.unknown_columns[i], "some_other_future_col") == 0)
            found_other_future = true;
    }
    HU_ASSERT_TRUE(found_apple_new);
    HU_ASSERT_TRUE(found_other_future);

    (void)unlink(path);
}

/* macOS 2026: the 8 columns Apple added to `message` (logged as drift at every
 * daemon start on 2026-10-02) are now known, so a current chat.db is clean. */
static const char *tahoe_schema_sql = "CREATE TABLE message ("
                                      "  ROWID INTEGER PRIMARY KEY AUTOINCREMENT,"
                                      "  guid TEXT UNIQUE,"
                                      "  text TEXT,"
                                      "  handle_id INTEGER,"
                                      "  date INTEGER DEFAULT 0,"
                                      "  is_from_me INTEGER DEFAULT 0,"
                                      "  associated_message_emoji TEXT,"
                                      "  date_recovered INTEGER DEFAULT 0,"
                                      "  is_sos INTEGER DEFAULT 0,"
                                      "  is_critical INTEGER DEFAULT 0,"
                                      "  bia_reference_id TEXT,"
                                      "  is_kt_verified INTEGER DEFAULT 0,"
                                      "  fallback_hash TEXT,"
                                      "  is_pending_satellite_send INTEGER DEFAULT 0,"
                                      "  needs_relay INTEGER DEFAULT 0,"
                                      "  schedule_type INTEGER DEFAULT 0,"
                                      "  schedule_state INTEGER DEFAULT 0,"
                                      "  sent_or_received_off_grid INTEGER DEFAULT 0,"
                                      "  is_time_sensitive INTEGER DEFAULT 0,"
                                      "  ck_chat_id TEXT,"
                                      "  index_state INTEGER DEFAULT 0"
                                      ");";

static void test_schema_probe_tahoe_columns_are_not_drift(void) {
    char path[256];
    make_tmp_db_path(path, sizeof(path));
    HU_ASSERT_EQ(build_db(path, tahoe_schema_sql), 0);
    hu_imessage_schema_reset_cache();

    hu_imessage_schema_caps_t caps;
    HU_ASSERT_EQ(hu_imessage_schema_probe(path, &caps), HU_OK);
    HU_ASSERT_TRUE(caps.probed);
    HU_ASSERT_EQ((int)caps.unknown_column_count, 0);
    HU_ASSERT_EQ((int)caps.unknown_column_total, 0);
    (void)unlink(path);
}

/* More unknown columns than names kept: the total must still be the real count
 * (2026-10-02 the log said "8 unknown" while chat.db had 14). */
static const char *many_unknown_sql = "CREATE TABLE message ("
                                      "  ROWID INTEGER PRIMARY KEY AUTOINCREMENT,"
                                      "  guid TEXT UNIQUE,"
                                      "  text TEXT,"
                                      "  zz_future_1 INTEGER, zz_future_2 INTEGER,"
                                      "  zz_future_3 INTEGER, zz_future_4 INTEGER,"
                                      "  zz_future_5 INTEGER, zz_future_6 INTEGER,"
                                      "  zz_future_7 INTEGER, zz_future_8 INTEGER,"
                                      "  zz_future_9 INTEGER, zz_future_10 INTEGER"
                                      ");";

static void test_schema_probe_counts_every_unknown_column(void) {
    char path[256];
    make_tmp_db_path(path, sizeof(path));
    HU_ASSERT_EQ(build_db(path, many_unknown_sql), 0);
    hu_imessage_schema_reset_cache();

    hu_imessage_schema_caps_t caps;
    HU_ASSERT_EQ(hu_imessage_schema_probe(path, &caps), HU_OK);
    HU_ASSERT_EQ((int)caps.unknown_column_count, HU_IMESSAGE_SCHEMA_UNKNOWN_MAX);
    HU_ASSERT_EQ((int)caps.unknown_column_total, 10);
    (void)unlink(path);
}

static void test_schema_probe_caches_result_across_calls(void) {
    char path[256];
    make_tmp_db_path(path, sizeof(path));
    HU_ASSERT_EQ(build_db(path, ventura_schema_sql), 0);
    hu_imessage_schema_reset_cache();

    hu_imessage_schema_caps_t caps1, caps2;
    HU_ASSERT_EQ(hu_imessage_schema_probe(path, &caps1), HU_OK);

    /* Delete the file; cache hit must still succeed. */
    (void)unlink(path);

    HU_ASSERT_EQ(hu_imessage_schema_probe(path, &caps2), HU_OK);
    HU_ASSERT_STR_EQ(caps1.schema_fingerprint, caps2.schema_fingerprint);
    HU_ASSERT_EQ((int)caps1.has_associated_message_emoji, (int)caps2.has_associated_message_emoji);
    HU_ASSERT_EQ((int)caps1.has_thread_originator_guid, (int)caps2.has_thread_originator_guid);
}

static void test_schema_probe_rejects_null_out(void) {
    HU_ASSERT_EQ(hu_imessage_schema_probe("/tmp/anything.db", NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_schema_probe_returns_io_for_nonexistent_db(void) {
    hu_imessage_schema_reset_cache();
    hu_imessage_schema_caps_t caps;
    hu_error_t err = hu_imessage_schema_probe("/tmp/hu_test_schema_does_not_exist_xyz.db", &caps);
    HU_ASSERT_TRUE(err == HU_ERR_IO);
}

static void test_schema_probe_fingerprints_differ_across_eras(void) {
    char path_c[256], path_v[256], path_s[256];
    make_tmp_db_path(path_c, sizeof(path_c));
    make_tmp_db_path(path_v, sizeof(path_v));
    make_tmp_db_path(path_s, sizeof(path_s));
    HU_ASSERT_EQ(build_db(path_c, catalina_schema_sql), 0);
    HU_ASSERT_EQ(build_db(path_v, ventura_schema_sql), 0);
    HU_ASSERT_EQ(build_db(path_s, sonoma_schema_sql), 0);

    hu_imessage_schema_caps_t caps_c, caps_v, caps_s;
    hu_imessage_schema_reset_cache();
    HU_ASSERT_EQ(hu_imessage_schema_probe(path_c, &caps_c), HU_OK);
    hu_imessage_schema_reset_cache();
    HU_ASSERT_EQ(hu_imessage_schema_probe(path_v, &caps_v), HU_OK);
    hu_imessage_schema_reset_cache();
    HU_ASSERT_EQ(hu_imessage_schema_probe(path_s, &caps_s), HU_OK);

    HU_ASSERT_TRUE(strcmp(caps_c.schema_fingerprint, caps_v.schema_fingerprint) != 0);
    HU_ASSERT_TRUE(strcmp(caps_v.schema_fingerprint, caps_s.schema_fingerprint) != 0);
    HU_ASSERT_TRUE(strcmp(caps_c.schema_fingerprint, caps_s.schema_fingerprint) != 0);

    fprintf(stderr, "[schema-fingerprint] catalina=%s\n", caps_c.schema_fingerprint);
    fprintf(stderr, "[schema-fingerprint] ventura =%s\n", caps_v.schema_fingerprint);
    fprintf(stderr, "[schema-fingerprint] sonoma  =%s\n", caps_s.schema_fingerprint);

    (void)unlink(path_c);
    (void)unlink(path_v);
    (void)unlink(path_s);
}

static void test_schema_log_fingerprint_does_not_crash_on_null_or_unprobed(void) {
    hu_imessage_schema_log_fingerprint(NULL);

    hu_imessage_schema_caps_t unprobed;
    memset(&unprobed, 0, sizeof(unprobed));
    hu_imessage_schema_log_fingerprint(&unprobed);
}

void run_imessage_schema_tests(void) {
    HU_TEST_SUITE("imessage_schema");
    HU_RUN_TEST(test_schema_probe_catalina_lacks_modern_columns);
    HU_RUN_TEST(test_schema_probe_ventura_has_thread_and_retract_not_emoji);
    HU_RUN_TEST(test_schema_probe_sonoma_has_associated_message_emoji);
    HU_RUN_TEST(test_schema_probe_drift_canary_captures_unknown_columns);
    HU_RUN_TEST(test_schema_probe_tahoe_columns_are_not_drift);
    HU_RUN_TEST(test_schema_probe_counts_every_unknown_column);
    HU_RUN_TEST(test_schema_probe_caches_result_across_calls);
    HU_RUN_TEST(test_schema_probe_rejects_null_out);
    HU_RUN_TEST(test_schema_probe_returns_io_for_nonexistent_db);
    HU_RUN_TEST(test_schema_probe_fingerprints_differ_across_eras);
    HU_RUN_TEST(test_schema_log_fingerprint_does_not_crash_on_null_or_unprobed);
}

#else /* HU_HAS_IMESSAGE && HU_ENABLE_SQLITE */

void run_imessage_schema_tests(void) {
    (void)0;
}

#endif /* HU_HAS_IMESSAGE && HU_ENABLE_SQLITE */
