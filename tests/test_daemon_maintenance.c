/* Contract tests for hu_daemon_prompt_budget_flush and
 * hu_daemon_verifier_metrics_flush — the cadence gates the once-per-minute
 * maintenance tick (src/daemon/daemon_maintenance.c) uses to persist
 * ~/.human/prompt_budget.snapshot.json and ~/.human/verifier_metrics.json
 * for the doctor CLI.
 *
 * The tick itself is compiled out under HU_IS_TEST, so the gate is extracted
 * and pinned here. Pins the 2026-09-06 doctor false alarm ("prompt_budget
 * snapshot is 157 seconds old (>120)"): the old inline gate only SEEDED its
 * timestamp on the first tick and then required a full 60 s monotonic gap on
 * ticks that arrive every 60 s, so a fresh daemon left the previous process's
 * file aging for 2-3 minutes and a steady-state daemon skipped every tick
 * whose gap landed a few ms short.
 *
 *   (a) the first call flushes immediately (restart refreshes the file)
 *   (b) a call inside HU_DAEMON_FLUSH_MIN_GAP_MS does not flush
 *   (c) a >60 s tick with ZERO new observations rewrites the file (mtime
 *       advances, content is regenerated)
 *   (d) minute ticks whose monotonic gap is a few ms short of 60 s still
 *       flush — no alternate-minute skipping
 *   (e) NULL budget / NULL state are no-ops
 *
 * The verifier-metrics flush shared the same inline gate shape and is pinned
 * to the same contract (f-i below); its writer keys on $HOME, so those tests
 * pin HOME to a private tmp dir the way tests/test_verifier_metrics.c does.
 */

#include "human/agent/prompt_budget.h"
#include "human/agent/verifier_metrics.h"
#include "human/core/allocator.h"
#include "human/daemon_maintenance.h"
#include "test_framework.h"
#include "test_tmpdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct pb_fixture {
    char dir[256];
    char path[320];
    hu_allocator_t alloc;
    hu_prompt_budget_t *budget;
} pb_fixture_t;

static void pb_fixture_setup(pb_fixture_t *f) {
    memset(f, 0, sizeof(*f));
    HU_ASSERT_TRUE(hu_test_mkdtemp("/tmp/hu_daemon_pb_flush_", f->dir, sizeof(f->dir)));
    snprintf(f->path, sizeof(f->path), "%s/prompt_budget.snapshot.json", f->dir);
    hu_prompt_budget_snapshot_set_path_for_test(f->path);
    f->alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_prompt_budget_init(&f->alloc, &f->budget), HU_OK);
}

static void pb_fixture_teardown(pb_fixture_t *f) {
    hu_prompt_budget_free(f->budget);
    hu_prompt_budget_snapshot_set_path_for_test(NULL);
    hu_test_rm_rf(f->dir);
}

/* Nanosecond mtime so two writes inside one second are still ordered. */
static int64_t mtime_ns(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
#ifdef __APPLE__
    return (int64_t)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    return (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
}

static long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1L;
}

static void test_pb_flush_first_tick_writes_snapshot(void) {
    pb_fixture_t f;
    pb_fixture_setup(&f);
    int64_t last = 0;

    HU_ASSERT_EQ(file_size(f.path), -1L); /* precondition: nothing on disk */
    HU_ASSERT_TRUE(hu_daemon_prompt_budget_flush(f.budget, 1000, &last));
    HU_ASSERT(file_size(f.path) > 0);
    HU_ASSERT_EQ(last, (int64_t)1000);

    pb_fixture_teardown(&f);
}

