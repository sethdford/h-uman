#ifndef HU_CRON_H
#define HU_CRON_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_cron_job_type {
    HU_CRON_JOB_SHELL = 0, /* run command via /bin/sh */
    HU_CRON_JOB_AGENT = 1, /* trigger an agent turn with a prompt */
} hu_cron_job_type_t;

typedef struct hu_cron_job {
    uint64_t id;
    char *expression;
    char *command; /* shell: command to run; agent: prompt to send */
    char *name;
    char *channel; /* agent jobs: target channel for response */
    int64_t next_run_secs;
    int64_t last_run_secs;
    char *last_status;
    hu_cron_job_type_t type;
    bool paused;
    bool one_shot;
    bool enabled;
    int64_t created_at_s;
} hu_cron_job_t;

typedef struct hu_cron_run {
    uint64_t id;
    uint64_t job_id;
    int64_t started_at_s;
    int64_t finished_at_s;
    char *status;
    char *output;
} hu_cron_run_t;

typedef struct hu_cron_scheduler hu_cron_scheduler_t;

hu_cron_scheduler_t *hu_cron_create(hu_allocator_t *alloc, size_t max_tasks, bool enabled);
void hu_cron_destroy(hu_cron_scheduler_t *sched, hu_allocator_t *alloc);

hu_error_t hu_cron_add_job(hu_cron_scheduler_t *sched, hu_allocator_t *alloc,
                           const char *expression, const char *command, const char *name,
                           uint64_t *out_id);

/* Add an agent-type cron job that triggers an agent turn instead of a shell command.
 * prompt: the message to send to the agent each time the job fires.
 * channel: target channel for the agent's response (e.g. "slack", "imessage"). */
hu_error_t hu_cron_add_agent_job(hu_cron_scheduler_t *sched, hu_allocator_t *alloc,
                                 const char *expression, const char *prompt, const char *channel,
                                 const char *name, uint64_t *out_id);

hu_error_t hu_cron_remove_job(hu_cron_scheduler_t *sched, uint64_t job_id);
hu_error_t hu_cron_update_job(hu_cron_scheduler_t *sched, hu_allocator_t *alloc, uint64_t job_id,
                              const char *expression, const char *command, const bool *enabled);

hu_error_t hu_cron_set_job_one_shot(hu_cron_scheduler_t *sched, uint64_t job_id, bool one_shot);

const hu_cron_job_t *hu_cron_get_job(const hu_cron_scheduler_t *sched, uint64_t job_id);
const hu_cron_job_t *hu_cron_list_jobs(const hu_cron_scheduler_t *sched, size_t *count);

hu_error_t hu_cron_add_run(hu_cron_scheduler_t *sched, hu_allocator_t *alloc, uint64_t job_id,
                           int64_t started_at, const char *status, const char *output);
const hu_cron_run_t *hu_cron_list_runs(hu_cron_scheduler_t *sched, uint64_t job_id, size_t limit,
                                       size_t *count);

#endif
