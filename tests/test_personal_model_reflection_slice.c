/* tests/test_personal_model_reflection_slice.c — T7 of
 * docs/plans/2026-05-26-reflection-loop.
 *
 * Pins the four contracts of hu_personal_model_build_prompt_with_reflection:
 *
 *   AC-T7.1  with db=NULL or channel=NULL, behaves identically to
 *            _build_prompt_with_overlay (NO reflection slice appended)
 *   AC-T7.2  with patterns in the db, the slice appears as
 *            "Recent observations about Seth (from reflection):\n
 *             - <observation> (confidence X.XX)\n..."
 *   AC-T7.3  surfaced patterns get marked → next call returns the
 *            updated list (single-use semantics)
 *   AC-T7.4  the latest prose_summary appears as "Latest reflection
 *            summary: ..." when a completed run exists */

#include "human/memory/personal_model.h"
#include "human/reflection.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>

/* ── Fixture: build an in-memory db with N patterns + an ok run ── */

static void insert_test_pattern(sqlite3 *db, const char *id, const char *subject,
                                const char *observation, double confidence,
                                const char *channels_json, int surfaced) {
    sqlite3_stmt *st = NULL;
    /* Use NOW-ish timestamps because query_for_system_prompt filters by a
     * 7-day recency window AND orders by confidence weighted by an age
     * decay. Epoch timestamps (1000ms) get filtered out as "too old". */
    uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
    const char *sql =
        "INSERT INTO reflection_patterns (id, type, subject, observation, confidence, "
        "evidence_json, channels_json, first_seen_run_id, last_seen_run_id, "
        "observation_count, created_at_ms, last_observed_at_ms, expires_at_ms, "
        "surfaced_to_user, retired) "
        "VALUES (?, 'preference', ?, ?, ?, '[]', ?, 'run_seed', 'run_seed', 1, "
        "?, ?, ?, ?, 0)";
    sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, subject, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, observation, -1, SQLITE_STATIC);
    sqlite3_bind_double(st, 4, confidence);
    sqlite3_bind_text(st, 5, channels_json, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)now_ms); /* created_at_ms */
    sqlite3_bind_int64(st, 7, (sqlite3_int64)now_ms); /* last_observed_at_ms */
    sqlite3_bind_int64(st, 8, (sqlite3_int64)(now_ms + 30ULL * 86400000ULL)); /* expires */
    sqlite3_bind_int(st, 9, surfaced);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static void insert_completed_run(sqlite3 *db, const char *run_id, const char *prose) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
                       "INSERT INTO reflection_runs (run_id, provider, started_at_ms, "
                       "completed_at_ms, input_turns, status, prose_summary) "
                       "VALUES (?, 'mock', 1000, 2000, 5, 'ok', ?)",
                       -1, &st, NULL);
    sqlite3_bind_text(st, 1, run_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, prose, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static hu_personal_model_t empty_model(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.core.user_name, sizeof m.core.user_name, "%s", "Seth");
    return m;
}

/* AC-T7.1: db=NULL → no reflection slice. */
static void test_reflection_slice_skipped_when_db_null(void) {
    hu_personal_model_t m = empty_model();
    char buf[4096];
    size_t n = hu_personal_model_build_prompt_with_reflection(&m, NULL, /*db=*/NULL, "imessage", 5,
                                                              buf, sizeof buf);
    HU_ASSERT(n > 0);
    HU_ASSERT(strstr(buf, "Recent observations") == NULL);
    HU_ASSERT(strstr(buf, "Latest reflection") == NULL);
    /* Personal context still appears. */
    HU_ASSERT(strstr(buf, "Seth") != NULL);
}

/* AC-T7.1 b: channel=NULL → no slice (but db is set). */
static void test_reflection_slice_skipped_when_channel_null(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    hu_personal_model_t m = empty_model();
    char buf[4096];
    size_t n =
        hu_personal_model_build_prompt_with_reflection(&m, NULL, db, NULL, 5, buf, sizeof buf);
    HU_ASSERT(n > 0);
    HU_ASSERT(strstr(buf, "Recent observations") == NULL);
    sqlite3_close(db);
}