static void test_pb_flush_skips_inside_min_gap(void) {
    pb_fixture_t f;
    pb_fixture_setup(&f);
    int64_t last = 0;
    HU_ASSERT_TRUE(hu_daemon_prompt_budget_flush(f.budget, 1000, &last));
    HU_ASSERT_EQ(unlink(f.path), 0);

    int64_t inside = 1000 + HU_DAEMON_FLUSH_MIN_GAP_MS - 1;
    HU_ASSERT_FALSE(hu_daemon_prompt_budget_flush(f.budget, inside, &last));
    HU_ASSERT_EQ(file_size(f.path), -1L);
    HU_ASSERT_EQ(last, (int64_t)1000); /* state untouched when not due */

    pb_fixture_teardown(&f);
}

static void test_pb_flush_rewrites_after_60s_tick_with_zero_turns(void) {
    pb_fixture_t f;
    pb_fixture_setup(&f);
    int64_t last = 0;
    HU_ASSERT_TRUE(hu_daemon_prompt_budget_flush(f.budget, 1000, &last));
    int64_t first_mtime = mtime_ns(f.path);
    HU_ASSERT(first_mtime > 0);

    /* Truncate the file so a rewrite is unambiguous even if the clock
     * granularity collapses two writes into one timestamp. */
    FILE *fp = fopen(f.path, "w");
    HU_ASSERT_NOT_NULL(fp);
    fclose(fp);
    HU_ASSERT_EQ(file_size(f.path), 0L);

    /* No hu_prompt_budget_observe in between: an idle daemon must still
     * refresh the file so its age never reads as "daemon not flushing". */
    HU_ASSERT_TRUE(hu_daemon_prompt_budget_flush(f.budget, 1000 + 60001, &last));
    HU_ASSERT(file_size(f.path) > 0);
    HU_ASSERT(mtime_ns(f.path) >= first_mtime);
    HU_ASSERT_EQ(last, (int64_t)(1000 + 60001));

    pb_fixture_teardown(&f);
}

static void test_pb_flush_minute_ticks_with_short_gap_never_skip(void) {
    pb_fixture_t f;
    pb_fixture_setup(&f);
    int64_t last = 0;
    /* Cron ticks land at minute boundaries plus loop jitter; the monotonic
     * gap between consecutive ticks is therefore 60 000 ms +/- a few ms.
     * Every tick must flush — the old >= 60000 gate skipped the short ones. */
    const int64_t ticks[] = {5000, 5000 + 59990, 5000 + 59990 + 59995,
                             5000 + 59990 + 59995 + 60003};
    for (size_t i = 0; i < sizeof(ticks) / sizeof(ticks[0]); i++) {
        if (i > 0)
            HU_ASSERT_EQ(unlink(f.path), 0);
        HU_ASSERT_TRUE(hu_daemon_prompt_budget_flush(f.budget, ticks[i], &last));
        HU_ASSERT(file_size(f.path) > 0);
        HU_ASSERT_EQ(last, ticks[i]);
    }
    pb_fixture_teardown(&f);
}

static void test_pb_flush_null_inputs_are_noops(void) {
    pb_fixture_t f;
    pb_fixture_setup(&f);
    int64_t last = 0;
    HU_ASSERT_FALSE(hu_daemon_prompt_budget_flush(NULL, 1000, &last));
    HU_ASSERT_EQ(last, (int64_t)0);
    HU_ASSERT_FALSE(hu_daemon_prompt_budget_flush(f.budget, 1000, NULL));
    HU_ASSERT_EQ(file_size(f.path), -1L);
    pb_fixture_teardown(&f);
}

/* ── verifier metrics ─────────────────────────────────────────────────── */

typedef struct vm_fixture {
    char home[256];
    char path[320];
    char *prev_home;
} vm_fixture_t;

static void vm_fixture_setup(vm_fixture_t *f) {
    memset(f, 0, sizeof(*f));
    HU_ASSERT_TRUE(hu_test_mkdtemp("/tmp/hu_daemon_vm_flush_", f->home, sizeof(f->home)));
    snprintf(f->path, sizeof(f->path), "%s/.human/verifier_metrics.json", f->home);
    const char *prev = getenv("HOME");
    f->prev_home = prev ? strdup(prev) : NULL;
    setenv("HOME", f->home, 1);
}

