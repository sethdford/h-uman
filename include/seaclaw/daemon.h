#ifndef SC_DAEMON_H
#define SC_DAEMON_H

#include "channel.h"
#include "channel_loop.h"
#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

sc_error_t sc_daemon_start(void);
sc_error_t sc_daemon_stop(void);
bool sc_daemon_status(void);

/* Cron schedule matching: 5-field expression (min hour dom month dow).
   Supports star, exact, step, range, range-step, and comma lists. */
bool sc_cron_schedule_matches(const char *schedule, const struct tm *tm);

struct sc_agent;

typedef struct sc_service_channel {
    void *channel_ctx;
    sc_channel_t *channel; /* full channel vtable — used for sending replies */
    sc_channel_loop_poll_fn poll_fn;
    uint32_t interval_ms;
    int64_t last_poll_ms;
} sc_service_channel_t;

/**
 * Run the service loop: polls channels, dispatches messages to the agent,
 * sends responses back, and executes cron jobs.
 * agent may be NULL for cron-only mode.
 * Blocks until SIGTERM/SIGINT. tick_interval_ms = 0 → default (1000ms).
 * In SC_IS_TEST mode: runs one tick and returns.
 */
sc_error_t sc_service_run(sc_allocator_t *alloc, uint32_t tick_interval_ms,
                          sc_service_channel_t *channels, size_t channel_count,
                          struct sc_agent *agent);

sc_error_t sc_daemon_install(sc_allocator_t *alloc);
sc_error_t sc_daemon_uninstall(void);
sc_error_t sc_daemon_logs(void);

#endif /* SC_DAEMON_H */
