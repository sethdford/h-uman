#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/cron.h"
#include "human/crontab.h"
#include "human/platform.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Unique crontab path per test to avoid shared-state flakiness (parallel-safe via pid). */
static hu_error_t get_unique_crontab_path(hu_allocator_t *alloc, char **path, size_t *path_len) {
    static unsigned counter;
    long pid = 0;
#if defined(__unix__) || defined(__APPLE__)
    pid = (long)getpid();
#endif
    char *tmp = hu_platform_get_temp_dir(alloc);
    if (!tmp) {
        const char *t = getenv("TMPDIR");
        if (!t)
            t = getenv("TEMP");
        if (!t)
            t = "/tmp";
        size_t tlen = strlen(t);
        size_t cap = tlen + 80;
        *path = (char *)alloc->alloc(alloc->ctx, cap);
        if (!*path)
            return HU_ERR_OUT_OF_MEMORY;
        int n = snprintf(*path, cap, "%s/human_crontab_test_%ld_%u.json", t, pid, counter++);
        if (n < 0 || (size_t)n >= cap) {
            alloc->free(alloc->ctx, *path, cap);
            *path = NULL;
            return HU_ERR_INTERNAL;
        }
        *path_len = (size_t)n;
        return HU_OK;
    }
    size_t tlen = strlen(tmp);
    size_t cap = tlen + 80;
    *path = (char *)alloc->alloc(alloc->ctx, cap);
    if (!*path) {
        alloc->free(alloc->ctx, tmp, tlen + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(*path, cap, "%s/human_crontab_test_%ld_%u.json", tmp, pid, counter++);
    alloc->free(alloc->ctx, tmp, tlen + 1);
    if (n < 0 || (size_t)n >= cap) {
        alloc->free(alloc->ctx, *path, cap);
        *path = NULL;
        return HU_ERR_INTERNAL;
    }
    *path_len = (size_t)n;
    return HU_OK;
}

static void test_cron_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    HU_ASSERT_NOT_NULL(s);
    hu_cron_destroy(s, &alloc);
}

static void test_cron_add_job(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    HU_ASSERT_NOT_NULL(s);

    uint64_t id = 0;
    hu_error_t err = hu_cron_add_job(s, &alloc, "*/5 * * * *", "echo hello", "test-job", &id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(id > 0);

    hu_cron_destroy(s, &alloc);
}

static void test_cron_list_jobs(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    HU_ASSERT_NOT_NULL(s);

    uint64_t id1 = 0, id2 = 0;
    hu_cron_add_job(s, &alloc, "*/5 * * * *", "echo a", "job1", &id1);
    hu_cron_add_job(s, &alloc, "0 * * * *", "echo b", NULL, &id2);

    size_t count = 0;
    const hu_cron_job_t *jobs = hu_cron_list_jobs(s, &count);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_NOT_NULL(jobs);
    HU_ASSERT_STR_EQ(jobs[0].expression, "*/5 * * * *");
    HU_ASSERT_STR_EQ(jobs[0].command, "echo a");
    HU_ASSERT_STR_EQ(jobs[0].name, "job1");
    HU_ASSERT_STR_EQ(jobs[1].expression, "0 * * * *");
    HU_ASSERT_STR_EQ(jobs[1].command, "echo b");
    HU_ASSERT_NULL(jobs[1].name);

    hu_cron_destroy(s, &alloc);
}

static void test_cron_list_jobs_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    HU_ASSERT_NOT_NULL(s);

    size_t count = 99;
    (void)hu_cron_list_jobs(s, &count);
    HU_ASSERT_EQ(count, 0u);

    hu_cron_destroy(s, &alloc);
}

static void test_cron_get_job(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    HU_ASSERT_NOT_NULL(s);

    uint64_t id = 0;
    hu_cron_add_job(s, &alloc, "0 0 * * *", "backup", "daily", &id);
    const hu_cron_job_t *job = hu_cron_get_job(s, id);
    HU_ASSERT_NOT_NULL(job);
    HU_ASSERT_EQ(job->id, id);
    HU_ASSERT_STR_EQ(job->command, "backup");
    HU_ASSERT_STR_EQ(job->name, "daily");

    const hu_cron_job_t *missing = hu_cron_get_job(s, 99999);
    HU_ASSERT_NULL(missing);

    hu_cron_destroy(s, &alloc);
}

static void test_cron_remove_job(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    HU_ASSERT_NOT_NULL(s);

    uint64_t id = 0;
    hu_cron_add_job(s, &alloc, "* * * * *", "x", NULL, &id);
    HU_ASSERT_EQ(id, 1u);

    hu_error_t err = hu_cron_remove_job(s, id);
    HU_ASSERT_EQ(err, HU_OK);

    size_t count = 0;
    hu_cron_list_jobs(s, &count);
    HU_ASSERT_EQ(count, 0u);

    hu_error_t err2 = hu_cron_remove_job(s, 42);
    HU_ASSERT_EQ(err2, HU_ERR_NOT_FOUND);

    hu_cron_destroy(s, &alloc);
}

static void test_cron_update_job(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    HU_ASSERT_NOT_NULL(s);

    uint64_t id = 0;
    hu_cron_add_job(s, &alloc, "* * * * *", "old_cmd", "old_name", &id);

    bool disabled = false;
    hu_error_t err = hu_cron_update_job(s, &alloc, id, "*/10 * * * *", "new_cmd", &disabled);
    HU_ASSERT_EQ(err, HU_OK);

    const hu_cron_job_t *job = hu_cron_get_job(s, id);
    HU_ASSERT_NOT_NULL(job);
    HU_ASSERT_STR_EQ(job->expression, "*/10 * * * *");
    HU_ASSERT_STR_EQ(job->command, "new_cmd");

    bool enabled = false;
    hu_cron_update_job(s, &alloc, id, NULL, NULL, &enabled);
    job = hu_cron_get_job(s, id);
    HU_ASSERT_FALSE(job->enabled);

    hu_cron_destroy(s, &alloc);
}

static void test_cron_add_run(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    HU_ASSERT_NOT_NULL(s);

    uint64_t job_id = 0;
    hu_cron_add_job(s, &alloc, "* * * * *", "run_me", NULL, &job_id);

    int64_t now = (int64_t)time(NULL);
    hu_error_t err = hu_cron_add_run(s, &alloc, job_id, now, "executed", "output line");
    HU_ASSERT_EQ(err, HU_OK);

    size_t count = 0;
    const hu_cron_run_t *runs = hu_cron_list_runs(s, job_id, 10, &count);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_NOT_NULL(runs);
    HU_ASSERT_EQ(runs[0].job_id, job_id);
    HU_ASSERT_STR_EQ(runs[0].status, "executed");
    HU_ASSERT_STR_EQ(runs[0].output, "output line");

    hu_cron_destroy(s, &alloc);
}

static void test_cron_list_runs_limit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    HU_ASSERT_NOT_NULL(s);

    uint64_t job_id = 0;
    hu_cron_add_job(s, &alloc, "* * * * *", "x", NULL, &job_id);

    int64_t t = (int64_t)time(NULL);
    for (int i = 0; i < 5; i++)
        hu_cron_add_run(s, &alloc, job_id, t + i, "ok", NULL);

    size_t count = 0;
    const hu_cron_run_t *runs = hu_cron_list_runs(s, job_id, 2, &count);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_NOT_NULL(runs);

    hu_cron_destroy(s, &alloc);
}