static void vm_fixture_teardown(vm_fixture_t *f) {
    if (f->prev_home) {
        setenv("HOME", f->prev_home, 1);
        free(f->prev_home);
    } else {
        unsetenv("HOME");
    }
    hu_test_rm_rf(f->home);
}

static const hu_verifier_metrics_t vm_sample = {.total_runs = 7,
                                                .total_claims_extracted = 21,
                                                .total_claims_flagged = 3,
                                                .last_update_epoch = 0};

static void test_vm_flush_first_tick_writes_file(void) {
    vm_fixture_t f;
    vm_fixture_setup(&f);
    int64_t last = 0;
    hu_verifier_metrics_t snap = vm_sample;

    HU_ASSERT_EQ(file_size(f.path), -1L);
    HU_ASSERT_TRUE(hu_daemon_verifier_metrics_flush(&snap, 1000, &last));
    HU_ASSERT(file_size(f.path) > 0);
    HU_ASSERT_EQ(last, (int64_t)1000);
    /* The caller's snapshot is not mutated by save()'s epoch stamp. */
    HU_ASSERT_EQ(snap.last_update_epoch, (int64_t)0);

    hu_verifier_metrics_t back;
    HU_ASSERT_EQ(hu_verifier_metrics_load(&back), HU_OK);
    HU_ASSERT_EQ(back.total_runs, (uint64_t)7);
    HU_ASSERT_EQ(back.total_claims_flagged, (uint64_t)3);
    HU_ASSERT(back.last_update_epoch > 0);

    vm_fixture_teardown(&f);
}

static void test_vm_flush_skips_inside_min_gap(void) {
    vm_fixture_t f;
    vm_fixture_setup(&f);
    int64_t last = 0;
    HU_ASSERT_TRUE(hu_daemon_verifier_metrics_flush(&vm_sample, 1000, &last));
    HU_ASSERT_EQ(unlink(f.path), 0);

    int64_t inside = 1000 + HU_DAEMON_FLUSH_MIN_GAP_MS - 1;
    HU_ASSERT_FALSE(hu_daemon_verifier_metrics_flush(&vm_sample, inside, &last));
    HU_ASSERT_EQ(file_size(f.path), -1L);
    HU_ASSERT_EQ(last, (int64_t)1000);

    vm_fixture_teardown(&f);
}

static void test_vm_flush_rewrites_after_60s_tick_with_zero_turns(void) {
    vm_fixture_t f;
    vm_fixture_setup(&f);
    int64_t last = 0;
    HU_ASSERT_TRUE(hu_daemon_verifier_metrics_flush(&vm_sample, 1000, &last));
    int64_t first_mtime = mtime_ns(f.path);
    HU_ASSERT(first_mtime > 0);

    FILE *fp = fopen(f.path, "w");
    HU_ASSERT_NOT_NULL(fp);
    fclose(fp);
    HU_ASSERT_EQ(file_size(f.path), 0L);

    /* Counters unchanged — an idle daemon must still refresh the heartbeat. */
    HU_ASSERT_TRUE(hu_daemon_verifier_metrics_flush(&vm_sample, 1000 + 60001, &last));
    HU_ASSERT(file_size(f.path) > 0);
    HU_ASSERT(mtime_ns(f.path) >= first_mtime);
    HU_ASSERT_EQ(last, (int64_t)(1000 + 60001));

    vm_fixture_teardown(&f);
}

static void test_vm_flush_minute_ticks_with_short_gap_never_skip(void) {
    vm_fixture_t f;
    vm_fixture_setup(&f);
    int64_t last = 0;
    const int64_t ticks[] = {5000, 5000 + 59990, 5000 + 59990 + 59995,
                             5000 + 59990 + 59995 + 60003};
    for (size_t i = 0; i < sizeof(ticks) / sizeof(ticks[0]); i++) {
        if (i > 0)
            HU_ASSERT_EQ(unlink(f.path), 0);
        HU_ASSERT_TRUE(hu_daemon_verifier_metrics_flush(&vm_sample, ticks[i], &last));
        HU_ASSERT(file_size(f.path) > 0);
        HU_ASSERT_EQ(last, ticks[i]);
    }
    vm_fixture_teardown(&f);
}

