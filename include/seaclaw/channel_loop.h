#ifndef SC_CHANNEL_LOOP_H
#define SC_CHANNEL_LOOP_H

#include "core/allocator.h"
#include "core/error.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Loop state — shared between supervisor and polling thread
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_channel_loop_state {
    int64_t last_activity;    /* epoch seconds, updated after each poll */
    bool stop_requested;      /* set by supervisor to request stop */
} sc_channel_loop_state_t;

/* Initialize loop state. */
void sc_channel_loop_state_init(sc_channel_loop_state_t *state);

/* Request stop (called by supervisor). */
void sc_channel_loop_request_stop(sc_channel_loop_state_t *state);

/* Check if stop requested. */
bool sc_channel_loop_should_stop(const sc_channel_loop_state_t *state);

/* Update last_activity to current time. */
void sc_channel_loop_touch(sc_channel_loop_state_t *state);

/* ──────────────────────────────────────────────────────────────────────────
 * Eviction callback — called periodically to evict idle sessions
 * ────────────────────────────────────────────────────────────────────────── */

typedef size_t (*sc_channel_loop_evict_fn)(void *ctx, uint64_t max_idle_secs);

/* ──────────────────────────────────────────────────────────────────────────
 * Poll callback — returns messages; caller frees. Placeholder for future
 * channel-specific poll implementations.
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_channel_loop_msg {
    char session_key[128];
    char content[4096];
} sc_channel_loop_msg_t;

typedef sc_error_t (*sc_channel_loop_poll_fn)(void *channel_ctx,
                                               sc_allocator_t *alloc,
                                               sc_channel_loop_msg_t *msgs,
                                               size_t max_msgs,
                                               size_t *out_count);

typedef sc_error_t (*sc_channel_loop_dispatch_fn)(void *agent_ctx,
                                                   const char *session_key,
                                                   const char *content,
                                                   char **response_out);

typedef struct sc_channel_loop_ctx {
    sc_allocator_t *alloc;
    void *channel_ctx;
    void *agent_ctx;
    sc_channel_loop_poll_fn poll_fn;
    sc_channel_loop_dispatch_fn dispatch_fn;
    sc_channel_loop_evict_fn evict_fn;
    void *evict_ctx;
    uint64_t evict_interval;
    uint64_t idle_timeout_secs;
} sc_channel_loop_ctx_t;

/* One poll iteration: poll, dispatch each message, optionally evict. */
sc_error_t sc_channel_loop_tick(sc_channel_loop_ctx_t *ctx,
                                sc_channel_loop_state_t *state,
                                int *messages_processed);

#endif /* SC_CHANNEL_LOOP_H */