static void test_cron_list_runs_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    HU_ASSERT_NOT_NULL(s);

    uint64_t job_id = 0;
    hu_cron_add_job(s, &alloc, "* * * * *", "x", NULL, &job_id);

    size_t count = 99;
    const hu_cron_run_t *runs = hu_cron_list_runs(s, job_id, 10, &count);
    HU_ASSERT_EQ(count, 0u);
    HU_ASSERT_NULL(runs);

    hu_cron_destroy(s, &alloc);
}

static void test_cron_add_job_default_expression(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    HU_ASSERT_NOT_NULL(s);

    uint64_t id = 0;
    hu_error_t err = hu_cron_add_job(s, &alloc, NULL, "cmd_only", NULL, &id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(id > 0);

    const hu_cron_job_t *job = hu_cron_get_job(s, id);
    HU_ASSERT_NOT_NULL(job);
    HU_ASSERT_STR_EQ(job->expression, "* * * * *");
    HU_ASSERT_STR_EQ(job->command, "cmd_only");

    hu_cron_destroy(s, &alloc);
}

static void test_cron_add_job_invalid_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    HU_ASSERT_NOT_NULL(s);

    uint64_t id = 0;
    hu_error_t err = hu_cron_add_job(s, &alloc, "x", "cmd", NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    err = hu_cron_add_job(NULL, &alloc, "* * * * *", "cmd", NULL, &id);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    err = hu_cron_add_job(s, &alloc, "* * * * *", NULL, NULL, &id);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_cron_destroy(s, &alloc);
}

static void test_cron_create_null_alloc(void) {
    hu_cron_scheduler_t *s = hu_cron_create(NULL, 100, true);
    HU_ASSERT_NULL(s);
}

static void test_cron_add_job_every_minute(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    hu_error_t err = hu_cron_add_job(s, &alloc, "* * * * *", "cmd", "every_min", &id);
    HU_ASSERT_EQ(err, HU_OK);
    const hu_cron_job_t *job = hu_cron_get_job(s, id);
    HU_ASSERT_STR_EQ(job->expression, "* * * * *");
    hu_cron_destroy(s, &alloc);
}

static void test_cron_add_job_hourly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    hu_cron_add_job(s, &alloc, "0 * * * *", "hourly_cmd", NULL, &id);
    const hu_cron_job_t *job = hu_cron_get_job(s, id);
    HU_ASSERT_STR_EQ(job->expression, "0 * * * *");
    hu_cron_destroy(s, &alloc);
}

