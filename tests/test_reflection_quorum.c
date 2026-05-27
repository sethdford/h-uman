/* tests/test_reflection_quorum.c — Phase 2 quorum predicate (T11).
 *
 * Pins the contract for hu_reflection_pattern_has_quorum:
 *
 *   AC-T11.1: false on single-observation patterns (obs_count < 3)
 *   AC-T11.2: false when observed ≥3 times BUT confidence ≤ 0.7
 *   AC-T11.3: true when observed ≥3 times AND confidence > 0.7
 *   AC-T11.4: false on retired patterns even if quorum-met
 *   AC-T11.5: false on unknown id (defensive default)
 *
 * Phase 1 semantics: predicate is TELEMETRY ONLY. Mutating
 * personal_model on the basis of it is reserved for Phase 2.
 * scripts/check-reflection-quorum-not-wired.sh is the CI gate that
 * prevents that wiring from leaking in early. */

#include "human/reflection.h"
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>
#include <string.h>

/* Insert a pattern row with observation_count + confidence we control.
 * Bypasses the UPSERT to set the count directly — easier than running
 * three upserts. */
static void insert_quorum_pattern(sqlite3 *db, const char *id, int observation_count,
                                  double confidence, int retired) {
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO reflection_patterns "
        "(id, type, subject, observation, confidence, evidence_json, channels_json, "
        " first_seen_run_id, last_seen_run_id, observation_count, "
        " created_at_ms, last_observed_at_ms, expires_at_ms, "
        " surfaced_to_user, retired, retired_at_ms) "
        "VALUES (?, 'preference', 'Seth', 'X', ?, '[]', '[\"imessage\"]', "
        " 'r1', 'r1', ?, 1000, 2000, 100000000, 0, ?, NULL)";
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, NULL), SQLITE_OK);
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_double(st, 2, confidence);
    sqlite3_bind_int(st, 3, observation_count);
    sqlite3_bind_int(st, 4, retired);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
    sqlite3_finalize(st);
}

/* AC-T11.1: single observation never reaches quorum. */
static void test_quorum_false_for_single_observation(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_reflection_storage_migrate(db), HU_OK);
    HU_ASSERT_EQ(hu_reflection_storage_insert_run(db, "r1", "p", 1000, 5), HU_OK);
    insert_quorum_pattern(db, "p_single", 1, 0.9, 0);

    HU_ASSERT_TRUE(!hu_reflection_pattern_has_quorum(db, "p_single"));
    sqlite3_close(db);
}

/* AC-T11.2: enough observations but confidence too low. */
static void test_quorum_false_when_confidence_below_floor(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_reflection_storage_migrate(db), HU_OK);
    HU_ASSERT_EQ(hu_reflection_storage_insert_run(db, "r1", "p", 1000, 5), HU_OK);
    insert_quorum_pattern(db, "p_lowconf", 5, 0.65, 0); /* obs ok, conf too low */

    HU_ASSERT_TRUE(!hu_reflection_pattern_has_quorum(db, "p_lowconf"));
    /* Boundary: exactly 0.7 is NOT > 0.7 (strict) */
    insert_quorum_pattern(db, "p_exactly_70", 5, 0.7, 0);
    HU_ASSERT_TRUE(!hu_reflection_pattern_has_quorum(db, "p_exactly_70"));
    sqlite3_close(db);
}

/* AC-T11.3: quorum reached. */
static void test_quorum_true_when_three_observations_above_floor(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_reflection_storage_migrate(db), HU_OK);
    HU_ASSERT_EQ(hu_reflection_storage_insert_run(db, "r1", "p", 1000, 5), HU_OK);
    insert_quorum_pattern(db, "p_quorum", 3, 0.8, 0);

    HU_ASSERT_TRUE(hu_reflection_pattern_has_quorum(db, "p_quorum"));
    /* Boundary: 0.71 should be enough (> 0.7 strict) */
    insert_quorum_pattern(db, "p_just_over", 4, 0.71, 0);
    HU_ASSERT_TRUE(hu_reflection_pattern_has_quorum(db, "p_just_over"));
    sqlite3_close(db);
}

/* AC-T11.4: retired patterns NEVER reach quorum, even if they would have. */
static void test_quorum_false_for_retired_pattern(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_reflection_storage_migrate(db), HU_OK);
    HU_ASSERT_EQ(hu_reflection_storage_insert_run(db, "r1", "p", 1000, 5), HU_OK);
    insert_quorum_pattern(db, "p_retired", 10, 0.95, 1); /* would-be quorum, but retired */

    HU_ASSERT_TRUE(!hu_reflection_pattern_has_quorum(db, "p_retired"));
    sqlite3_close(db);
}

/* AC-T11.5: unknown id returns false (defensive default). */
static void test_quorum_false_for_unknown_id(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_reflection_storage_migrate(db), HU_OK);
    HU_ASSERT_TRUE(!hu_reflection_pattern_has_quorum(db, "no_such_pattern"));
    HU_ASSERT_TRUE(!hu_reflection_pattern_has_quorum(db, ""));
    HU_ASSERT_TRUE(!hu_reflection_pattern_has_quorum(NULL, "x"));
    HU_ASSERT_TRUE(!hu_reflection_pattern_has_quorum(db, NULL));
    sqlite3_close(db);
}

void run_reflection_quorum_tests(void) {
    HU_TEST_SUITE("reflection_quorum");
    HU_RUN_TEST(test_quorum_false_for_single_observation);
    HU_RUN_TEST(test_quorum_false_when_confidence_below_floor);
    HU_RUN_TEST(test_quorum_true_when_three_observations_above_floor);
    HU_RUN_TEST(test_quorum_false_for_retired_pattern);
    HU_RUN_TEST(test_quorum_false_for_unknown_id);
}

#else /* !HU_ENABLE_SQLITE — stub so the runner symbol resolves */

void run_reflection_quorum_tests(void) {
    /* No-op stub: SQLite disabled at build time. */
}

#endif /* HU_ENABLE_SQLITE */
