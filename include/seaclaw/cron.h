#ifndef SC_CRON_H
#define SC_CRON_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sc_cron_job {
    uint64_t id;
    char *expression;
    char *command;
    char *name;
    int64_t next_run_secs;
    int64_t last_run_secs;
    char *last_status;
    bool paused;
    bool one_shot;
    bool enabled;
    int64_t created_at_s;
} sc_cron_job_t;

typedef struct sc_cron_run {
    uint64_t id;
    uint64_t job_id;
    int64_t started_at_s;
    int64_t finished_at_s;
    char *status;
    char *output;
} sc_cron_run_t;

typedef struct sc_cron_scheduler sc_cron_scheduler_t;

sc_cron_scheduler_t *sc_cron_create(sc_allocator_t *alloc, size_t max_tasks, bool enabled);
void sc_cron_destroy(sc_cron_scheduler_t *sched, sc_allocator_t *alloc);

sc_error_t sc_cron_add_job(sc_cron_scheduler_t *sched, sc_allocator_t *alloc,
    const char *expression, const char *command, const char *name,
    uint64_t *out_id);

sc_error_t sc_cron_remove_job(sc_cron_scheduler_t *sched, uint64_t job_id);
sc_error_t sc_cron_update_job(sc_cron_scheduler_t *sched, sc_allocator_t *alloc,
    uint64_t job_id, const char *expression, const char *command, const bool *enabled);

const sc_cron_job_t *sc_cron_get_job(const sc_cron_scheduler_t *sched, uint64_t job_id);
const sc_cron_job_t *sc_cron_list_jobs(const sc_cron_scheduler_t *sched, size_t *count);

sc_error_t sc_cron_add_run(sc_cron_scheduler_t *sched, sc_allocator_t *alloc,
    uint64_t job_id, int64_t started_at, const char *status, const char *output);
const sc_cron_run_t *sc_cron_list_runs(sc_cron_scheduler_t *sched,
    uint64_t job_id, size_t limit, size_t *count);

#endif