static void test_cron_add_job_daily(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    hu_cron_add_job(s, &alloc, "0 0 * * *", "daily", "daily_job", &id);
    const hu_cron_job_t *job = hu_cron_get_job(s, id);
    HU_ASSERT_STR_EQ(job->expression, "0 0 * * *");
    HU_ASSERT_STR_EQ(job->name, "daily_job");
    hu_cron_destroy(s, &alloc);
}

static void test_cron_add_job_weekly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    hu_cron_add_job(s, &alloc, "0 0 * * 0", "weekly", NULL, &id);
    const hu_cron_job_t *job = hu_cron_get_job(s, id);
    HU_ASSERT_STR_EQ(job->expression, "0 0 * * 0");
    hu_cron_destroy(s, &alloc);
}

static void test_cron_add_job_monthly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    hu_cron_add_job(s, &alloc, "0 0 1 * *", "monthly", "m", &id);
    const hu_cron_job_t *job = hu_cron_get_job(s, id);
    HU_ASSERT_STR_EQ(job->expression, "0 0 1 * *");
    hu_cron_destroy(s, &alloc);
}

static void test_cron_add_job_expression_stored(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    hu_cron_add_job(s, &alloc, "*/5 12 * * 1-5", "work", NULL, &id);
    const hu_cron_job_t *job = hu_cron_get_job(s, id);
    HU_ASSERT_STR_EQ(job->expression, "*/5 12 * * 1-5");
    hu_cron_destroy(s, &alloc);
}

