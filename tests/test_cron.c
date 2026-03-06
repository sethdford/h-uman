#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "seaclaw/cron.h"
#include "seaclaw/crontab.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>
#include <unistd.h>

static void test_cron_create_destroy(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    SC_ASSERT_NOT_NULL(s);
    sc_cron_destroy(s, &alloc);
}

static void test_cron_add_job(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    SC_ASSERT_NOT_NULL(s);

    uint64_t id = 0;
    sc_error_t err = sc_cron_add_job(s, &alloc, "*/5 * * * *", "echo hello", "test-job", &id);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(id > 0);

    sc_cron_destroy(s, &alloc);
}

static void test_cron_list_jobs(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    SC_ASSERT_NOT_NULL(s);

    uint64_t id1 = 0, id2 = 0;
    sc_cron_add_job(s, &alloc, "*/5 * * * *", "echo a", "job1", &id1);
    sc_cron_add_job(s, &alloc, "0 * * * *", "echo b", NULL, &id2);

    size_t count = 0;
    const sc_cron_job_t *jobs = sc_cron_list_jobs(s, &count);
    SC_ASSERT_EQ(count, 2u);
    SC_ASSERT_NOT_NULL(jobs);
    SC_ASSERT_STR_EQ(jobs[0].expression, "*/5 * * * *");
    SC_ASSERT_STR_EQ(jobs[0].command, "echo a");
    SC_ASSERT_STR_EQ(jobs[0].name, "job1");
    SC_ASSERT_STR_EQ(jobs[1].expression, "0 * * * *");
    SC_ASSERT_STR_EQ(jobs[1].command, "echo b");
    SC_ASSERT_NULL(jobs[1].name);

    sc_cron_destroy(s, &alloc);
}

static void test_cron_list_jobs_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    SC_ASSERT_NOT_NULL(s);

    size_t count = 99;
    (void)sc_cron_list_jobs(s, &count);
    SC_ASSERT_EQ(count, 0u);

    sc_cron_destroy(s, &alloc);
}

static void test_cron_get_job(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    SC_ASSERT_NOT_NULL(s);

    uint64_t id = 0;
    sc_cron_add_job(s, &alloc, "0 0 * * *", "backup", "daily", &id);
    const sc_cron_job_t *job = sc_cron_get_job(s, id);
    SC_ASSERT_NOT_NULL(job);
    SC_ASSERT_EQ(job->id, id);
    SC_ASSERT_STR_EQ(job->command, "backup");
    SC_ASSERT_STR_EQ(job->name, "daily");

    const sc_cron_job_t *missing = sc_cron_get_job(s, 99999);
    SC_ASSERT_NULL(missing);

    sc_cron_destroy(s, &alloc);
}

static void test_cron_remove_job(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    SC_ASSERT_NOT_NULL(s);

    uint64_t id = 0;
    sc_cron_add_job(s, &alloc, "* * * * *", "x", NULL, &id);
    SC_ASSERT_EQ(id, 1u);

    sc_error_t err = sc_cron_remove_job(s, id);
    SC_ASSERT_EQ(err, SC_OK);

    size_t count = 0;
    sc_cron_list_jobs(s, &count);
    SC_ASSERT_EQ(count, 0u);

    sc_error_t err2 = sc_cron_remove_job(s, 42);
    SC_ASSERT_EQ(err2, SC_ERR_NOT_FOUND);

    sc_cron_destroy(s, &alloc);
}

static void test_cron_update_job(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    SC_ASSERT_NOT_NULL(s);

    uint64_t id = 0;
    sc_cron_add_job(s, &alloc, "* * * * *", "old_cmd", "old_name", &id);

    bool disabled = false;
    sc_error_t err = sc_cron_update_job(s, &alloc, id, "*/10 * * * *", "new_cmd", &disabled);
    SC_ASSERT_EQ(err, SC_OK);

    const sc_cron_job_t *job = sc_cron_get_job(s, id);
    SC_ASSERT_NOT_NULL(job);
    SC_ASSERT_STR_EQ(job->expression, "*/10 * * * *");
    SC_ASSERT_STR_EQ(job->command, "new_cmd");

    bool enabled = false;
    sc_cron_update_job(s, &alloc, id, NULL, NULL, &enabled);
    job = sc_cron_get_job(s, id);
    SC_ASSERT_FALSE(job->enabled);

    sc_cron_destroy(s, &alloc);
}

