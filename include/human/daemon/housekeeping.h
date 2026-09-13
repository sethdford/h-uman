/* include/human/daemon/housekeeping.h
 *
 * The service loop's once-per-minute housekeeping: proactive check-in
 * scheduling, intrinsic-drive tick, autodream / persona evolver / LoRA
 * nightly clocks, feed polling, A/B seeding, community-insight refresh.
 * Carved out of hu_service_run (daemon.c) 2026-09-12 — behavior-preserving
 * move of the 1,154-line `if (current_minute > last_cron_minute)` block.
 *
 * The context is the block's real surface: seven values it reads and three
 * pieces of loop state it advances. Pointers mark the advanced ones so the
 * caller keeps ownership of its loop variables and nothing is copied back
 * by hand. */
#ifndef HU_DAEMON_HOUSEKEEPING_H
#define HU_DAEMON_HOUSEKEEPING_H

#include "human/core/allocator.h"
#include "human/daemon.h"

#include <stddef.h>
#include <time.h>

struct hu_agent;
struct hu_config;
struct hu_graph;

typedef struct hu_daemon_housekeeping_ctx {
    /* read */
    hu_allocator_t *alloc;
    struct hu_agent *agent;
    const struct hu_config *config;
    hu_service_channel_t *channels;
    size_t channel_count;
    struct hu_graph *graph; /* knowledge graph handle, may be NULL */
    time_t t;               /* wall clock this tick */
    time_t current_minute;  /* t / 60 */
    /* advanced */
    time_t *last_cron_minute; /* set to current_minute once the tick ran */
    time_t *proactive_due_at; /* jittered hourly proactive check-in deadline */
    char *community_insights; /* service-loop-owned buffer, refreshed here */
    size_t community_insights_cap;
    size_t *community_insights_len;
} hu_daemon_housekeeping_ctx_t;

/* Runs the housekeeping block once when ctx->current_minute has advanced past
 * *ctx->last_cron_minute; a no-op otherwise. */
void hu_daemon_housekeeping_tick(hu_daemon_housekeeping_ctx_t *ctx);

#endif /* HU_DAEMON_HOUSEKEEPING_H */