static void test_cron_max_tasks_limit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 3, true);
    uint64_t id1 = 0, id2 = 0, id3 = 0, id4 = 0;
    HU_ASSERT_EQ(hu_cron_add_job(s, &alloc, "* * * * *", "a", NULL, &id1), HU_OK);
    HU_ASSERT_EQ(hu_cron_add_job(s, &alloc, "* * * * *", "b", NULL, &id2), HU_OK);
    HU_ASSERT_EQ(hu_cron_add_job(s, &alloc, "* * * * *", "c", NULL, &id3), HU_OK);
    hu_error_t err = hu_cron_add_job(s, &alloc, "* * * * *", "d", NULL, &id4);
    HU_ASSERT_EQ(err, HU_ERR_OUT_OF_MEMORY);
    hu_cron_destroy(s, &alloc);
}

static void test_cron_remove_nonexistent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    hu_error_t err = hu_cron_remove_job(s, 99999);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
    hu_cron_destroy(s, &alloc);
}

static void test_cron_update_expression_only(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    hu_cron_add_job(s, &alloc, "* * * * *", "orig", NULL, &id);
    bool en = true;
    hu_cron_update_job(s, &alloc, id, "0 */2 * * *", "orig", &en);
    const hu_cron_job_t *job = hu_cron_get_job(s, id);
    HU_ASSERT_STR_EQ(job->expression, "0 */2 * * *");
    HU_ASSERT_STR_EQ(job->command, "orig");
    hu_cron_destroy(s, &alloc);
}

static void test_cron_job_created_at_set(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    hu_cron_add_job(s, &alloc, "* * * * *", "x", NULL, &id);
    const hu_cron_job_t *job = hu_cron_get_job(s, id);
    HU_ASSERT(job->created_at_s > 0);
    hu_cron_destroy(s, &alloc);
}

static void test_cron_list_runs_returns_newest_first(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    uint64_t job_id = 0;
    hu_cron_add_job(s, &alloc, "* * * * *", "x", NULL, &job_id);
    int64_t t = (int64_t)time(NULL);
    hu_cron_add_run(s, &alloc, job_id, t, "ok", "first");
    hu_cron_add_run(s, &alloc, job_id, t + 1, "ok", "second");
    size_t count = 0;
    const hu_cron_run_t *runs = hu_cron_list_runs(s, job_id, 10, &count);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_NOT_NULL(runs);
    hu_cron_destroy(s, &alloc);
}

static void test_cron_add_run_null_status_uses_default(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    uint64_t job_id = 0;
    hu_cron_add_job(s, &alloc, "* * * * *", "x", NULL, &job_id);
    hu_error_t err = hu_cron_add_run(s, &alloc, job_id, (int64_t)time(NULL), NULL, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    size_t count = 0;
    const hu_cron_run_t *runs = hu_cron_list_runs(s, job_id, 10, &count);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(runs[0].status, "executed");
    hu_cron_destroy(s, &alloc);
}

/* ─── Expression patterns ─────────────────────────────────────────────────── */
static void test_cron_add_job_every_five_minutes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    hu_cron_add_job(s, &alloc, "*/5 * * * *", "every_five", NULL, &id);
    const hu_cron_job_t *job = hu_cron_get_job(s, id);
    HU_ASSERT_STR_EQ(job->expression, "*/5 * * * *");
    hu_cron_destroy(s, &alloc);
}

static void test_cron_add_job_weekdays_at_nine(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    hu_cron_add_job(s, &alloc, "0 9 * * 1-5", "weekday_morning", NULL, &id);
    const hu_cron_job_t *job = hu_cron_get_job(s, id);
    HU_ASSERT_STR_EQ(job->expression, "0 9 * * 1-5");
    hu_cron_destroy(s, &alloc);
}

