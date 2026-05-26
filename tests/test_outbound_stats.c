/* tests/test_outbound_stats.c
 *
 * Sprint 60 — per-stage × per-verdict counters that back the doctor
 * `/v1/outbound/stats` check. Pins the contract:
 *
 *   1. hu_outbound_stats_record bumps the (stage, verdict) cell.
 *   2. Snapshot reads return cumulative counts.
 *   3. Stage names from pipeline_configs.c map to enum ids.
 *   4. Unknown stage names route to the OTHER bucket so we never
 *      drop a count silently.
 *   5. Verdict kinds outside [0..3] are clamped (defensive).
 *   6. reset_for_test zeros everything (testability — production
 *      has no reset path).
 *   7. Thread-safety smoke test — concurrent records preserve the
 *      total, no races.
 */

#include "test_framework.h"

#include "human/agent/outbound_stats.h"
#include <pthread.h>
#include <string.h>

/* ── Name → enum mapping ─────────────────────────────────────────── */

static void test_stage_from_name_maps_known_stages(void) {
    HU_ASSERT_EQ(hu_outbound_stats_stage_from_name("strip"), HU_OUTBOUND_STATS_STAGE_STRIP);
    HU_ASSERT_EQ(hu_outbound_stats_stage_from_name("shape"), HU_OUTBOUND_STATS_STAGE_SHAPE);
    HU_ASSERT_EQ(hu_outbound_stats_stage_from_name("echo"), HU_OUTBOUND_STATS_STAGE_ECHO);
    HU_ASSERT_EQ(hu_outbound_stats_stage_from_name("crosstalk"),
                 HU_OUTBOUND_STATS_STAGE_CROSSTALK);
    HU_ASSERT_EQ(hu_outbound_stats_stage_from_name("persona"), HU_OUTBOUND_STATS_STAGE_PERSONA);
    HU_ASSERT_EQ(hu_outbound_stats_stage_from_name("moderation"),
                 HU_OUTBOUND_STATS_STAGE_MODERATION);
}

static void test_stage_from_name_unknown_falls_to_other(void) {
    /* Unknown names route to OTHER bucket — never drop a count. */
    HU_ASSERT_EQ(hu_outbound_stats_stage_from_name("unknown_stage"),
                 HU_OUTBOUND_STATS_STAGE_OTHER);
    HU_ASSERT_EQ(hu_outbound_stats_stage_from_name(""), HU_OUTBOUND_STATS_STAGE_OTHER);
    HU_ASSERT_EQ(hu_outbound_stats_stage_from_name(NULL), HU_OUTBOUND_STATS_STAGE_OTHER);
}

static void test_stage_name_returns_stable_strings(void) {
    HU_ASSERT_STR_EQ(hu_outbound_stats_stage_name(HU_OUTBOUND_STATS_STAGE_STRIP), "strip");
    HU_ASSERT_STR_EQ(hu_outbound_stats_stage_name(HU_OUTBOUND_STATS_STAGE_CROSSTALK), "crosstalk");
    HU_ASSERT_STR_EQ(hu_outbound_stats_stage_name(HU_OUTBOUND_STATS_STAGE_OTHER), "other");
    /* Never NULL — even for invalid ids. */
    HU_ASSERT_NOT_NULL(hu_outbound_stats_stage_name((hu_outbound_stats_stage_t)999));
}

/* ── Record + snapshot ───────────────────────────────────────────── */

static void test_record_bumps_correct_cell(void) {
    hu_outbound_stats_reset_for_test();

    /* SEND=0, REWRITE=1, REGENERATE=2, REJECT=3 — match
     * hu_outbound_verdict_kind_t values. */
    hu_outbound_stats_record("crosstalk", 3); /* REJECT */
    hu_outbound_stats_record("crosstalk", 3);
    hu_outbound_stats_record("crosstalk", 0); /* SEND */
    hu_outbound_stats_record("persona", 2);   /* REGENERATE */

    hu_outbound_stats_snapshot_t snap = {0};
    HU_ASSERT_EQ(hu_outbound_stats_snapshot(&snap), HU_OK);

    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_CROSSTALK][3], 2u);
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_CROSSTALK][0], 1u);
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_PERSONA][2], 1u);
    /* Untouched cells stay at 0. */
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_STRIP][0], 0u);
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_PERSONA][3], 0u);
}