static void test_vm_flush_null_inputs_are_noops(void) {
    vm_fixture_t f;
    vm_fixture_setup(&f);
    int64_t last = 0;
    HU_ASSERT_FALSE(hu_daemon_verifier_metrics_flush(NULL, 1000, &last));
    HU_ASSERT_EQ(last, (int64_t)0);
    HU_ASSERT_FALSE(hu_daemon_verifier_metrics_flush(&vm_sample, 1000, NULL));
    HU_ASSERT_EQ(file_size(f.path), -1L);
    vm_fixture_teardown(&f);
}

/* ── heartbeat engine ─────────────────────────────────────────────────────
 * Contract for hu_daemon_heartbeat_flush — the gate that wires the
 * previously-inert config->heartbeat.{enabled,interval_minutes} into
 * src/observability/heartbeat.c's hu_heartbeat_ensure_file/hu_heartbeat_tick.
 * The production tick that calls this is compiled out under HU_IS_TEST (same
 * as the flush gates above), so the gate is extracted and pinned here.
 *
 *   (a) disabled (or interval_ms<=0) creates no file at all
 *   (b) the first call always ticks and creates HEARTBEAT.md
 *   (c) a call inside the configured interval does not tick again
 *   (d) two ticks whose wall-clock times differ advance the file's mtime —
 *       hu_heartbeat_tick only reads the file, so this pins the explicit
 *       utime() touch that makes the file usable as a liveness signal
 *   (e) NULL alloc / workspace_dir / last_tick_ms are no-ops
 */

typedef struct hb_fixture {
    char dir[256];
    char path[320]; /* HEARTBEAT.md */
    hu_allocator_t alloc;
} hb_fixture_t;

static void hb_fixture_setup(hb_fixture_t *f) {
    memset(f, 0, sizeof(*f));
    HU_ASSERT_TRUE(hu_test_mkdtemp("/tmp/hu_daemon_hb_flush_", f->dir, sizeof(f->dir)));
    snprintf(f->path, sizeof(f->path), "%s/HEARTBEAT.md", f->dir);
    f->alloc = hu_system_allocator();
}

static void hb_fixture_teardown(hb_fixture_t *f) {
    hu_test_rm_rf(f->dir);
}

static time_t hb_file_mtime(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : (time_t)-1;
}

static void test_hb_flush_disabled_creates_no_file(void) {
    hb_fixture_t f;
    hb_fixture_setup(&f);
    int64_t last = 0;

    HU_ASSERT_FALSE(hu_daemon_heartbeat_flush(&f.alloc, false, 60000, f.dir, 1000, 1000000, &last));
    HU_ASSERT_EQ(file_size(f.path), -1L);
    HU_ASSERT_EQ(last, (int64_t)0);

    /* interval_ms<=0 is equally inert, even when enabled. */
    HU_ASSERT_FALSE(hu_daemon_heartbeat_flush(&f.alloc, true, 0, f.dir, 1000, 1000000, &last));
    HU_ASSERT_EQ(file_size(f.path), -1L);

    hb_fixture_teardown(&f);
}

static void test_hb_flush_first_tick_creates_file(void) {
    hb_fixture_t f;
    hb_fixture_setup(&f);
    int64_t last = 0;

    HU_ASSERT_EQ(file_size(f.path), -1L); /* precondition: nothing on disk */
    HU_ASSERT_TRUE(hu_daemon_heartbeat_flush(&f.alloc, true, 60000, f.dir, 1000, 1000000, &last));
    HU_ASSERT(file_size(f.path) > 0);
    HU_ASSERT_EQ(last, (int64_t)1000);

    hb_fixture_teardown(&f);
}

