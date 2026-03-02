#include "seaclaw/cron.h"
#include <stdint.h>
#include "seaclaw/core/string.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct sc_cron_scheduler {
    sc_cron_job_t *jobs;
    size_t jobs_len;
    size_t jobs_cap;
    sc_cron_run_t *runs;
    size_t runs_len;
    size_t runs_cap;
    uint64_t next_job_id;
    uint64_t next_run_id;
    size_t max_tasks;
    bool enabled;
    sc_allocator_t *alloc;
    /* Scratch buffer for list_runs result (no alloc in API) */
    sc_cron_run_t *scratch_runs;
    size_t scratch_cap;
};

static void free_job(sc_allocator_t *alloc, sc_cron_job_t *job) {
    if (!job || !alloc) return;
    if (job->expression) sc_str_free(alloc, job->expression);
    if (job->command) sc_str_free(alloc, job->command);
    if (job->name) sc_str_free(alloc, job->name);
    if (job->last_status) sc_str_free(alloc, job->last_status);
    memset(job, 0, sizeof(*job));
}

static void free_run(sc_allocator_t *alloc, sc_cron_run_t *run) {
    if (!run || !alloc) return;
    if (run->status) sc_str_free(alloc, run->status);
    if (run->output) sc_str_free(alloc, run->output);
    memset(run, 0, sizeof(*run));
}

sc_cron_scheduler_t *sc_cron_create(sc_allocator_t *alloc, size_t max_tasks, bool enabled) {
    if (!alloc) return NULL;
    sc_cron_scheduler_t *s = (sc_cron_scheduler_t *)alloc->alloc(alloc->ctx, sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->alloc = alloc;
    s->max_tasks = max_tasks > 0 ? max_tasks : 64;
    s->enabled = enabled;
    s->next_job_id = 1;
    s->next_run_id = 1;
    return s;
}

void sc_cron_destroy(sc_cron_scheduler_t *sched, sc_allocator_t *alloc) {
    if (!sched || !alloc) return;
    for (size_t i = 0; i < sched->jobs_len; i++)
        free_job(alloc, &sched->jobs[i]);
    if (sched->jobs)
        alloc->free(alloc->ctx, sched->jobs, sched->jobs_cap * sizeof(sc_cron_job_t));
    for (size_t i = 0; i < sched->runs_len; i++)
        free_run(alloc, &sched->runs[i]);
    if (sched->runs)
        alloc->free(alloc->ctx, sched->runs, sched->runs_cap * sizeof(sc_cron_run_t));
    if (sched->scratch_runs)
        alloc->free(alloc->ctx, sched->scratch_runs, sched->scratch_cap * sizeof(sc_cron_run_t));
    alloc->free(alloc->ctx, sched, sizeof(*sched));
}

sc_error_t sc_cron_add_job(sc_cron_scheduler_t *sched, sc_allocator_t *alloc,
    const char *expression, const char *command, const char *name,
    uint64_t *out_id)
{
    if (!sched || !alloc || !command || !out_id) return SC_ERR_INVALID_ARGUMENT;
    if (sched->jobs_len >= sched->max_tasks) return SC_ERR_OUT_OF_MEMORY;

    /* Ensure capacity */
    if (sched->jobs_len >= sched->jobs_cap) {
        size_t new_cap = sched->jobs_cap == 0 ? 8 : sched->jobs_cap * 2;
        if (new_cap > sched->max_tasks) new_cap = sched->max_tasks;
        sc_cron_job_t *n = (sc_cron_job_t *)alloc->realloc(alloc->ctx,
            sched->jobs, sched->jobs_cap * sizeof(sc_cron_job_t), new_cap * sizeof(sc_cron_job_t));
        if (!n) return SC_ERR_OUT_OF_MEMORY;
        sched->jobs = n;
        sched->jobs_cap = new_cap;
    }

    sc_cron_job_t *job = &sched->jobs[sched->jobs_len];
    memset(job, 0, sizeof(*job));
    job->id = sched->next_job_id++;
    *out_id = job->id;

    job->expression = expression && expression[0]
        ? sc_strndup(alloc, expression, strlen(expression)) : sc_strndup(alloc, "* * * * *", 9);
    job->command = sc_strndup(alloc, command, strlen(command));
    job->name = (name && name[0]) ? sc_strndup(alloc, name, strlen(name)) : NULL;
    if (!job->command || !job->expression) {
        free_job(alloc, job);
        return SC_ERR_OUT_OF_MEMORY;
    }
    job->enabled = true;
    job->paused = false;
    job->one_shot = false;
    job->created_at_s = (int64_t)time(NULL);

    sched->jobs_len++;
    return SC_OK;
}

sc_error_t sc_cron_remove_job(sc_cron_scheduler_t *sched, uint64_t job_id) {
    if (!sched) return SC_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < sched->jobs_len; i++) {
        if (sched->jobs[i].id == job_id) {
            sc_allocator_t *a = sched->alloc;
            free_job(a, &sched->jobs[i]);
            memmove(&sched->jobs[i], &sched->jobs[i + 1],
                (sched->jobs_len - 1 - i) * sizeof(sc_cron_job_t));
            sched->jobs_len--;
            return SC_OK;
        }
    }
    return SC_ERR_NOT_FOUND;
}

