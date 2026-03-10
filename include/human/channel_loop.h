#ifndef HU_CHANNEL_LOOP_H
#define HU_CHANNEL_LOOP_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Loop state — shared between supervisor and polling thread
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_channel_loop_state {
    int64_t last_activity; /* epoch seconds, updated after each poll */
    bool stop_requested;   /* set by supervisor to request stop */
} hu_channel_loop_state_t;

/* Initialize loop state. */
void hu_channel_loop_state_init(hu_channel_loop_state_t *state);

/* Request stop (called by supervisor). */
void hu_channel_loop_request_stop(hu_channel_loop_state_t *state);

/* Check if stop requested. */
bool hu_channel_loop_should_stop(const hu_channel_loop_state_t *state);

/* Update last_activity to current time. */
void hu_channel_loop_touch(hu_channel_loop_state_t *state);

/* ──────────────────────────────────────────────────────────────────────────
 * Eviction callback — called periodically to evict idle sessions
 * ────────────────────────────────────────────────────────────────────────── */

typedef size_t (*hu_channel_loop_evict_fn)(void *ctx, uint64_t max_idle_secs);

/* ──────────────────────────────────────────────────────────────────────────
 * Poll callback — returns messages; caller frees. Placeholder for future
 * channel-specific poll implementations.
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_channel_loop_msg {
    char session_key[128];
    char content[4096];
    bool is_group;
    int64_t message_id;  /* platform message ID for reactions; -1 if unknown */
    bool has_attachment; /* true if message has image attachment (for vision pipeline) */
} hu_channel_loop_msg_t;

typedef hu_error_t (*hu_channel_loop_poll_fn)(void *channel_ctx, hu_allocator_t *alloc,
                                              hu_channel_loop_msg_t *msgs, size_t max_msgs,
                                              size_t *out_count);

typedef hu_error_t (*hu_channel_loop_dispatch_fn)(void *agent_ctx, const char *session_key,
                                                  const char *content, char **response_out);

typedef struct hu_channel_loop_ctx {
    hu_allocator_t *alloc;
    void *channel_ctx;
    void *agent_ctx;
    hu_channel_loop_poll_fn poll_fn;
    hu_channel_loop_dispatch_fn dispatch_fn;
    hu_channel_loop_evict_fn evict_fn;
    void *evict_ctx;
    uint64_t evict_interval;
    uint64_t idle_timeout_secs;
} hu_channel_loop_ctx_t;

/* One poll iteration: poll, dispatch each message, optionally evict. */
hu_error_t hu_channel_loop_tick(hu_channel_loop_ctx_t *ctx, hu_channel_loop_state_t *state,
                                int *messages_processed);

#endif /* HU_CHANNEL_LOOP_H */
