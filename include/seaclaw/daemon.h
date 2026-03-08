#ifndef SC_DAEMON_H
#define SC_DAEMON_H

#include "channel.h"
#include "channel_loop.h"
#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct sc_agent;
struct sc_config;

sc_error_t sc_daemon_start(void);
sc_error_t sc_daemon_stop(void);
bool sc_daemon_status(void);

/* Cron schedule matching: 5-field expression (min hour dom month dow).
   Supports star, exact, step, range, range-step, and comma lists. */
bool sc_cron_schedule_matches(const char *schedule, const struct tm *tm);

struct sc_agent;
struct sc_config;

typedef sc_error_t (*sc_channel_webhook_fn)(void *channel_ctx, sc_allocator_t *alloc,
                                            const char *body, size_t body_len);

typedef struct sc_service_channel {
    void *channel_ctx;
    sc_channel_t *channel; /* full channel vtable — used for sending replies */
    sc_channel_loop_poll_fn poll_fn;
    sc_channel_webhook_fn webhook_fn; /* optional — NULL for polling-only channels */
    uint32_t interval_ms;
    int64_t last_poll_ms;
} sc_service_channel_t;

/**
 * Run the service loop: polls channels, dispatches messages to the agent,
 * sends responses back, and executes cron jobs.
 * agent may be NULL for cron-only mode.
 * config may be NULL; when non-NULL, used for per-channel persona overrides.
 * Blocks until SIGTERM/SIGINT. tick_interval_ms = 0 → default (1000ms).
 * In SC_IS_TEST mode: runs one tick and returns.
 */
sc_error_t sc_service_run(sc_allocator_t *alloc, uint32_t tick_interval_ms,
                          sc_service_channel_t *channels, size_t channel_count,
                          struct sc_agent *agent, const struct sc_config *config);

/* Run agent-type cron jobs that match the current time.
 * Iterates the agent's in-memory scheduler, fires sc_agent_turn for SC_CRON_JOB_AGENT entries,
 * and routes responses to the specified channel. */
sc_error_t sc_service_run_agent_cron(sc_allocator_t *alloc, struct sc_agent *agent,
                                     sc_service_channel_t *channels, size_t channel_count);

#ifdef SC_HAS_PERSONA
/* Proactive check-ins: iterate contacts with proactive_checkin=true,
 * check last interaction time, and initiate natural conversations. */
void sc_service_run_proactive_checkins(sc_allocator_t *alloc, struct sc_agent *agent,
                                       sc_service_channel_t *channels, size_t channel_count);
#endif

sc_error_t sc_daemon_install(sc_allocator_t *alloc);
sc_error_t sc_daemon_uninstall(void);
sc_error_t sc_daemon_logs(void);

#endif /* SC_DAEMON_H */