sc_error_t sc_cron_update_job(sc_cron_scheduler_t *sched, sc_allocator_t *alloc,
    uint64_t job_id, const char *expression, const char *command, const bool *enabled)
{
    if (!sched || !alloc) return SC_ERR_INVALID_ARGUMENT;
    sc_cron_job_t *job = NULL;
    for (size_t i = 0; i < sched->jobs_len; i++) {
        if (sched->jobs[i].id == job_id) { job = &sched->jobs[i]; break; }
    }
    if (!job) return SC_ERR_NOT_FOUND;

    if (expression && expression[0]) {
        char *n = sc_strndup(alloc, expression, strlen(expression));
        if (!n) return SC_ERR_OUT_OF_MEMORY;
        if (job->expression) sc_str_free(alloc, job->expression);
        job->expression = n;
    }
    if (command && command[0]) {
        char *n = sc_strndup(alloc, command, strlen(command));
        if (!n) return SC_ERR_OUT_OF_MEMORY;
        if (job->command) sc_str_free(alloc, job->command);
        job->command = n;
    }
    if (enabled != NULL)
        job->enabled = *enabled;

    return SC_OK;
}

const sc_cron_job_t *sc_cron_get_job(const sc_cron_scheduler_t *sched, uint64_t job_id) {
    if (!sched) return NULL;
    for (size_t i = 0; i < sched->jobs_len; i++) {
        if (sched->jobs[i].id == job_id) return &sched->jobs[i];
    }
    return NULL;
}

const sc_cron_job_t *sc_cron_list_jobs(const sc_cron_scheduler_t *sched, size_t *count) {
    if (!sched || !count) return NULL;
    *count = sched->jobs_len;
    return sched->jobs;
}

sc_error_t sc_cron_add_run(sc_cron_scheduler_t *sched, sc_allocator_t *alloc,
    uint64_t job_id, int64_t started_at, const char *status, const char *output)
{
    if (!sched || !alloc) return SC_ERR_INVALID_ARGUMENT;
    if (sched->runs_len >= sched->max_tasks * 64) return SC_ERR_OUT_OF_MEMORY; /* rough cap */

    if (sched->runs_len >= sched->runs_cap) {
        size_t new_cap = sched->runs_cap == 0 ? 32 : sched->runs_cap * 2;
        sc_cron_run_t *n = (sc_cron_run_t *)alloc->realloc(alloc->ctx,
            sched->runs, sched->runs_cap * sizeof(sc_cron_run_t), new_cap * sizeof(sc_cron_run_t));
        if (!n) return SC_ERR_OUT_OF_MEMORY;
        sched->runs = n;
        sched->runs_cap = new_cap;
    }

    sc_cron_run_t *run = &sched->runs[sched->runs_len];
    memset(run, 0, sizeof(*run));
    run->id = sched->next_run_id++;
    run->job_id = job_id;
    run->started_at_s = started_at;
    run->finished_at_s = started_at;
    run->status = status && status[0]
        ? sc_strndup(alloc, status, strlen(status)) : sc_strndup(alloc, "executed", 8);
    run->output = (output && output[0]) ? sc_strndup(alloc, output, strlen(output)) : NULL;
    if (!run->status) {
        memset(run, 0, sizeof(*run));
        return SC_ERR_OUT_OF_MEMORY;
    }

    sched->runs_len++;
    return SC_OK;
}

const sc_cron_run_t *sc_cron_list_runs(sc_cron_scheduler_t *sched,
    uint64_t job_id, size_t limit, size_t *count)
{
    if (!sched || !count) return NULL;
    *count = 0;

    /* Count matching runs (newest at end) */
    size_t n = 0;
    for (size_t i = sched->runs_len; i > 0; i--) {
        if (sched->runs[i - 1].job_id == job_id) n++;
    }
    if (n > limit) n = limit;
    *count = n;
    if (n == 0) return NULL;

    /* Ensure scratch buffer */
    if (n > sched->scratch_cap) {
        sc_allocator_t *a = sched->alloc;
        size_t new_cap = n;
        sc_cron_run_t *buf = (sc_cron_run_t *)a->alloc(a->ctx, new_cap * sizeof(sc_cron_run_t));
        if (!buf) return NULL;
        if (sched->scratch_runs)
            a->free(a->ctx, sched->scratch_runs, sched->scratch_cap * sizeof(sc_cron_run_t));
        sched->scratch_runs = buf;
        sched->scratch_cap = new_cap;
    }

    /* Fill scratch (most recent first) */
    size_t idx = 0;
    for (size_t i = sched->runs_len; i > 0 && idx < n; i--) {
        if (sched->runs[i - 1].job_id == job_id) {
            sched->scratch_runs[idx++] = sched->runs[i - 1];
        }
    }

    return sched->scratch_runs;
}