static void test_record_unknown_stage_routes_to_other(void) {
    hu_outbound_stats_reset_for_test();
    hu_outbound_stats_record("not_a_real_stage", 0);
    hu_outbound_stats_record(NULL, 3);

    hu_outbound_stats_snapshot_t snap = {0};
    HU_ASSERT_EQ(hu_outbound_stats_snapshot(&snap), HU_OK);
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_OTHER][0], 1u);
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_OTHER][3], 1u);
}

static void test_record_out_of_range_verdict_clamps_safely(void) {
    hu_outbound_stats_reset_for_test();
    /* -1, 4, 999 are out of [0..3] — must not crash, must not
     * corrupt the table by indexing outside the verdict array. */
    hu_outbound_stats_record("strip", -1);
    hu_outbound_stats_record("strip", 4);
    hu_outbound_stats_record("strip", 999);
    hu_outbound_stats_record("strip", 0); /* valid; should still record */

    hu_outbound_stats_snapshot_t snap = {0};
    HU_ASSERT_EQ(hu_outbound_stats_snapshot(&snap), HU_OK);
    /* Valid record landed. Cells outside [0..3] must NOT show up
     * — strip[0] is the only cell that should be nonzero. */
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_STRIP][0], 1u);
}

static void test_snapshot_null_out_rejects(void) {
    HU_ASSERT_EQ(hu_outbound_stats_snapshot(NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_reset_zeros_all_cells(void) {
    hu_outbound_stats_reset_for_test();
    hu_outbound_stats_record("strip", 0);
    hu_outbound_stats_record("crosstalk", 3);

    hu_outbound_stats_reset_for_test();
    hu_outbound_stats_snapshot_t snap = {0};
    HU_ASSERT_EQ(hu_outbound_stats_snapshot(&snap), HU_OK);
    /* Every cell back to zero. */
    for (size_t s = 0; s < HU_OUTBOUND_STATS_STAGE_COUNT; s++) {
        for (size_t v = 0; v < HU_OUTBOUND_STATS_VERDICT_COUNT; v++) {
            HU_ASSERT_EQ(snap.counts[s][v], 0u);
        }
    }
}

/* ── Thread-safety smoke test ────────────────────────────────────── */

#define STATS_CONCURRENT_THREADS 4
#define STATS_RECORDS_PER_THREAD 1000

static void *stats_concurrent_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < STATS_RECORDS_PER_THREAD; i++) {
        hu_outbound_stats_record("crosstalk", 3); /* REJECT */
    }
    return NULL;
}

/* Concurrent records must preserve the total — atomic increment
 * with no race. Without atomics this would lose updates. */
static void test_record_is_thread_safe(void) {
    hu_outbound_stats_reset_for_test();

    pthread_t threads[STATS_CONCURRENT_THREADS];
    for (int t = 0; t < STATS_CONCURRENT_THREADS; t++)
        pthread_create(&threads[t], NULL, stats_concurrent_worker, NULL);
    for (int t = 0; t < STATS_CONCURRENT_THREADS; t++)
        pthread_join(threads[t], NULL);

    hu_outbound_stats_snapshot_t snap = {0};
    HU_ASSERT_EQ(hu_outbound_stats_snapshot(&snap), HU_OK);
    uint64_t expected = (uint64_t)STATS_CONCURRENT_THREADS * STATS_RECORDS_PER_THREAD;
    HU_ASSERT_EQ(snap.counts[HU_OUTBOUND_STATS_STAGE_CROSSTALK][3], expected);
}

void run_outbound_stats_tests(void) {
    HU_TEST_SUITE("outbound_stats");
    HU_RUN_TEST(test_stage_from_name_maps_known_stages);
    HU_RUN_TEST(test_stage_from_name_unknown_falls_to_other);
    HU_RUN_TEST(test_stage_name_returns_stable_strings);
    HU_RUN_TEST(test_record_bumps_correct_cell);
    HU_RUN_TEST(test_record_unknown_stage_routes_to_other);
    HU_RUN_TEST(test_record_out_of_range_verdict_clamps_safely);
    HU_RUN_TEST(test_snapshot_null_out_rejects);
    HU_RUN_TEST(test_reset_zeros_all_cells);
    HU_RUN_TEST(test_record_is_thread_safe);
    /* Defensive — leave counter table clean for other suites. */
    hu_outbound_stats_reset_for_test();
}
