/* tests/test_outbound_pipeline_perf.c
 *
 * Sprint 60 follow-up — pipeline microbenchmark. Measures latency
 * through the full HU_OUTBOUND_PATH_PROACTIVE pipeline so a future
 * regression that doubles per-stage cost is caught at PR time, not
 * in production.
 *
 * Setup: in-memory SQLite db seeded with 50 OTHER-contact messages
 * (representative of mid-life production daemon state where the
 * crosstalk lookup has real work to do). PROACTIVE pipeline built
 * once, reused for every iteration.
 *
 * Measurement: clock_gettime(CLOCK_MONOTONIC) sandwich around
 * hu_outbound_pipeline_run, latencies stored in a sorted array,
 * P50/P95/P99 picked by index.
 *
 * Gated on HU_ENABLE_SQLITE (the crosstalk SQLite path is what we
 * want to measure under realistic conditions).
 */

#include "test_framework.h"

#include "human/agent/outbound_crosstalk_sqlite.h"
#include "human/agent/outbound_pipeline.h"
#include "human/agent/outbound_stats.h"
#include "human/core/allocator.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Per-iteration latency budget — 5ms P99 against a 50-contact
 * crosstalk corpus, non-instrumented build.
 *
 * Observed steady-state P99 is ~0.25ms, so 5ms gives ~20× headroom
 * — enough to absorb 2–3× noise on a loaded CI runner while still
 * catching a 10× regression (e.g. a future stage adding synchronous
 * I/O, an N+1 SQLite query, or a classifier going from O(1) to
 * O(corpus size)).
 *
 * If a future PR trips this, the right response is to inspect WHY
 * P99 climbed, not to raise the budget. */
#define HU_PIPELINE_PERF_P99_BUDGET_NS (5 * 1000 * 1000) /* 5ms */

/* Under ASan the P99 tail is dominated by instrumentation + host-load
 * noise (measured 46–95ms on a loaded dev Mac vs ~0.25ms P50), so the
 * budget assertion is skipped — the measurement still runs (ASan leak
 * coverage of the pipeline path) and the numbers are still printed.
 * CI's non-ASan test preset enforces the budget. */
#if defined(__SANITIZE_ADDRESS__)
#define HU_PIPELINE_PERF_UNDER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HU_PIPELINE_PERF_UNDER_ASAN 1
#endif
#endif
#ifndef HU_PIPELINE_PERF_UNDER_ASAN
#define HU_PIPELINE_PERF_UNDER_ASAN 0
#endif

/* Warm-up + measurement counts.
 *
 * 100 warm-up iterations let SQLite's page cache and prepared
 * statements settle; the first 1–5 iterations are 5–10× slower
 * than steady state and would otherwise dominate P99.
 *
 * 1000 measured iterations is enough samples for a stable P99
 * (P99 = sample[989] out of 1000) without making the test
 * annoyingly slow (~3s total wall time on dev hardware). */
#define HU_PIPELINE_PERF_WARMUP_N  100
#define HU_PIPELINE_PERF_MEASURE_N 1000

