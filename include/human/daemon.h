#ifndef HU_DAEMON_H
#define HU_DAEMON_H

#include "channel.h"
#include "channel_loop.h"
#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct hu_agent;
struct hu_config;

hu_error_t hu_daemon_start(void);
hu_error_t hu_daemon_stop(void);
bool hu_daemon_status(void);

/* Write/remove PID file for foreground service-loop instances.
 * Used by cmd_service_loop so hu_daemon_status() can detect running instances. */
hu_error_t hu_daemon_write_pid(void);
void hu_daemon_remove_pid(void);

/* Cron schedule matching: 5-field expression (min hour dom month dow).
   Supports star, exact, step, range, range-step, and comma lists. */
bool hu_cron_schedule_matches(const char *schedule, const struct tm *tm);

struct hu_agent;
struct hu_config;

typedef hu_error_t (*hu_channel_webhook_fn)(void *channel_ctx, hu_allocator_t *alloc,
                                            const char *body, size_t body_len);

typedef struct hu_service_channel {
    void *channel_ctx;
    hu_channel_t *channel; /* full channel vtable — used for sending replies */
    hu_channel_loop_poll_fn poll_fn;
    hu_channel_webhook_fn webhook_fn; /* optional — NULL for polling-only channels */
    uint32_t interval_ms;
    int64_t last_poll_ms;
    uint64_t last_contact_ms;
} hu_service_channel_t;

/**
 * Run the service loop: polls channels, dispatches messages to the agent,
 * sends responses back, and executes cron jobs.
 * agent may be NULL for cron-only mode.
 * config may be NULL; when non-NULL, used for per-channel persona overrides.
 * Blocks until SIGTERM/SIGINT. tick_interval_ms = 0 → default (1000ms).
 * In HU_IS_TEST mode: runs one tick and returns.
 */
hu_error_t hu_service_run(hu_allocator_t *alloc, uint32_t tick_interval_ms,
                          hu_service_channel_t *channels, size_t channel_count,
                          struct hu_agent *agent, const struct hu_config *config);

/* Run agent-type cron jobs that match the current time.
 * Iterates the agent's in-memory scheduler, fires hu_agent_turn for HU_CRON_JOB_AGENT entries,
 * and routes responses to the specified channel. */
hu_error_t hu_service_run_agent_cron(hu_allocator_t *alloc, struct hu_agent *agent,
                                     hu_service_channel_t *channels, size_t channel_count);

#ifdef HU_HAS_PERSONA
/* Proactive check-ins: iterate contacts with proactive_checkin=true,
 * check last interaction time, and initiate natural conversations. */
void hu_service_run_proactive_checkins(hu_allocator_t *alloc, struct hu_agent *agent,
                                       hu_service_channel_t *channels, size_t channel_count);
#endif

hu_error_t hu_daemon_install(hu_allocator_t *alloc);
hu_error_t hu_daemon_uninstall(void);
hu_error_t hu_daemon_logs(void);

#endif /* HU_DAEMON_H */