static void test_cron_add_run(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    SC_ASSERT_NOT_NULL(s);

    uint64_t job_id = 0;
    sc_cron_add_job(s, &alloc, "* * * * *", "run_me", NULL, &job_id);

    int64_t now = (int64_t)time(NULL);
    sc_error_t err = sc_cron_add_run(s, &alloc, job_id, now, "executed", "output line");
    SC_ASSERT_EQ(err, SC_OK);

    size_t count = 0;
    const sc_cron_run_t *runs = sc_cron_list_runs(s, job_id, 10, &count);
    SC_ASSERT_EQ(count, 1u);
    SC_ASSERT_NOT_NULL(runs);
    SC_ASSERT_EQ(runs[0].job_id, job_id);
    SC_ASSERT_STR_EQ(runs[0].status, "executed");
    SC_ASSERT_STR_EQ(runs[0].output, "output line");

    sc_cron_destroy(s, &alloc);
}

static void test_cron_list_runs_limit(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    SC_ASSERT_NOT_NULL(s);

    uint64_t job_id = 0;
    sc_cron_add_job(s, &alloc, "* * * * *", "x", NULL, &job_id);

    int64_t t = (int64_t)time(NULL);
    for (int i = 0; i < 5; i++)
        sc_cron_add_run(s, &alloc, job_id, t + i, "ok", NULL);

    size_t count = 0;
    const sc_cron_run_t *runs = sc_cron_list_runs(s, job_id, 2, &count);
    SC_ASSERT_EQ(count, 2u);
    SC_ASSERT_NOT_NULL(runs);

    sc_cron_destroy(s, &alloc);
}

static void test_cron_list_runs_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    SC_ASSERT_NOT_NULL(s);

    uint64_t job_id = 0;
    sc_cron_add_job(s, &alloc, "* * * * *", "x", NULL, &job_id);

    size_t count = 99;
    const sc_cron_run_t *runs = sc_cron_list_runs(s, job_id, 10, &count);
    SC_ASSERT_EQ(count, 0u);
    SC_ASSERT_NULL(runs);

    sc_cron_destroy(s, &alloc);
}

static void test_cron_add_job_default_expression(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    SC_ASSERT_NOT_NULL(s);

    uint64_t id = 0;
    sc_error_t err = sc_cron_add_job(s, &alloc, NULL, "cmd_only", NULL, &id);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(id > 0);

    const sc_cron_job_t *job = sc_cron_get_job(s, id);
    SC_ASSERT_NOT_NULL(job);
    SC_ASSERT_STR_EQ(job->expression, "* * * * *");
    SC_ASSERT_STR_EQ(job->command, "cmd_only");

    sc_cron_destroy(s, &alloc);
}

static void test_cron_add_job_invalid_args(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    SC_ASSERT_NOT_NULL(s);

    uint64_t id = 0;
    sc_error_t err = sc_cron_add_job(s, &alloc, "x", "cmd", NULL, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);

    err = sc_cron_add_job(NULL, &alloc, "* * * * *", "cmd", NULL, &id);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);

    err = sc_cron_add_job(s, &alloc, "* * * * *", NULL, NULL, &id);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);

    sc_cron_destroy(s, &alloc);
}

static void test_cron_create_null_alloc(void) {
    sc_cron_scheduler_t *s = sc_cron_create(NULL, 100, true);
    SC_ASSERT_NULL(s);
}

static void test_cron_add_job_every_minute(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    sc_error_t err = sc_cron_add_job(s, &alloc, "* * * * *", "cmd", "every_min", &id);
    SC_ASSERT_EQ(err, SC_OK);
    const sc_cron_job_t *job = sc_cron_get_job(s, id);
    SC_ASSERT_STR_EQ(job->expression, "* * * * *");
    sc_cron_destroy(s, &alloc);
}

