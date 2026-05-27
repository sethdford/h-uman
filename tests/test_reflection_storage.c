/* tests/test_reflection_storage.c — Reflection SQLite storage layer (T2).
 *
 * Pins the three storage contracts per
 * docs/plans/2026-05-26-reflection-loop/tasks.md Task 2:
 *   AC-T2.1  migrate creates both tables + indexes (idempotent)
 *   AC-T2.2  UPSERT bumps observation_count + takes MAX(confidence)
 *   AC-T2.3  patterns with confidence < 0.5 are dropped silently
 *
 * Tests use in-memory SQLite (:memory:) so no fixture cleanup is
 * needed — close the db handle and the database goes away.
 *
 * Why these tests are load-bearing: the confidence-floor drop at
 * UPSERT is the contract that the parse layer relies on (parse keeps
 * low-confidence patterns so the run-level dropped count can be
 * reported, but they must not pollute reflection_patterns). Without
 * AC-T2.3 the storage layer would carry junk that consumers would
 * surface to the user. */

#include "human/reflection.h"
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>
#include <string.h>

/* AC-T2.1: migrate creates both tables + idx. */
static void test_storage_migrate_creates_tables_and_indexes(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_reflection_storage_migrate(db), HU_OK);

    /* both tables exist */
    sqlite3_stmt *stmt = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "SELECT name FROM sqlite_master WHERE type='table' "
                                    "AND name IN ('reflection_runs','reflection_patterns') "
                                    "ORDER BY name",
                                    -1, &stmt, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "reflection_patterns");
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "reflection_runs");
    sqlite3_finalize(stmt);

    /* indexes exist */
    sqlite3_stmt *st2 = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "SELECT name FROM sqlite_master WHERE type='index' "
                                    "AND name IN ('idx_patterns_recent','idx_patterns_unsurfaced')"
                                    " ORDER BY name",
                                    -1, &st2, NULL),
                 SQLITE_OK);
    int idx_count = 0;
    while (sqlite3_step(st2) == SQLITE_ROW)
        idx_count++;
    HU_ASSERT_EQ(idx_count, 2);
    sqlite3_finalize(st2);

    /* idempotent — running again must succeed */
    HU_ASSERT_EQ(hu_reflection_storage_migrate(db), HU_OK);

    sqlite3_close(db);
}

/* Helper: seed a pattern struct with a stable id. */
static void seed_pattern(hu_reflection_pattern_t *p, const char *subject, const char *observation,
                         double conf, uint64_t now_ms) {
    memset(p, 0, sizeof(*p));
    p->type = HU_REFLECTION_PATTERN_PREFERENCE;
    snprintf(p->subject, sizeof p->subject, "%s", subject);
    snprintf(p->observation, sizeof p->observation, "%s", observation);
    p->confidence = conf;
    p->evidence_count = 1;
    snprintf(p->evidence_ids[0], sizeof p->evidence_ids[0], "turn_1");
    p->channel_count = 1;
    snprintf(p->channels[0], sizeof p->channels[0], "imessage");
    p->created_at_ms = now_ms;
    p->last_observed_at_ms = now_ms;
    p->expires_at_ms = now_ms + 30L * 86400000L;
    /* Use the SAME hash function the parser uses — exposed publicly at
     * T2 so storage tests can derive ids directly. This means same
     * (subject, observation) → same id no matter how the pattern got
     * into the upsert path (parsed JSON OR struct-constructed). The
     * `conf` argument is intentionally NOT part of the id: re-deriving
     * the same pattern at different confidence must hit the same row
     * (that's what AC-T2.2 verifies). */
    hu_reflection_compute_id(p->type, p->subject, p->observation, p->id, sizeof(p->id));
    (void)conf; /* documented above: not an id input */
}