static int compare_u64(const void *a, const void *b) {
    uint64_t ua = *(const uint64_t *)a;
    uint64_t ub = *(const uint64_t *)b;
    return (ua > ub) - (ua < ub);
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static const char *perf_db_path(void) {
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_pipeline_perf_%d.db", (int)getpid());
    return path;
}

static void seed_db(sqlite3 *db, int n_contacts) {
    char *err = NULL;
    sqlite3_exec(db,
                 "CREATE TABLE messages("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "session_id TEXT NOT NULL,"
                 "role TEXT NOT NULL,"
                 "content TEXT NOT NULL,"
                 "created_at TEXT DEFAULT(datetime('now')))",
                 NULL, NULL, &err);
    if (err)
        sqlite3_free(err);
    /* Insert n_contacts distinct OTHER contacts, each with one
     * recent message. Realistic mid-life daemon state. */
    for (int i = 0; i < n_contacts; i++) {
        char sql[512];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO messages(session_id, role, content, created_at) "
                 "VALUES('+CONTACT_%03d', 'assistant', "
                 "'realistic prose for the cross-contact pool %d', "
                 "datetime('now', '-%d hours'))",
                 i, i, i % 168);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
}

/* The actual measurement loop. Returns 0 on success; sets *out_p50_ns,
 * *out_p95_ns, *out_p99_ns from the sorted measurement-phase samples. */
static int run_perf_measurement(uint64_t *out_p50_ns, uint64_t *out_p95_ns, uint64_t *out_p99_ns) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    unlink(perf_db_path());
    if (sqlite3_open(perf_db_path(), &db) != SQLITE_OK)
        return -1;
    seed_db(db, 50); /* 50 OTHER contacts = realistic mid-life corpus */
    hu_outbound_crosstalk_register_sqlite(db);

    hu_outbound_pipeline_t *pipe = NULL;
    if (hu_outbound_pipeline_for_path(&alloc, HU_OUTBOUND_PATH_PROACTIVE, &pipe) != HU_OK) {
        hu_outbound_crosstalk_unregister_sqlite();
        sqlite3_close(db);
        return -1;
    }

    /* Reset stats so this benchmark doesn't pollute another test's
     * snapshot. */
    hu_outbound_stats_reset_for_test();

    /* Warm-up phase: drive HU_PIPELINE_PERF_WARMUP_N iterations
     * with results discarded. The pipeline's first call typically
     * pays a one-time sqlite-prepare cost; subsequent calls hit
     * the prepared-statement cache. */
    for (int i = 0; i < HU_PIPELINE_PERF_WARMUP_N; i++) {
        const char *clean = "warm-up message";
        size_t len = strlen(clean);
        char *content = (char *)alloc.alloc(alloc.ctx, len + 1);
        memcpy(content, clean, len + 1);
        hu_outbound_message_t msg = {.content = content, .content_len = len};
        hu_outbound_context_t ctx = {.alloc = &alloc,
                                     .path = HU_OUTBOUND_PATH_PROACTIVE,
                                     .recipient_contact_id = "+RECIPIENT",
                                     .recipient_contact_id_len = 10,
                                     .regenerate_budget = 1};
        hu_outbound_verdict_t v = {0};
        hu_outbound_pipeline_run(pipe, &msg, &ctx, &v);
        hu_outbound_verdict_clear(&v, &alloc);
        if (msg.content)
            alloc.free(alloc.ctx, msg.content, msg.content_len + 1);
    }

    /* Measurement phase: HU_PIPELINE_PERF_MEASURE_N iterations,
     * each timed and recorded. */
    uint64_t *samples = (uint64_t *)calloc(HU_PIPELINE_PERF_MEASURE_N, sizeof(uint64_t));
    if (!samples) {
        hu_outbound_pipeline_destroy(pipe);
        hu_outbound_crosstalk_unregister_sqlite();
        sqlite3_close(db);
        return -1;
    }

    for (int i = 0; i < HU_PIPELINE_PERF_MEASURE_N; i++) {
        const char *clean = "hope your week is going well";
        size_t len = strlen(clean);
        char *content = (char *)alloc.alloc(alloc.ctx, len + 1);
        memcpy(content, clean, len + 1);
        hu_outbound_message_t msg = {.content = content, .content_len = len};
        hu_outbound_context_t ctx = {.alloc = &alloc,
                                     .path = HU_OUTBOUND_PATH_PROACTIVE,
                                     .recipient_contact_id = "+RECIPIENT",
                                     .recipient_contact_id_len = 10,
                                     .regenerate_budget = 1};
        hu_outbound_verdict_t v = {0};

        uint64_t t0 = now_ns();
        hu_outbound_pipeline_run(pipe, &msg, &ctx, &v);
        uint64_t t1 = now_ns();
        samples[i] = t1 - t0;

        hu_outbound_verdict_clear(&v, &alloc);
        if (msg.content)
            alloc.free(alloc.ctx, msg.content, msg.content_len + 1);
    }

    qsort(samples, HU_PIPELINE_PERF_MEASURE_N, sizeof(uint64_t), compare_u64);
    *out_p50_ns = samples[HU_PIPELINE_PERF_MEASURE_N / 2];
    *out_p95_ns = samples[HU_PIPELINE_PERF_MEASURE_N * 95 / 100];
    *out_p99_ns = samples[HU_PIPELINE_PERF_MEASURE_N * 99 / 100];

    free(samples);
    hu_outbound_pipeline_destroy(pipe);
    hu_outbound_crosstalk_unregister_sqlite();
    sqlite3_close(db);
    unlink(perf_db_path());
    return 0;
}

/* P99 of the proactive pipeline against a 50-contact corpus must
 * stay under the budget. A regression that doubles per-stage cost
 * (e.g., a future stage doing synchronous I/O) trips this. */
static void test_pipeline_p99_under_budget(void) {
    if (HU_PIPELINE_PERF_MEASURE_N == 0) {
        /* Test is dormant until the user contributes the warm-up/
         * measurement-N decision. Mark with a non-fatal note. */
        fprintf(stderr, "  [SKIP] pipeline perf — WARMUP_N + MEASURE_N awaiting contribution\n");
        return;
    }
    uint64_t p50 = 0, p95 = 0, p99 = 0;
    int rc = run_perf_measurement(&p50, &p95, &p99);
    HU_ASSERT_EQ(rc, 0);

    /* Print so operators reading test output see the actual numbers,
     * not just pass/fail. */
    fprintf(stderr, "  pipeline P50=%.3f ms  P95=%.3f ms  P99=%.3f ms (budget %d ms)\n", p50 / 1e6,
            p95 / 1e6, p99 / 1e6, HU_PIPELINE_PERF_P99_BUDGET_NS / 1000000);

#if HU_PIPELINE_PERF_UNDER_ASAN
    fprintf(stderr, "  [SKIP] p99 budget not asserted under ASan\n");
#else
    HU_ASSERT_TRUE(p99 < HU_PIPELINE_PERF_P99_BUDGET_NS);
#endif
}

void run_outbound_pipeline_perf_tests(void) {
    HU_TEST_SUITE("outbound_pipeline_perf");
    /* Quarantined: the p99<budget assertion is timing-sensitive and flakes on
     * loaded shared CI runners. HU_RUN_TEST_FLAKY retries the measurement up to
     * hu__flaky_retries+1 times, failing only if p99 exceeds budget on EVERY
     * attempt — kills the false-red while still catching a consistent regression.
     * Do NOT widen HU_PIPELINE_PERF_P99_BUDGET_NS to mask a real regression. */
    HU_RUN_TEST_FLAKY(test_pipeline_p99_under_budget);
    hu_outbound_stats_reset_for_test();
    hu_outbound_crosstalk_unregister_sqlite();
}
