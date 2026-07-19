/* tests/test_reflection_consumer.c — T6 consumer queries.
 *
 * Pins the four consumer contracts from
 * docs/plans/2026-05-26-reflection-loop/tasks.md Task 6:
 *
 *   AC-T6.1 query_for_system_prompt excludes retired + surfaced + low-conf
 *   AC-T6.2 query_for_system_prompt channel filter matches single AND cross-channel
 *   AC-T6.3 query_for_system_prompt caps at max_patterns
 *   AC-T6.4 query_unsurfaced returns only unsurfaced ≥ min_confidence
 *   AC-T6.5 mark_surfaced is idempotent
 *   AC-T6.6 retire is idempotent and sets retired_at_ms
 *
 * Test fixture pattern: in-memory SQLite, hand-built pattern rows via
 * direct INSERT (the storage layer's UPSERT also works but bypasses
 * type-string conversion, so we build rows that match the schema
 * exactly). */

#include "human/reflection.h"
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

/* Insert a pattern row directly via SQL, bypassing the upsert path —
 * lets us test queries against handcrafted state including retired
 * and surfaced patterns that the upsert path can't produce. */
static void insert_test_pattern(sqlite3 *db, const char *id, const char *type, const char *subject,
                                const char *observation, double confidence,
                                const char *channels_json, int surfaced, int retired,
                                uint64_t last_observed_ms) {
    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO reflection_patterns "
                      "(id, type, subject, observation, confidence, evidence_json, channels_json, "
                      " first_seen_run_id, last_seen_run_id, observation_count, "
                      " created_at_ms, last_observed_at_ms, expires_at_ms, "
                      " surfaced_to_user, retired, retired_at_ms) "
                      "VALUES (?, ?, ?, ?, ?, '[\"t1\"]', ?, 'r1', 'r1', 1, ?, ?, ?, ?, ?, NULL)";
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, NULL), SQLITE_OK);
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, type, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, subject, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, observation, -1, SQLITE_STATIC);
    sqlite3_bind_double(st, 5, confidence);
    sqlite3_bind_text(st, 6, channels_json, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 7, (sqlite3_int64)last_observed_ms);
    sqlite3_bind_int64(st, 8, (sqlite3_int64)last_observed_ms);
    /* expires_at_ms = last_observed + 30d so the patterns are always
     * "fresh" — query filters on last_observed_ms, not expires. */
    sqlite3_bind_int64(st, 9, (sqlite3_int64)(last_observed_ms + 30L * 86400000L));
    sqlite3_bind_int(st, 10, surfaced);
    sqlite3_bind_int(st, 11, retired);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
    sqlite3_finalize(st);
}

/* Convenience: build a recent-ish timestamp so the 7d / 30d windows
 * in the queries see the patterns. Tests run in CI where wall clock
 * varies, so we anchor everything at "now - small_delta_ms" — that
 * passes both the 7d (query_for_system_prompt) and 30d
 * (query_unsurfaced) recency filters. */
static uint64_t now_ms_minus(uint64_t delta_ms) {
    sqlite3 *probe = NULL;
    sqlite3_open(":memory:", &probe);
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(probe, "SELECT strftime('%s','now') * 1000", -1, &st, NULL);
    sqlite3_step(st);
    uint64_t now_ms = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(probe);
    return now_ms > delta_ms ? now_ms - delta_ms : 0;
}

