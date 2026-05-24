/* tests/test_m3_outcome_ring_population.c
 *
 * Spec 1 Task 7 + Task 8 (AC-M3-4 + expanded): pin
 *   - the population helper hu_m3_record_outcome_from_provider_result,
 *   - the drain marker semantics,
 *   - the SQLite-backed drainer and its daemon-tick interval gate.
 *
 * Gate symmetry (per .claude/rules/test-source-gate-symmetry.md):
 *   m3_frontier_adapter lives behind HU_ENABLE_ML. The drainer + table
 *   init are inside HU_ENABLE_SQLITE within HU_ENABLE_ML. The
 *   internal-#ifdef-wrap-with-stub-runner pattern lets this test source
 *   stay in the unconditional HU_TEST_SOURCES list.
 *
 * Production-symbol coverage (per
 * .claude/rules/test-references-production-symbol.md):
 * references hu_m3_record_outcome_from_provider_result,
 * hu_m3_frontier_adapter_drain_marker,
 * hu_m3_frontier_adapter_advance_drain_marker,
 * hu_m3_outcomes_init_table, hu_m3_drain_outcomes_to_sqlite,
 * hu_daemon_tick_m3_outcome_drain.
 */

#include "test_framework.h"

#ifdef HU_ENABLE_ML

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/m3_frontier_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hu_allocator_t A(void) {
    return hu_system_allocator();
}

/* Mirror tests/test_ml.c::open_fixture_adapter — write a tiny header to
 * disk and open. We re-implement (rather than extern-link) because the
 * helper lives behind file-scope static linkage in test_ml.c. */