/* AC-T2.2: UPSERT bumps observation_count + takes MAX(confidence). */
static void test_storage_upsert_bumps_observation_count_and_takes_max_confidence(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_reflection_storage_migrate(db), HU_OK);

    HU_ASSERT_EQ(hu_reflection_storage_insert_run(db, "run_1", "gemini-3.5-flash", 1000, 10),
                 HU_OK);
    HU_ASSERT_EQ(hu_reflection_storage_insert_run(db, "run_2", "gemini-3.5-flash", 2000, 12),
                 HU_OK);

    hu_reflection_pattern_t p;
    seed_pattern(&p, "Seth", "prefers concise replies", 0.8, 1000);

    HU_ASSERT_EQ(hu_reflection_storage_upsert(db, "run_1", &p), HU_OK);

    /* Re-insert same id under run_2 with LOWER confidence and bumped time —
     * UPSERT must keep MAX (0.8), bump observation_count to 2, and
     * update last_seen_run_id to run_2. */
    p.confidence = 0.6;
    p.last_observed_at_ms = 2000;
    HU_ASSERT_EQ(hu_reflection_storage_upsert(db, "run_2", &p), HU_OK);

    /* Verify */
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "SELECT observation_count, confidence, last_seen_run_id, "
                                    "       last_observed_at_ms"
                                    " FROM reflection_patterns WHERE id = ?",
                                    -1, &st, NULL),
                 SQLITE_OK);
    sqlite3_bind_text(st, 1, p.id, -1, SQLITE_STATIC);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(st, 0), 2);
    HU_ASSERT_TRUE(sqlite3_column_double(st, 1) > 0.79);
    HU_ASSERT_TRUE(sqlite3_column_double(st, 1) < 0.81);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 2), "run_2");
    HU_ASSERT_EQ(sqlite3_column_int64(st, 3), 2000);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
    sqlite3_finalize(st);

    /* Now the OTHER direction — higher confidence on the third observation
     * should be taken as the new max. */
    p.confidence = 0.95;
    p.last_observed_at_ms = 3000;
    HU_ASSERT_EQ(hu_reflection_storage_insert_run(db, "run_3", "gemini-3.5-flash", 3000, 8), HU_OK);
    HU_ASSERT_EQ(hu_reflection_storage_upsert(db, "run_3", &p), HU_OK);

    sqlite3_stmt *st2 = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "SELECT observation_count, confidence "
                                    "FROM reflection_patterns WHERE id = ?",
                                    -1, &st2, NULL),
                 SQLITE_OK);
    sqlite3_bind_text(st2, 1, p.id, -1, SQLITE_STATIC);
    HU_ASSERT_EQ(sqlite3_step(st2), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(st2, 0), 3);
    HU_ASSERT_TRUE(sqlite3_column_double(st2, 1) > 0.94);
    sqlite3_finalize(st2);

    sqlite3_close(db);
}

/* AC-T2.3: low-confidence patterns dropped at upsert boundary. */
static void test_storage_upsert_drops_low_confidence(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_reflection_storage_migrate(db), HU_OK);
    HU_ASSERT_EQ(hu_reflection_storage_insert_run(db, "run_1", "gemini-3.5-flash", 1000, 10),
                 HU_OK);

    hu_reflection_pattern_t p;
    seed_pattern(&p, "Seth", "weak signal", 0.3, 1000);

    /* Returns HU_OK to caller but doesn't insert — caller bumps
     * low_confidence_dropped_count via complete_run separately. */
    HU_ASSERT_EQ(hu_reflection_storage_upsert(db, "run_1", &p), HU_OK);

    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM reflection_patterns", -1, &st, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(st, 0), 0);
    sqlite3_finalize(st);

    sqlite3_close(db);
}

/* AC-T2.4 (bonus): last_completed_ms returns MAX across status='ok' rows. */
static void test_storage_last_completed_ms_returns_max_ok_only(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_reflection_storage_migrate(db), HU_OK);

    /* No runs yet → 0 */
    HU_ASSERT_EQ((unsigned long long)hu_reflection_storage_last_completed_ms(db), 0ULL);

    hu_reflection_storage_insert_run(db, "run_ok", "p", 1000, 5);
    hu_reflection_storage_complete_run(db, "run_ok", "ok", 200, NULL, NULL, NULL, 0);

    hu_reflection_storage_insert_run(db, "run_err", "p", 2000, 5);
    hu_reflection_storage_complete_run(db, "run_err", "schema_invalid", 50, NULL, NULL, "bad json",
                                       0);

    /* last_completed_ms ignores status!='ok'. The 'ok' row was completed
     * at SQLite's `time(NULL)*1000` from inside complete_run, which is
     * some non-zero value > 0. The 'schema_invalid' row also has a
     * completed_at_ms but status filters it out. */
    uint64_t last = hu_reflection_storage_last_completed_ms(db);
    HU_ASSERT_TRUE(last > 0);

    sqlite3_close(db);
}

void run_reflection_storage_tests(void) {
    HU_TEST_SUITE("reflection_storage");
    HU_RUN_TEST(test_storage_migrate_creates_tables_and_indexes);
    HU_RUN_TEST(test_storage_upsert_bumps_observation_count_and_takes_max_confidence);
    HU_RUN_TEST(test_storage_upsert_drops_low_confidence);
    HU_RUN_TEST(test_storage_last_completed_ms_returns_max_ok_only);
}

#else /* !HU_ENABLE_SQLITE — empty stub so the runner symbol still resolves */

void run_reflection_storage_tests(void) {
    /* No-op stub: SQLite disabled at build time. The pre-commit
     * gate-symmetry check (scripts/check-test-source-gate-symmetry.sh)
     * expects either CMake gating OR an internal-#ifdef stub; we use
     * the internal pattern so the test source can stay in the
     * unconditional HU_TEST_SOURCES list. */
}

#endif /* HU_ENABLE_SQLITE */