/* ─── Job enable/disable toggle ───────────────────────────────────────────── */
static void test_cron_update_job_disable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    hu_cron_add_job(s, &alloc, "* * * * *", "enabled_job", NULL, &id);
    bool off = false;
    hu_error_t err = hu_cron_update_job(s, &alloc, id, NULL, NULL, &off);
    HU_ASSERT_EQ(err, HU_OK);
    const hu_cron_job_t *job = hu_cron_get_job(s, id);
    HU_ASSERT_FALSE(job->enabled);
    hu_cron_destroy(s, &alloc);
}

static void test_cron_update_job_reenable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    hu_cron_add_job(s, &alloc, "* * * * *", "job", NULL, &id);
    bool off = false;
    hu_cron_update_job(s, &alloc, id, NULL, NULL, &off);
    bool on = true;
    hu_cron_update_job(s, &alloc, id, NULL, NULL, &on);
    const hu_cron_job_t *job = hu_cron_get_job(s, id);
    HU_ASSERT_TRUE(job->enabled);
    hu_cron_destroy(s, &alloc);
}

/* ─── Run history newest first ───────────────────────────────────────────── */
static void test_cron_list_runs_newest_first_order(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    uint64_t job_id = 0;
    hu_cron_add_job(s, &alloc, "* * * * *", "x", NULL, &job_id);
    int64_t t = (int64_t)time(NULL);
    hu_cron_add_run(s, &alloc, job_id, t, "ok", "first");
    hu_cron_add_run(s, &alloc, job_id, t + 1, "ok", "second");
    hu_cron_add_run(s, &alloc, job_id, t + 2, "ok", "third");
    size_t count = 0;
    const hu_cron_run_t *runs = hu_cron_list_runs(s, job_id, 10, &count);
    HU_ASSERT_EQ(count, 3u);
    HU_ASSERT_NOT_NULL(runs);
    HU_ASSERT_STR_EQ(runs[0].output, "third");
    HU_ASSERT_STR_EQ(runs[1].output, "second");
    HU_ASSERT_STR_EQ(runs[2].output, "first");
    hu_cron_destroy(s, &alloc);
}

/* ─── Add run for nonexistent job ────────────────────────────────────────── */
static void test_cron_add_run_nonexistent_job(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    hu_error_t err = hu_cron_add_run(s, &alloc, 99999, (int64_t)time(NULL), "ok", NULL);
    HU_ASSERT_EQ(err, HU_OK);
    size_t count = 99;
    const hu_cron_run_t *runs = hu_cron_list_runs(s, 99999, 10, &count);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_NOT_NULL(runs);
    hu_cron_destroy(s, &alloc);
}

/* ─── Crontab tests ──────────────────────────────────────────────────────── */
static void test_crontab_get_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *path = NULL;
    size_t path_len = 0;
    hu_error_t err = hu_crontab_get_path(&alloc, &path, &path_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(path);
    HU_ASSERT(path_len > 0);
    alloc.free(alloc.ctx, path, path_len + 1);
}

static void test_crontab_load_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_crontab_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = hu_crontab_load(&alloc, "/nonexistent/path/crontab.json", &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
    hu_crontab_entries_free(&alloc, entries, count);
}