/* AC-T7.2: with patterns in db, the slice appears. */
static void test_reflection_slice_appended_with_patterns(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    insert_completed_run(db, "run_seed", "Seth prefers concise mornings");
    insert_test_pattern(db, "p1", "Seth", "prefers jazz between 3-5pm", 0.85, "[\"imessage\"]", 0);
    insert_test_pattern(db, "p2", "Seth", "shifts to one-word replies after 9pm", 0.78,
                        "[\"imessage\"]", 0);

    hu_personal_model_t m = empty_model();
    char buf[8192];
    size_t n = hu_personal_model_build_prompt_with_reflection(&m, NULL, db, "imessage", 5, buf,
                                                              sizeof buf);
    HU_ASSERT(n > 0);
    HU_ASSERT(strstr(buf, "Recent observations about Seth") != NULL);
    HU_ASSERT(strstr(buf, "prefers jazz between 3-5pm") != NULL);
    HU_ASSERT(strstr(buf, "shifts to one-word replies after 9pm") != NULL);
    HU_ASSERT(strstr(buf, "(confidence 0.85)") != NULL);
    HU_ASSERT(strstr(buf, "Latest reflection summary: Seth prefers concise mornings") != NULL);
    sqlite3_close(db);
}

/* AC-T7.3: single-use semantics — patterns get marked, second call empty. */
static void test_reflection_patterns_are_marked_surfaced_after_first_use(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    insert_completed_run(db, "run_seed", "Run prose");
    insert_test_pattern(db, "p1", "Seth", "first observation", 0.85, "[\"imessage\"]", 0);

    hu_personal_model_t m = empty_model();
    char buf[4096];

    /* First call: pattern appears + prose summary. */
    hu_personal_model_build_prompt_with_reflection(&m, NULL, db, "imessage", 5, buf, sizeof buf);
    HU_ASSERT(strstr(buf, "first observation") != NULL);

    /* Verify the surfaced flag flipped in the db. */
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT surfaced_to_user FROM reflection_patterns WHERE id='p1'", -1,
                       &st, NULL);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(st, 0), 1);
    sqlite3_finalize(st);

    /* Second call: pattern is now marked-surfaced, query_for_system_prompt
     * filters it out. Prose summary still appears because that's not
     * gated on surfaced. */
    memset(buf, 0, sizeof buf);
    hu_personal_model_build_prompt_with_reflection(&m, NULL, db, "imessage", 5, buf, sizeof buf);
    HU_ASSERT(strstr(buf, "first observation") == NULL);
    HU_ASSERT(strstr(buf, "Recent observations about Seth") == NULL);
    HU_ASSERT(strstr(buf, "Latest reflection: Run prose") != NULL);

    sqlite3_close(db);
}

/* AC-T7.4: prose summary appears when present even without patterns. */
static void test_reflection_prose_summary_appears_without_patterns(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    insert_completed_run(db, "run_seed", "Lone digest sentence");
    /* No patterns inserted. */

    hu_personal_model_t m = empty_model();
    char buf[4096];
    hu_personal_model_build_prompt_with_reflection(&m, NULL, db, "imessage", 5, buf, sizeof buf);
    HU_ASSERT(strstr(buf, "Lone digest sentence") != NULL);
    HU_ASSERT(strstr(buf, "Recent observations") == NULL);
    sqlite3_close(db);
}

void run_personal_model_reflection_slice_tests(void) {
    HU_TEST_SUITE("personal_model_reflection_slice");
    HU_RUN_TEST(test_reflection_slice_skipped_when_db_null);
    HU_RUN_TEST(test_reflection_slice_skipped_when_channel_null);
    HU_RUN_TEST(test_reflection_slice_appended_with_patterns);
    HU_RUN_TEST(test_reflection_patterns_are_marked_surfaced_after_first_use);
    HU_RUN_TEST(test_reflection_prose_summary_appears_without_patterns);
}

#else /* !HU_ENABLE_SQLITE */

void run_personal_model_reflection_slice_tests(void) {
    /* Stub when SQLite is disabled. */
}

#endif /* HU_ENABLE_SQLITE */