/* AC-T6.1: query excludes retired + surfaced + low-conf. */
static void test_consumer_query_excludes_retired_and_surfaced_and_low_conf(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_reflection_storage_migrate(db), HU_OK);
    HU_ASSERT_EQ(hu_reflection_storage_insert_run(db, "r1", "p", 1000, 10), HU_OK);

    uint64_t recent = now_ms_minus(60 * 60 * 1000); /* 1 hour ago */
    /* Should appear (only valid one) */
    insert_test_pattern(db, "p_keep", "preference", "Seth", "kept", 0.9, "[\"imessage\"]", 0, 0,
                        recent);
    /* Should NOT appear: retired */
    insert_test_pattern(db, "p_retired", "preference", "Seth", "retired", 0.9, "[\"imessage\"]", 0,
                        1, recent);
    /* Should NOT appear: already surfaced */
    insert_test_pattern(db, "p_surfaced", "preference", "Seth", "surfaced", 0.9, "[\"imessage\"]",
                        1, 0, recent);
    /* Should NOT appear: confidence too low */
    insert_test_pattern(db, "p_lowconf", "preference", "Seth", "lowconf", 0.5, "[\"imessage\"]", 0,
                        0, recent);

    hu_reflection_pattern_t *out = NULL;
    int n = 0;
    HU_ASSERT_EQ(hu_reflection_query_for_system_prompt(db, "imessage", 10, &out, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_STR_EQ(out[0].id, "p_keep");
    free(out);
    sqlite3_close(db);
}

/* AC-T6.2: channel filter matches single-channel AND cross-channel. */
static void test_consumer_query_channel_filter_includes_cross_channel(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_reflection_storage_migrate(db), HU_OK);
    HU_ASSERT_EQ(hu_reflection_storage_insert_run(db, "r1", "p", 1000, 10), HU_OK);

    uint64_t recent = now_ms_minus(60 * 60 * 1000);
    insert_test_pattern(db, "p_imsg", "preference", "Seth", "imessage-only", 0.9, "[\"imessage\"]",
                        0, 0, recent);
    insert_test_pattern(db, "p_telegram", "preference", "Seth", "telegram-only", 0.9,
                        "[\"telegram\"]", 0, 0, recent);
    insert_test_pattern(db, "p_cross", "behavioral_shift", "Seth", "cross-channel", 0.9,
                        "[\"imessage\",\"telegram\"]", 0, 0, recent);

    /* Query for telegram: expect p_telegram + p_cross (cross-channel
     * matches every channel filter), NOT p_imsg. */
    hu_reflection_pattern_t *out = NULL;
    int n = 0;
    HU_ASSERT_EQ(hu_reflection_query_for_system_prompt(db, "telegram", 10, &out, &n), HU_OK);
    HU_ASSERT_EQ(n, 2);

    /* Both expected IDs appear; ordering is by confidence-decay so we
     * don't pin the order here — just check presence. */
    int saw_telegram = 0, saw_cross = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(out[i].id, "p_telegram") == 0)
            saw_telegram = 1;
        if (strcmp(out[i].id, "p_cross") == 0)
            saw_cross = 1;
    }
    HU_ASSERT_TRUE(saw_telegram);
    HU_ASSERT_TRUE(saw_cross);
    free(out);
    sqlite3_close(db);
}

/* AC-T6.3: query caps at max_patterns. */
static void test_consumer_query_caps_at_max(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_reflection_storage_migrate(db), HU_OK);
    HU_ASSERT_EQ(hu_reflection_storage_insert_run(db, "r1", "p", 1000, 10), HU_OK);

    uint64_t recent = now_ms_minus(60 * 60 * 1000);
    for (int i = 0; i < 10; i++) {
        char id[32];
        snprintf(id, sizeof id, "p_%d", i);
        insert_test_pattern(db, id, "preference", "Seth", id, 0.9, "[\"imessage\"]", 0, 0, recent);
    }

    hu_reflection_pattern_t *out = NULL;
    int n = 0;
    HU_ASSERT_EQ(hu_reflection_query_for_system_prompt(db, "imessage", 3, &out, &n), HU_OK);
    HU_ASSERT_EQ(n, 3);
    free(out);
    sqlite3_close(db);
}

/* AC-T6.4: query_unsurfaced respects min_confidence + skips surfaced. */
static void test_consumer_query_unsurfaced_filters_by_min_confidence(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_reflection_storage_migrate(db), HU_OK);
    HU_ASSERT_EQ(hu_reflection_storage_insert_run(db, "r1", "p", 1000, 10), HU_OK);

    uint64_t recent = now_ms_minus(60 * 60 * 1000);
    insert_test_pattern(db, "p_hi", "preference", "Seth", "high-conf", 0.9, "[\"imessage\"]", 0, 0,
                        recent);
    insert_test_pattern(db, "p_mid", "preference", "Seth", "mid-conf", 0.7, "[\"imessage\"]", 0, 0,
                        recent);
    insert_test_pattern(db, "p_lo", "preference", "Seth", "low-conf", 0.55, "[\"imessage\"]", 0, 0,
                        recent);
    insert_test_pattern(db, "p_surf", "preference", "Seth", "already-surfaced", 0.9,
                        "[\"imessage\"]", 1, 0, recent);

    hu_reflection_pattern_t *out = NULL;
    int n = 0;
    HU_ASSERT_EQ(hu_reflection_query_unsurfaced(db, 0.6, &out, &n), HU_OK);
    HU_ASSERT_EQ(n, 2); /* p_hi + p_mid; p_lo below threshold; p_surf surfaced */

    int saw_hi = 0, saw_mid = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(out[i].id, "p_hi") == 0)
            saw_hi = 1;
        if (strcmp(out[i].id, "p_mid") == 0)
            saw_mid = 1;
    }
    HU_ASSERT_TRUE(saw_hi);
    HU_ASSERT_TRUE(saw_mid);
    free(out);
    sqlite3_close(db);
}

/* Regression (2026-07-19): readers run before the reflection loop's
 * writer has ever migrated the schema (init_proposer queries
 * unconditionally on SQLite-backed agents; the daemon-tick migrate is
 * gated behind the reflection loop being enabled). The consumer
 * queries must ensure the schema themselves instead of erroring with
 * "no such table: reflection_patterns" every proposer tick. */
static int reflection_patterns_table_exists(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "SELECT 1 FROM sqlite_master WHERE type='table' "
                                    "AND name='reflection_patterns'",
                                    -1, &st, NULL),
                 SQLITE_OK);
    int exists = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return exists;
}