static hu_m3_frontier_adapter_t *open_fixture(hu_allocator_t *alloc, const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return NULL;
    unsigned char blob[12];
    memcpy(blob, HU_M3_ADAPTER_MAGIC, 8);
    blob[8] = 1;
    blob[9] = 0;
    blob[10] = 0;
    blob[11] = 0;
    if (fwrite(blob, 1, sizeof(blob), fp) != sizeof(blob)) {
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    hu_m3_frontier_adapter_t *a = NULL;
    if (hu_m3_frontier_adapter_try_open(alloc, path, strlen(path), &a) != HU_OK)
        return NULL;
    return a;
}

/* ─────────────────────────────────────────────────────────────────────
 * Task 7 — hu_m3_record_outcome_from_provider_result populates the ring
 * ───────────────────────────────────────────────────────────────── */

static void test_m3_record_outcome_advances_ring_head(void) {
    hu_allocator_t alloc = A();
    hu_m3_frontier_adapter_t *a = open_fixture(&alloc, "/tmp/hu_m3_pop_head.bin");
    HU_ASSERT_NOT_NULL(a);

    HU_ASSERT_EQ((unsigned long)hu_m3_frontier_adapter_outcomes_recorded(a), 0ULL);

    HU_ASSERT_EQ(hu_m3_record_outcome_from_provider_result(a, /*ts*/ 1700000000000ULL,
                                                           /*pt*/ 100, /*ct*/ 50, /*lat*/ 250,
                                                           /*ch*/ 0xabcdULL, /*kind*/ 2),
                 HU_OK);
    HU_ASSERT_EQ((unsigned long)hu_m3_frontier_adapter_outcomes_recorded(a), 1ULL);

    /* NULL adapter must still be no-op (matches the rest of the API). */
    HU_ASSERT_EQ(hu_m3_record_outcome_from_provider_result(NULL, 0, 0, 0, 0, 0, 0), HU_OK);

    hu_m3_frontier_adapter_close(&alloc, a);
}

static void test_m3_record_outcome_captures_token_and_latency(void) {
    hu_allocator_t alloc = A();
    hu_m3_frontier_adapter_t *a = open_fixture(&alloc, "/tmp/hu_m3_pop_fields.bin");
    HU_ASSERT_NOT_NULL(a);

    const uint64_t ts = 1700000001234ULL;
    const uint32_t pt = 42;
    const uint32_t ct = 17;
    const uint64_t lat = 99;
    const uint64_t ch = 0x9999ULL;
    const uint8_t kind = 1;
    HU_ASSERT_EQ(hu_m3_record_outcome_from_provider_result(a, ts, pt, ct, lat, ch, kind), HU_OK);

    hu_m3_inference_outcome_t buf[2];
    size_t count = 0;
    HU_ASSERT_EQ(hu_m3_frontier_adapter_snapshot_outcomes(a, buf, 2, &count), HU_OK);
    HU_ASSERT_EQ((unsigned long)count, 1ULL);
    HU_ASSERT_EQ((unsigned long)buf[0].timestamp_unix_ms, (unsigned long)ts);
    HU_ASSERT_EQ((unsigned)buf[0].prompt_tokens, (unsigned)pt);
    HU_ASSERT_EQ((unsigned)buf[0].completion_tokens, (unsigned)ct);
    HU_ASSERT_EQ((unsigned long)buf[0].latency_ms, (unsigned long)lat);
    HU_ASSERT_EQ((unsigned long)buf[0].contact_id_hash, (unsigned long)ch);
    HU_ASSERT_EQ((unsigned)buf[0].turn_kind, (unsigned)kind);

    hu_m3_frontier_adapter_close(&alloc, a);
}

/* ─────────────────────────────────────────────────────────────────────
 * Task 8 — drain marker advance + persistence to SQLite + tick interval
 * ───────────────────────────────────────────────────────────────── */

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>

static sqlite3 *open_in_memory_db(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_EQ(hu_m3_outcomes_init_table(db), HU_OK);
    return db;
}

static int64_t count_persisted_outcomes(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    int64_t n = -1;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM m3_outcomes", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            n = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return n;
}

static void test_m3_outcome_drain_persists_to_sqlite(void) {
    hu_allocator_t alloc = A();
    hu_m3_frontier_adapter_t *a = open_fixture(&alloc, "/tmp/hu_m3_drain_persist.bin");
    HU_ASSERT_NOT_NULL(a);
    sqlite3 *db = open_in_memory_db();

    /* Record 3 outcomes. */
    for (int i = 0; i < 3; i++) {
        HU_ASSERT_EQ(hu_m3_record_outcome_from_provider_result(
                         a, /*ts*/ 1700000000000ULL + (uint64_t)i, /*pt*/ 10, /*ct*/ 20,
                         /*lat*/ 50, /*ch*/ 0xfeedULL, /*kind*/ 2),
                     HU_OK);
    }
    HU_ASSERT_EQ((unsigned long)hu_m3_frontier_adapter_outcomes_recorded(a), 3ULL);

    int64_t drained = -1;
    HU_ASSERT_EQ(
        hu_m3_drain_outcomes_to_sqlite(a, db, /*now_ms*/ 1700000000999LL, &alloc, 0, &drained),
        HU_OK);
    HU_ASSERT_EQ(drained, (int64_t)3);
    HU_ASSERT_EQ(count_persisted_outcomes(db), (int64_t)3);

    /* Schema fields populated correctly — spot-check first row. */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT timestamp_unix_ms, latency_ms, prompt_tokens, "
                                "completion_tokens, contact_id_hash, turn_kind FROM m3_outcomes "
                                "ORDER BY id LIMIT 1",
                                -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    HU_ASSERT_EQ((int64_t)sqlite3_column_int64(stmt, 0), (int64_t)1700000000000LL);
    HU_ASSERT_EQ((int64_t)sqlite3_column_int64(stmt, 1), (int64_t)50);
    HU_ASSERT_EQ((int)sqlite3_column_int(stmt, 2), 10);
    HU_ASSERT_EQ((int)sqlite3_column_int(stmt, 3), 20);
    HU_ASSERT_EQ((int64_t)sqlite3_column_int64(stmt, 4), (int64_t)0xfeed);
    HU_ASSERT_EQ((int)sqlite3_column_int(stmt, 5), 2);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
    hu_m3_frontier_adapter_close(&alloc, a);
}

static void test_m3_outcome_drain_advances_marker(void) {
    hu_allocator_t alloc = A();
    hu_m3_frontier_adapter_t *a = open_fixture(&alloc, "/tmp/hu_m3_drain_marker.bin");
    HU_ASSERT_NOT_NULL(a);
    sqlite3 *db = open_in_memory_db();

    /* Initial state — marker is 0. */
    HU_ASSERT_EQ((unsigned long)hu_m3_frontier_adapter_drain_marker(a), 0ULL);

    /* Write 2, drain — marker should advance to 2. */
    for (int i = 0; i < 2; i++) {
        HU_ASSERT_EQ(hu_m3_record_outcome_from_provider_result(a, 1700000000000ULL + (uint64_t)i, 5,
                                                               5, 10, 0x1, 1),
                     HU_OK);
    }
    int64_t drained = -1;
    HU_ASSERT_EQ(hu_m3_drain_outcomes_to_sqlite(a, db, 1LL, &alloc, 0, &drained), HU_OK);
    HU_ASSERT_EQ(drained, (int64_t)2);
    HU_ASSERT_EQ((unsigned long)hu_m3_frontier_adapter_drain_marker(a), 2ULL);

    /* Drain again with no new records — drained == 0, marker unchanged. */
    drained = -1;
    HU_ASSERT_EQ(hu_m3_drain_outcomes_to_sqlite(a, db, 2LL, &alloc, 0, &drained), HU_OK);
    HU_ASSERT_EQ(drained, (int64_t)0);
    HU_ASSERT_EQ((unsigned long)hu_m3_frontier_adapter_drain_marker(a), 2ULL);
    /* SQLite row count unchanged. */
    HU_ASSERT_EQ(count_persisted_outcomes(db), (int64_t)2);

    /* Write 1 more, drain — only the new record persists, marker = 3. */
    HU_ASSERT_EQ(hu_m3_record_outcome_from_provider_result(a, 1700000000099ULL, 7, 7, 11, 0x2, 1),
                 HU_OK);
    HU_ASSERT_EQ(hu_m3_drain_outcomes_to_sqlite(a, db, 3LL, &alloc, 0, &drained), HU_OK);
    HU_ASSERT_EQ(drained, (int64_t)1);
    HU_ASSERT_EQ((unsigned long)hu_m3_frontier_adapter_drain_marker(a), 3ULL);
    HU_ASSERT_EQ(count_persisted_outcomes(db), (int64_t)3);

    /* Advance attempt with a lower marker value is a no-op (monotonic). */
    hu_m3_frontier_adapter_advance_drain_marker(a, 1);
    HU_ASSERT_EQ((unsigned long)hu_m3_frontier_adapter_drain_marker(a), 3ULL);

    sqlite3_close(db);
    hu_m3_frontier_adapter_close(&alloc, a);
}

static void test_m3_outcome_drain_tick_respects_interval(void) {
    hu_allocator_t alloc = A();
    hu_m3_frontier_adapter_t *a = open_fixture(&alloc, "/tmp/hu_m3_drain_tick.bin");
    HU_ASSERT_NOT_NULL(a);
    sqlite3 *db = open_in_memory_db();
    hu_daemon_tick_m3_outcome_drain_reset_warn_guards_for_test();

    /* Record 2 outcomes. */
    HU_ASSERT_EQ(hu_m3_record_outcome_from_provider_result(a, 1ULL, 1, 1, 1, 0x1, 1), HU_OK);
    HU_ASSERT_EQ(hu_m3_record_outcome_from_provider_result(a, 2ULL, 1, 1, 1, 0x1, 1), HU_OK);

    const int64_t now_ms = 1000LL * 1000;
    const int64_t interval_sec = 300; /* 5 min */
    int64_t last_run = 0;
    int64_t drained = -1;

    /* First tick — runs (watermark is 0). */
    HU_ASSERT_EQ(
        hu_daemon_tick_m3_outcome_drain(a, db, now_ms, &last_run, interval_sec, &alloc, &drained),
        HU_OK);
    HU_ASSERT_EQ(last_run, now_ms);
    HU_ASSERT_EQ(drained, (int64_t)2);

    /* Second tick 1 minute later — interval-gated, skipped. */
    int64_t now2 = now_ms + (60LL * 1000);
    drained = -1;
    HU_ASSERT_EQ(
        hu_daemon_tick_m3_outcome_drain(a, db, now2, &last_run, interval_sec, &alloc, &drained),
        HU_OK);
    HU_ASSERT_EQ(last_run, now_ms);    /* unchanged */
    HU_ASSERT_EQ(drained, (int64_t)0); /* no drain ran */

    /* Add another record — still gated. */
    HU_ASSERT_EQ(hu_m3_record_outcome_from_provider_result(a, 3ULL, 1, 1, 1, 0x1, 1), HU_OK);
    drained = -1;
    HU_ASSERT_EQ(
        hu_daemon_tick_m3_outcome_drain(a, db, now2, &last_run, interval_sec, &alloc, &drained),
        HU_OK);
    HU_ASSERT_EQ(drained, (int64_t)0);

    /* Third tick after 6 minutes — runs again. */
    int64_t now3 = now_ms + (6LL * 60 * 1000);
    drained = -1;
    HU_ASSERT_EQ(
        hu_daemon_tick_m3_outcome_drain(a, db, now3, &last_run, interval_sec, &alloc, &drained),
        HU_OK);
    HU_ASSERT_EQ(last_run, now3);
    HU_ASSERT_EQ(drained, (int64_t)1);
    HU_ASSERT_EQ(count_persisted_outcomes(db), (int64_t)3);

    sqlite3_close(db);
    hu_m3_frontier_adapter_close(&alloc, a);
}

static void test_m3_outcome_drain_handles_empty_ring(void) {
    hu_allocator_t alloc = A();
    hu_m3_frontier_adapter_t *a = open_fixture(&alloc, "/tmp/hu_m3_drain_empty.bin");
    HU_ASSERT_NOT_NULL(a);
    sqlite3 *db = open_in_memory_db();

    int64_t drained = -1;
    HU_ASSERT_EQ(hu_m3_drain_outcomes_to_sqlite(a, db, 1LL, &alloc, 0, &drained), HU_OK);
    HU_ASSERT_EQ(drained, (int64_t)0);
    HU_ASSERT_EQ((unsigned long)hu_m3_frontier_adapter_drain_marker(a), 0ULL);
    HU_ASSERT_EQ(count_persisted_outcomes(db), (int64_t)0);

    /* NULL adapter is a no-op. */
    drained = -1;
    HU_ASSERT_EQ(hu_m3_drain_outcomes_to_sqlite(NULL, db, 1LL, &alloc, 0, &drained), HU_OK);
    HU_ASSERT_EQ(drained, (int64_t)0);

    /* NULL db is invalid-argument. */
    drained = -1;
    HU_ASSERT_EQ(hu_m3_drain_outcomes_to_sqlite(a, NULL, 1LL, &alloc, 0, &drained),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(drained, (int64_t)0);

    /* daemon-tick with NULL db is HU_OK + once-guard log (no error). */
    hu_daemon_tick_m3_outcome_drain_reset_warn_guards_for_test();
    int64_t last_run = 0;
    drained = -1;
    HU_ASSERT_EQ(hu_daemon_tick_m3_outcome_drain(a, NULL, 1LL, &last_run, 60, &alloc, &drained),
                 HU_OK);
    HU_ASSERT_EQ(drained, (int64_t)0);

    sqlite3_close(db);
    hu_m3_frontier_adapter_close(&alloc, a);
}

#endif /* HU_ENABLE_SQLITE */

void run_m3_outcome_ring_population_tests(void);
void run_m3_outcome_ring_population_tests(void) {
    HU_TEST_SUITE("m3_outcome_ring_population");
    HU_RUN_TEST(test_m3_record_outcome_advances_ring_head);
    HU_RUN_TEST(test_m3_record_outcome_captures_token_and_latency);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_m3_outcome_drain_persists_to_sqlite);
    HU_RUN_TEST(test_m3_outcome_drain_advances_marker);
    HU_RUN_TEST(test_m3_outcome_drain_tick_respects_interval);
    HU_RUN_TEST(test_m3_outcome_drain_handles_empty_ring);
#endif
}

#else /* !HU_ENABLE_ML — stub runner */

void run_m3_outcome_ring_population_tests(void);
void run_m3_outcome_ring_population_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_ML */