static void test_crontab_save_load_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *tmp_path = NULL;
    size_t tmp_len = 0;
    HU_ASSERT_EQ(get_unique_crontab_path(&alloc, &tmp_path, &tmp_len), HU_OK);
    HU_ASSERT_NOT_NULL(tmp_path);
    unlink(tmp_path);

    hu_crontab_entry_t entries[2] = {{0}};
    entries[0].id = hu_strndup(&alloc, "1", 1);
    entries[0].schedule = hu_strndup(&alloc, "0 * * * *", 9);
    entries[0].command = hu_strndup(&alloc, "echo hourly", 11);
    entries[0].enabled = true;
    entries[1].id = hu_strndup(&alloc, "2", 1);
    entries[1].schedule = hu_strndup(&alloc, "*/5 * * * *", 11);
    entries[1].command = hu_strndup(&alloc, "echo every5", 11);
    entries[1].enabled = false;
    HU_ASSERT_NOT_NULL(entries[0].id);

    hu_error_t err = hu_crontab_save(&alloc, tmp_path, entries, 2);
    HU_ASSERT_EQ(err, HU_OK);

    hu_crontab_entry_t *loaded = NULL;
    size_t loaded_count = 0;
    err = hu_crontab_load(&alloc, tmp_path, &loaded, &loaded_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(loaded_count, 2u);
    HU_ASSERT_STR_EQ(loaded[0].schedule, "0 * * * *");
    HU_ASSERT_STR_EQ(loaded[0].command, "echo hourly");
    HU_ASSERT_TRUE(loaded[0].enabled);
    HU_ASSERT_FALSE(loaded[1].enabled);

    hu_crontab_entries_free(&alloc, loaded, loaded_count);
    for (int i = 0; i < 2; i++) {
        if (entries[i].id)
            alloc.free(alloc.ctx, entries[i].id, strlen(entries[i].id) + 1);
        if (entries[i].schedule)
            alloc.free(alloc.ctx, entries[i].schedule, strlen(entries[i].schedule) + 1);
        if (entries[i].command)
            alloc.free(alloc.ctx, entries[i].command, strlen(entries[i].command) + 1);
    }
    alloc.free(alloc.ctx, tmp_path, tmp_len + 1);
}

static void test_crontab_add(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *path = NULL;
    size_t path_len = 0;
    HU_ASSERT_EQ(get_unique_crontab_path(&alloc, &path, &path_len), HU_OK);
    unlink(path);

    char *new_id = NULL;
    hu_error_t err = hu_crontab_add(&alloc, path, "0 9 * * *", 9, "echo morning", 12, &new_id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(new_id);

    hu_crontab_entry_t *entries = NULL;
    size_t count = 0;
    hu_crontab_load(&alloc, path, &entries, &count);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(entries[0].command, "echo morning");

    hu_crontab_entries_free(&alloc, entries, count);
    alloc.free(alloc.ctx, new_id, strlen(new_id) + 1);
    alloc.free(alloc.ctx, path, path_len + 1);
}

static void test_crontab_remove(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *path = NULL;
    size_t path_len = 0;
    HU_ASSERT_EQ(get_unique_crontab_path(&alloc, &path, &path_len), HU_OK);
    unlink(path);

    char *id1 = NULL, *id2 = NULL;
    hu_crontab_add(&alloc, path, "* * * * *", 9, "cmd1", 4, &id1);
    hu_crontab_add(&alloc, path, "0 * * * *", 9, "cmd2", 4, &id2);

    hu_error_t err = hu_crontab_remove(&alloc, path, id1);
    HU_ASSERT_EQ(err, HU_OK);

    hu_crontab_entry_t *entries = NULL;
    size_t count = 0;
    hu_crontab_load(&alloc, path, &entries, &count);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(entries[0].command, "cmd2");

    hu_crontab_entries_free(&alloc, entries, count);
    alloc.free(alloc.ctx, id1, strlen(id1) + 1);
    alloc.free(alloc.ctx, id2, strlen(id2) + 1);
    alloc.free(alloc.ctx, path, path_len + 1);
}

static void test_cron_job_paused_field(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    hu_cron_add_job(s, &alloc, "* * * * *", "x", NULL, &id);
    const hu_cron_job_t *job = hu_cron_get_job(s, id);
    HU_ASSERT_FALSE(job->paused);
    hu_cron_destroy(s, &alloc);
}

static void test_cron_job_one_shot_field(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    hu_cron_add_job(s, &alloc, "* * * * *", "x", NULL, &id);
    const hu_cron_job_t *job = hu_cron_get_job(s, id);
    HU_ASSERT_FALSE(job->one_shot);
    hu_cron_destroy(s, &alloc);
}

static void test_cron_set_job_one_shot(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *s = hu_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    hu_cron_add_job(s, &alloc, "* * * * *", "x", NULL, &id);
    HU_ASSERT_EQ(hu_cron_set_job_one_shot(s, id, true), HU_OK);
    const hu_cron_job_t *job = hu_cron_get_job(s, id);
    HU_ASSERT_TRUE(job->one_shot);
    HU_ASSERT_EQ(hu_cron_set_job_one_shot(s, id, false), HU_OK);
    job = hu_cron_get_job(s, id);
    HU_ASSERT_FALSE(job->one_shot);
    HU_ASSERT_EQ(hu_cron_set_job_one_shot(s, 99999, true), HU_ERR_NOT_FOUND);
    hu_cron_destroy(s, &alloc);
}

static void test_crontab_add_null_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *id = NULL;
    hu_error_t err = hu_crontab_add(&alloc, NULL, "* * * * *", 9, "cmd", 3, &id);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_crontab_load_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_crontab_entry_t *e = NULL;
    size_t c = 0;
    HU_ASSERT_EQ(hu_crontab_load(NULL, "/tmp", &e, &c), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_crontab_load(&alloc, NULL, &e, &c), HU_ERR_INVALID_ARGUMENT);
}