static void test_hb_flush_skips_inside_interval(void) {
    hb_fixture_t f;
    hb_fixture_setup(&f);
    int64_t last = 0;
    HU_ASSERT_TRUE(hu_daemon_heartbeat_flush(&f.alloc, true, 60000, f.dir, 1000, 1000000, &last));

    HU_ASSERT_FALSE(
        hu_daemon_heartbeat_flush(&f.alloc, true, 60000, f.dir, 1000 + 59999, 2000000, &last));
    HU_ASSERT_EQ(last, (int64_t)1000); /* state untouched when not due */

    hb_fixture_teardown(&f);
}

static void test_hb_flush_two_ticks_advance_file_mtime(void) {
    hb_fixture_t f;
    hb_fixture_setup(&f);
    int64_t last = 0;

    /* interval_ms=1: any positive monotonic gap is due. */
    HU_ASSERT_TRUE(hu_daemon_heartbeat_flush(&f.alloc, true, 1, f.dir, 1000, 1000000, &last));
    time_t first_mtime = hb_file_mtime(f.path);
    HU_ASSERT(first_mtime > 0);

    HU_ASSERT_TRUE(hu_daemon_heartbeat_flush(&f.alloc, true, 1, f.dir, 1001, 2000000, &last));
    time_t second_mtime = hb_file_mtime(f.path);
    HU_ASSERT(second_mtime > first_mtime);
    HU_ASSERT_EQ(last, (int64_t)1001);

    hb_fixture_teardown(&f);
}

static void test_hb_flush_null_inputs_are_noops(void) {
    hb_fixture_t f;
    hb_fixture_setup(&f);
    int64_t last = 0;

    HU_ASSERT_FALSE(hu_daemon_heartbeat_flush(NULL, true, 1, f.dir, 1000, 1000000, &last));
    HU_ASSERT_EQ(last, (int64_t)0);
    HU_ASSERT_FALSE(hu_daemon_heartbeat_flush(&f.alloc, true, 1, NULL, 1000, 1000000, &last));
    HU_ASSERT_FALSE(hu_daemon_heartbeat_flush(&f.alloc, true, 1, f.dir, 1000, 1000000, NULL));
    HU_ASSERT_EQ(file_size(f.path), -1L);

    hb_fixture_teardown(&f);
}

void run_daemon_maintenance_tests(void);
void run_daemon_maintenance_tests(void) {
    HU_TEST_SUITE("daemon-maintenance");
    HU_RUN_TEST(test_pb_flush_first_tick_writes_snapshot);
    HU_RUN_TEST(test_pb_flush_skips_inside_min_gap);
    HU_RUN_TEST(test_pb_flush_rewrites_after_60s_tick_with_zero_turns);
    HU_RUN_TEST(test_pb_flush_minute_ticks_with_short_gap_never_skip);
    HU_RUN_TEST(test_pb_flush_null_inputs_are_noops);
    HU_RUN_TEST(test_vm_flush_first_tick_writes_file);
    HU_RUN_TEST(test_vm_flush_skips_inside_min_gap);
    HU_RUN_TEST(test_vm_flush_rewrites_after_60s_tick_with_zero_turns);
    HU_RUN_TEST(test_vm_flush_minute_ticks_with_short_gap_never_skip);
    HU_RUN_TEST(test_vm_flush_null_inputs_are_noops);
    HU_RUN_TEST(test_hb_flush_disabled_creates_no_file);
    HU_RUN_TEST(test_hb_flush_first_tick_creates_file);
    HU_RUN_TEST(test_hb_flush_skips_inside_interval);
    HU_RUN_TEST(test_hb_flush_two_ticks_advance_file_mtime);
    HU_RUN_TEST(test_hb_flush_null_inputs_are_noops);
}