static void test_cron_add_job_hourly(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    sc_cron_add_job(s, &alloc, "0 * * * *", "hourly_cmd", NULL, &id);
    const sc_cron_job_t *job = sc_cron_get_job(s, id);
    SC_ASSERT_STR_EQ(job->expression, "0 * * * *");
    sc_cron_destroy(s, &alloc);
}

static void test_cron_add_job_daily(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    sc_cron_add_job(s, &alloc, "0 0 * * *", "daily", "daily_job", &id);
    const sc_cron_job_t *job = sc_cron_get_job(s, id);
    SC_ASSERT_STR_EQ(job->expression, "0 0 * * *");
    SC_ASSERT_STR_EQ(job->name, "daily_job");
    sc_cron_destroy(s, &alloc);
}

static void test_cron_add_job_weekly(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    sc_cron_add_job(s, &alloc, "0 0 * * 0", "weekly", NULL, &id);
    const sc_cron_job_t *job = sc_cron_get_job(s, id);
    SC_ASSERT_STR_EQ(job->expression, "0 0 * * 0");
    sc_cron_destroy(s, &alloc);
}

static void test_cron_add_job_monthly(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    sc_cron_add_job(s, &alloc, "0 0 1 * *", "monthly", "m", &id);
    const sc_cron_job_t *job = sc_cron_get_job(s, id);
    SC_ASSERT_STR_EQ(job->expression, "0 0 1 * *");
    sc_cron_destroy(s, &alloc);
}

static void test_cron_add_job_expression_stored(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    sc_cron_add_job(s, &alloc, "*/5 12 * * 1-5", "work", NULL, &id);
    const sc_cron_job_t *job = sc_cron_get_job(s, id);
    SC_ASSERT_STR_EQ(job->expression, "*/5 12 * * 1-5");
    sc_cron_destroy(s, &alloc);
}

static void test_cron_max_tasks_limit(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 3, true);
    uint64_t id1 = 0, id2 = 0, id3 = 0, id4 = 0;
    SC_ASSERT_EQ(sc_cron_add_job(s, &alloc, "* * * * *", "a", NULL, &id1), SC_OK);
    SC_ASSERT_EQ(sc_cron_add_job(s, &alloc, "* * * * *", "b", NULL, &id2), SC_OK);
    SC_ASSERT_EQ(sc_cron_add_job(s, &alloc, "* * * * *", "c", NULL, &id3), SC_OK);
    sc_error_t err = sc_cron_add_job(s, &alloc, "* * * * *", "d", NULL, &id4);
    SC_ASSERT_EQ(err, SC_ERR_OUT_OF_MEMORY);
    sc_cron_destroy(s, &alloc);
}

static void test_cron_remove_nonexistent(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    sc_error_t err = sc_cron_remove_job(s, 99999);
    SC_ASSERT_EQ(err, SC_ERR_NOT_FOUND);
    sc_cron_destroy(s, &alloc);
}

static void test_cron_update_expression_only(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    sc_cron_add_job(s, &alloc, "* * * * *", "orig", NULL, &id);
    bool en = true;
    sc_cron_update_job(s, &alloc, id, "0 */2 * * *", "orig", &en);
    const sc_cron_job_t *job = sc_cron_get_job(s, id);
    SC_ASSERT_STR_EQ(job->expression, "0 */2 * * *");
    SC_ASSERT_STR_EQ(job->command, "orig");
    sc_cron_destroy(s, &alloc);
}

static void test_cron_job_created_at_set(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    sc_cron_add_job(s, &alloc, "* * * * *", "x", NULL, &id);
    const sc_cron_job_t *job = sc_cron_get_job(s, id);
    SC_ASSERT(job->created_at_s > 0);
    sc_cron_destroy(s, &alloc);
}

static void test_cron_list_runs_returns_newest_first(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    uint64_t job_id = 0;
    sc_cron_add_job(s, &alloc, "* * * * *", "x", NULL, &job_id);
    int64_t t = (int64_t)time(NULL);
    sc_cron_add_run(s, &alloc, job_id, t, "ok", "first");
    sc_cron_add_run(s, &alloc, job_id, t + 1, "ok", "second");
    size_t count = 0;
    const sc_cron_run_t *runs = sc_cron_list_runs(s, job_id, 10, &count);
    SC_ASSERT_EQ(count, 2u);
    SC_ASSERT_NOT_NULL(runs);
    sc_cron_destroy(s, &alloc);
}