void run_cron_tests(void) {
    HU_TEST_SUITE("cron");
    HU_RUN_TEST(test_cron_create_destroy);
    HU_RUN_TEST(test_cron_add_job);
    HU_RUN_TEST(test_cron_list_jobs);
    HU_RUN_TEST(test_cron_list_jobs_empty);
    HU_RUN_TEST(test_cron_get_job);
    HU_RUN_TEST(test_cron_remove_job);
    HU_RUN_TEST(test_cron_update_job);
    HU_RUN_TEST(test_cron_add_run);
    HU_RUN_TEST(test_cron_list_runs_limit);
    HU_RUN_TEST(test_cron_list_runs_empty);
    HU_RUN_TEST(test_cron_add_job_default_expression);
    HU_RUN_TEST(test_cron_add_job_invalid_args);
    HU_RUN_TEST(test_cron_create_null_alloc);
    HU_RUN_TEST(test_cron_add_job_every_minute);
    HU_RUN_TEST(test_cron_add_job_hourly);
    HU_RUN_TEST(test_cron_add_job_daily);
    HU_RUN_TEST(test_cron_add_job_weekly);
    HU_RUN_TEST(test_cron_add_job_monthly);
    HU_RUN_TEST(test_cron_add_job_expression_stored);
    HU_RUN_TEST(test_cron_max_tasks_limit);
    HU_RUN_TEST(test_cron_remove_nonexistent);
    HU_RUN_TEST(test_cron_update_expression_only);
    HU_RUN_TEST(test_cron_job_created_at_set);
    HU_RUN_TEST(test_cron_list_runs_returns_newest_first);
    HU_RUN_TEST(test_cron_add_run_null_status_uses_default);
    HU_RUN_TEST(test_cron_add_job_every_five_minutes);
    HU_RUN_TEST(test_cron_add_job_weekdays_at_nine);
    HU_RUN_TEST(test_cron_update_job_disable);
    HU_RUN_TEST(test_cron_update_job_reenable);
    HU_RUN_TEST(test_cron_list_runs_newest_first_order);
    HU_RUN_TEST(test_cron_add_run_nonexistent_job);
    HU_RUN_TEST(test_crontab_get_path);
    HU_RUN_TEST(test_crontab_load_empty);
    HU_RUN_TEST(test_crontab_save_load_roundtrip);
    HU_RUN_TEST(test_crontab_add);
    HU_RUN_TEST(test_crontab_remove);
    HU_RUN_TEST(test_cron_job_paused_field);
    HU_RUN_TEST(test_cron_job_one_shot_field);
    HU_RUN_TEST(test_cron_set_job_one_shot);
    HU_RUN_TEST(test_crontab_add_null_path);
    HU_RUN_TEST(test_crontab_load_null_args);
}