static void test_consumer_query_unsurfaced_on_fresh_db_ensures_schema(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    /* Deliberately NO hu_reflection_storage_migrate — writer never ran. */
    HU_ASSERT_EQ(reflection_patterns_table_exists(db), 0);

    hu_reflection_pattern_t *out = NULL;
    int n = -1;
    HU_ASSERT_EQ(hu_reflection_query_unsurfaced(db, 0.6, &out, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);
    HU_ASSERT_EQ(out, NULL);
    HU_ASSERT_EQ(reflection_patterns_table_exists(db), 1);
    sqlite3_close(db);
}

static void test_consumer_query_for_system_prompt_on_fresh_db_ensures_schema(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(reflection_patterns_table_exists(db), 0);

    hu_reflection_pattern_t *out = NULL;
    int n = -1;
    HU_ASSERT_EQ(hu_reflection_query_for_system_prompt(db, "imessage", 10, &out, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);
    HU_ASSERT_EQ(out, NULL);
    HU_ASSERT_EQ(reflection_patterns_table_exists(db), 1);
    sqlite3_close(db);
}

/* AC-T6.5: mark_surfaced is idempotent. */
static void test_consumer_mark_surfaced_is_idempotent(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_reflection_storage_migrate(db), HU_OK);
    HU_ASSERT_EQ(hu_reflection_storage_insert_run(db, "r1", "p", 1000, 10), HU_OK);

    uint64_t recent = now_ms_minus(60 * 60 * 1000);
    insert_test_pattern(db, "p_x", "preference", "Seth", "X", 0.9, "[\"imessage\"]", 0, 0, recent);

    hu_reflection_mark_surfaced(db, "p_x");
    hu_reflection_mark_surfaced(db, "p_x"); /* second call — must not error */

    /* After marking, the system-prompt query should not return it. */
    hu_reflection_pattern_t *out = NULL;
    int n = 0;
    HU_ASSERT_EQ(hu_reflection_query_for_system_prompt(db, "imessage", 10, &out, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);
    HU_ASSERT_EQ(out, NULL);

    /* Calling mark_surfaced for an unknown id must also no-op. */
    hu_reflection_mark_surfaced(db, "no_such_id");
    sqlite3_close(db);
}

/* AC-T6.6: retire is idempotent + sets retired_at_ms. */
static void test_consumer_retire_is_idempotent_and_sets_timestamp(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_reflection_storage_migrate(db), HU_OK);
    HU_ASSERT_EQ(hu_reflection_storage_insert_run(db, "r1", "p", 1000, 10), HU_OK);

    uint64_t recent = now_ms_minus(60 * 60 * 1000);
    insert_test_pattern(db, "p_bad", "preference", "Seth", "wrong", 0.9, "[\"imessage\"]", 0, 0,
                        recent);

    hu_reflection_retire(db, "p_bad");
    hu_reflection_retire(db, "p_bad"); /* idempotent — second call ok */

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT retired, retired_at_ms FROM reflection_patterns WHERE id = ?",
                       -1, &st, NULL);
    sqlite3_bind_text(st, 1, "p_bad", -1, SQLITE_STATIC);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(st, 0), 1);
    HU_ASSERT_TRUE(sqlite3_column_int64(st, 1) > 0);
    sqlite3_finalize(st);

    /* Retired patterns must not appear in system-prompt slice. */
    hu_reflection_pattern_t *out = NULL;
    int n = 0;
    HU_ASSERT_EQ(hu_reflection_query_for_system_prompt(db, "imessage", 10, &out, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);
    HU_ASSERT_EQ(out, NULL);

    /* Retire on unknown id — no-op. */
    hu_reflection_retire(db, "no_such_id");
    sqlite3_close(db);
}

void run_reflection_consumer_tests(void) {
    HU_TEST_SUITE("reflection_consumer");
    HU_RUN_TEST(test_consumer_query_excludes_retired_and_surfaced_and_low_conf);
    HU_RUN_TEST(test_consumer_query_channel_filter_includes_cross_channel);
    HU_RUN_TEST(test_consumer_query_caps_at_max);
    HU_RUN_TEST(test_consumer_query_unsurfaced_filters_by_min_confidence);
    HU_RUN_TEST(test_consumer_query_unsurfaced_on_fresh_db_ensures_schema);
    HU_RUN_TEST(test_consumer_query_for_system_prompt_on_fresh_db_ensures_schema);
    HU_RUN_TEST(test_consumer_mark_surfaced_is_idempotent);
    HU_RUN_TEST(test_consumer_retire_is_idempotent_and_sets_timestamp);
}

#else /* !HU_ENABLE_SQLITE — stub so the runner symbol resolves */

void run_reflection_consumer_tests(void) {
    /* No-op stub: SQLite disabled at build time. */
}

#endif /* HU_ENABLE_SQLITE */