static void test_cron_add_run_null_status_uses_default(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    uint64_t job_id = 0;
    sc_cron_add_job(s, &alloc, "* * * * *", "x", NULL, &job_id);
    sc_error_t err = sc_cron_add_run(s, &alloc, job_id, (int64_t)time(NULL), NULL, NULL);
    SC_ASSERT_EQ(err, SC_OK);
    size_t count = 0;
    const sc_cron_run_t *runs = sc_cron_list_runs(s, job_id, 10, &count);
    SC_ASSERT_EQ(count, 1u);
    SC_ASSERT_STR_EQ(runs[0].status, "executed");
    sc_cron_destroy(s, &alloc);
}

/* ─── Expression patterns ─────────────────────────────────────────────────── */
static void test_cron_add_job_every_five_minutes(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    sc_cron_add_job(s, &alloc, "*/5 * * * *", "every_five", NULL, &id);
    const sc_cron_job_t *job = sc_cron_get_job(s, id);
    SC_ASSERT_STR_EQ(job->expression, "*/5 * * * *");
    sc_cron_destroy(s, &alloc);
}

static void test_cron_add_job_weekdays_at_nine(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    sc_cron_add_job(s, &alloc, "0 9 * * 1-5", "weekday_morning", NULL, &id);
    const sc_cron_job_t *job = sc_cron_get_job(s, id);
    SC_ASSERT_STR_EQ(job->expression, "0 9 * * 1-5");
    sc_cron_destroy(s, &alloc);
}

/* ─── Job enable/disable toggle ───────────────────────────────────────────── */
static void test_cron_update_job_disable(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    sc_cron_add_job(s, &alloc, "* * * * *", "enabled_job", NULL, &id);
    bool off = false;
    sc_error_t err = sc_cron_update_job(s, &alloc, id, NULL, NULL, &off);
    SC_ASSERT_EQ(err, SC_OK);
    const sc_cron_job_t *job = sc_cron_get_job(s, id);
    SC_ASSERT_FALSE(job->enabled);
    sc_cron_destroy(s, &alloc);
}

static void test_cron_update_job_reenable(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    sc_cron_add_job(s, &alloc, "* * * * *", "job", NULL, &id);
    bool off = false;
    sc_cron_update_job(s, &alloc, id, NULL, NULL, &off);
    bool on = true;
    sc_cron_update_job(s, &alloc, id, NULL, NULL, &on);
    const sc_cron_job_t *job = sc_cron_get_job(s, id);
    SC_ASSERT_TRUE(job->enabled);
    sc_cron_destroy(s, &alloc);
}

/* ─── Run history newest first ───────────────────────────────────────────── */
static void test_cron_list_runs_newest_first_order(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    uint64_t job_id = 0;
    sc_cron_add_job(s, &alloc, "* * * * *", "x", NULL, &job_id);
    int64_t t = (int64_t)time(NULL);
    sc_cron_add_run(s, &alloc, job_id, t, "ok", "first");
    sc_cron_add_run(s, &alloc, job_id, t + 1, "ok", "second");
    sc_cron_add_run(s, &alloc, job_id, t + 2, "ok", "third");
    size_t count = 0;
    const sc_cron_run_t *runs = sc_cron_list_runs(s, job_id, 10, &count);
    SC_ASSERT_EQ(count, 3u);
    SC_ASSERT_NOT_NULL(runs);
    SC_ASSERT_STR_EQ(runs[0].output, "third");
    SC_ASSERT_STR_EQ(runs[1].output, "second");
    SC_ASSERT_STR_EQ(runs[2].output, "first");
    sc_cron_destroy(s, &alloc);
}

/* ─── Add run for nonexistent job ────────────────────────────────────────── */
static void test_cron_add_run_nonexistent_job(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    sc_error_t err = sc_cron_add_run(s, &alloc, 99999, (int64_t)time(NULL), "ok", NULL);
    SC_ASSERT_EQ(err, SC_OK);
    size_t count = 99;
    const sc_cron_run_t *runs = sc_cron_list_runs(s, 99999, 10, &count);
    SC_ASSERT_EQ(count, 1u);
    SC_ASSERT_NOT_NULL(runs);
    sc_cron_destroy(s, &alloc);
}

