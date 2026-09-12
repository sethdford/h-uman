/* Contract tests for hu_daemon_prompt_budget_flush — the cadence gate the
 * once-per-minute maintenance tick (src/daemon/daemon_maintenance.c) uses to
 * persist ~/.human/prompt_budget.snapshot.json for the doctor CLI.
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
 *   (b) a call inside HU_DAEMON_PB_FLUSH_MIN_GAP_MS does not flush
 *   (c) a >60 s tick with ZERO new observations rewrites the file (mtime
 *       advances, content is regenerated)
 *   (d) minute ticks whose monotonic gap is a few ms short of 60 s still
 *       flush — no alternate-minute skipping
 *   (e) NULL budget / NULL state are no-ops
 */

#include "human/agent/prompt_budget.h"
#include "human/core/allocator.h"
#include "human/daemon_maintenance.h"
#include "test_framework.h"
#include "test_tmpdir.h"

#include <stdio.h>
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

    int64_t inside = 1000 + HU_DAEMON_PB_FLUSH_MIN_GAP_MS - 1;
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

void run_daemon_maintenance_tests(void);
void run_daemon_maintenance_tests(void) {
    HU_TEST_SUITE("daemon-maintenance");
    HU_RUN_TEST(test_pb_flush_first_tick_writes_snapshot);
    HU_RUN_TEST(test_pb_flush_skips_inside_min_gap);
    HU_RUN_TEST(test_pb_flush_rewrites_after_60s_tick_with_zero_turns);
    HU_RUN_TEST(test_pb_flush_minute_ticks_with_short_gap_never_skip);
    HU_RUN_TEST(test_pb_flush_null_inputs_are_noops);
}