/* ─── Crontab tests ──────────────────────────────────────────────────────── */
static void test_crontab_get_path(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *path = NULL;
    size_t path_len = 0;
    sc_error_t err = sc_crontab_get_path(&alloc, &path, &path_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(path);
    SC_ASSERT(path_len > 0);
    alloc.free(alloc.ctx, path, path_len + 1);
}

static void test_crontab_load_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_crontab_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err = sc_crontab_load(&alloc, "/nonexistent/path/crontab.json", &entries, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 0u);
}

static void test_crontab_save_load_roundtrip(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *tmp_path = NULL;
    size_t tmp_len = 0;
    sc_crontab_get_path(&alloc, &tmp_path, &tmp_len);
    SC_ASSERT_NOT_NULL(tmp_path);
    unlink(tmp_path);

    sc_crontab_entry_t entries[2] = {{0}};
    entries[0].id = sc_strndup(&alloc, "1", 1);
    entries[0].schedule = sc_strndup(&alloc, "0 * * * *", 9);
    entries[0].command = sc_strndup(&alloc, "echo hourly", 11);
    entries[0].enabled = true;
    entries[1].id = sc_strndup(&alloc, "2", 1);
    entries[1].schedule = sc_strndup(&alloc, "*/5 * * * *", 11);
    entries[1].command = sc_strndup(&alloc, "echo every5", 11);
    entries[1].enabled = false;
    SC_ASSERT_NOT_NULL(entries[0].id);

    sc_error_t err = sc_crontab_save(&alloc, tmp_path, entries, 2);
    SC_ASSERT_EQ(err, SC_OK);

    sc_crontab_entry_t *loaded = NULL;
    size_t loaded_count = 0;
    err = sc_crontab_load(&alloc, tmp_path, &loaded, &loaded_count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(loaded_count, 2u);
    SC_ASSERT_STR_EQ(loaded[0].schedule, "0 * * * *");
    SC_ASSERT_STR_EQ(loaded[0].command, "echo hourly");
    SC_ASSERT_TRUE(loaded[0].enabled);
    SC_ASSERT_FALSE(loaded[1].enabled);

    sc_crontab_entries_free(&alloc, loaded, loaded_count);
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
    sc_allocator_t alloc = sc_system_allocator();
    char *path = NULL;
    size_t path_len = 0;
    sc_crontab_get_path(&alloc, &path, &path_len);
    unlink(path);
    /* Ensure no stale file from prior tests */
    FILE *f = fopen(path, "w");
    if (f)
        fclose(f);
    unlink(path);

    char *new_id = NULL;
    sc_error_t err = sc_crontab_add(&alloc, path, "0 9 * * *", 9, "echo morning", 12, &new_id);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(new_id);

    sc_crontab_entry_t *entries = NULL;
    size_t count = 0;
    sc_crontab_load(&alloc, path, &entries, &count);
    SC_ASSERT_EQ(count, 1u);
    SC_ASSERT_STR_EQ(entries[0].command, "echo morning");

    sc_crontab_entries_free(&alloc, entries, count);
    alloc.free(alloc.ctx, new_id, strlen(new_id) + 1);
    alloc.free(alloc.ctx, path, path_len + 1);
}

static void test_crontab_remove(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *path = NULL;
    size_t path_len = 0;
    sc_crontab_get_path(&alloc, &path, &path_len);
    unlink(path);

    char *id1 = NULL, *id2 = NULL;
    sc_crontab_add(&alloc, path, "* * * * *", 9, "cmd1", 4, &id1);
    sc_crontab_add(&alloc, path, "0 * * * *", 9, "cmd2", 4, &id2);

    sc_error_t err = sc_crontab_remove(&alloc, path, id1);
    SC_ASSERT_EQ(err, SC_OK);

    sc_crontab_entry_t *entries = NULL;
    size_t count = 0;
    sc_crontab_load(&alloc, path, &entries, &count);
    SC_ASSERT_EQ(count, 1u);
    SC_ASSERT_STR_EQ(entries[0].command, "cmd2");

    sc_crontab_entries_free(&alloc, entries, count);
    alloc.free(alloc.ctx, id1, strlen(id1) + 1);
    alloc.free(alloc.ctx, id2, strlen(id2) + 1);
    alloc.free(alloc.ctx, path, path_len + 1);
}

static void test_cron_job_paused_field(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    sc_cron_add_job(s, &alloc, "* * * * *", "x", NULL, &id);
    const sc_cron_job_t *job = sc_cron_get_job(s, id);
    SC_ASSERT_FALSE(job->paused);
    sc_cron_destroy(s, &alloc);
}

static void test_cron_job_one_shot_field(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *s = sc_cron_create(&alloc, 100, true);
    uint64_t id = 0;
    sc_cron_add_job(s, &alloc, "* * * * *", "x", NULL, &id);
    const sc_cron_job_t *job = sc_cron_get_job(s, id);
    SC_ASSERT_FALSE(job->one_shot);
    sc_cron_destroy(s, &alloc);
}

static void test_crontab_add_null_path(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *id = NULL;
    sc_error_t err = sc_crontab_add(&alloc, NULL, "* * * * *", 9, "cmd", 3, &id);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_crontab_load_null_args(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_crontab_entry_t *e = NULL;
    size_t c = 0;
    SC_ASSERT_EQ(sc_crontab_load(NULL, "/tmp", &e, &c), SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_EQ(sc_crontab_load(&alloc, NULL, &e, &c), SC_ERR_INVALID_ARGUMENT);
}

void run_cron_tests(void) {
    SC_TEST_SUITE("cron");
    SC_RUN_TEST(test_cron_create_destroy);
    SC_RUN_TEST(test_cron_add_job);
    SC_RUN_TEST(test_cron_list_jobs);
    SC_RUN_TEST(test_cron_list_jobs_empty);
    SC_RUN_TEST(test_cron_get_job);
    SC_RUN_TEST(test_cron_remove_job);
    SC_RUN_TEST(test_cron_update_job);
    SC_RUN_TEST(test_cron_add_run);
    SC_RUN_TEST(test_cron_list_runs_limit);
    SC_RUN_TEST(test_cron_list_runs_empty);
    SC_RUN_TEST(test_cron_add_job_default_expression);
    SC_RUN_TEST(test_cron_add_job_invalid_args);
    SC_RUN_TEST(test_cron_create_null_alloc);
    SC_RUN_TEST(test_cron_add_job_every_minute);
    SC_RUN_TEST(test_cron_add_job_hourly);
    SC_RUN_TEST(test_cron_add_job_daily);
    SC_RUN_TEST(test_cron_add_job_weekly);
    SC_RUN_TEST(test_cron_add_job_monthly);
    SC_RUN_TEST(test_cron_add_job_expression_stored);
    SC_RUN_TEST(test_cron_max_tasks_limit);
    SC_RUN_TEST(test_cron_remove_nonexistent);
    SC_RUN_TEST(test_cron_update_expression_only);
    SC_RUN_TEST(test_cron_job_created_at_set);
    SC_RUN_TEST(test_cron_list_runs_returns_newest_first);
    SC_RUN_TEST(test_cron_add_run_null_status_uses_default);
    SC_RUN_TEST(test_cron_add_job_every_five_minutes);
    SC_RUN_TEST(test_cron_add_job_weekdays_at_nine);
    SC_RUN_TEST(test_cron_update_job_disable);
    SC_RUN_TEST(test_cron_update_job_reenable);
    SC_RUN_TEST(test_cron_list_runs_newest_first_order);
    SC_RUN_TEST(test_cron_add_run_nonexistent_job);
    SC_RUN_TEST(test_crontab_get_path);
    SC_RUN_TEST(test_crontab_load_empty);
    SC_RUN_TEST(test_crontab_save_load_roundtrip);
    SC_RUN_TEST(test_crontab_add);
    SC_RUN_TEST(test_crontab_remove);
    SC_RUN_TEST(test_cron_job_paused_field);
    SC_RUN_TEST(test_cron_job_one_shot_field);
    SC_RUN_TEST(test_crontab_add_null_path);
    SC_RUN_TEST(test_crontab_load_null_args);
}
