#include "seaclaw/channel_loop.h"
#include <stdint.h>
#include <string.h>
#include <time.h>

#define SC_LOOP_MSG_BUF 16

void sc_channel_loop_state_init(sc_channel_loop_state_t *state) {
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    state->last_activity = (int64_t)time(NULL);
}

void sc_channel_loop_request_stop(sc_channel_loop_state_t *state) {
    if (state)
        state->stop_requested = true;
}

bool sc_channel_loop_should_stop(const sc_channel_loop_state_t *state) {
    return state ? state->stop_requested : true;
}

void sc_channel_loop_touch(sc_channel_loop_state_t *state) {
    if (state)
        state->last_activity = (int64_t)time(NULL);
}

sc_error_t sc_channel_loop_tick(sc_channel_loop_ctx_t *ctx, sc_channel_loop_state_t *state,
                                int *messages_processed) {
    if (!ctx || !state || !ctx->alloc)
        return SC_ERR_INVALID_ARGUMENT;
    if (messages_processed)
        *messages_processed = 0;
    if (!ctx->poll_fn || !ctx->dispatch_fn)
        return SC_ERR_INVALID_ARGUMENT;

    sc_channel_loop_msg_t msgs[SC_LOOP_MSG_BUF];
    size_t count = 0;
    sc_error_t err = ctx->poll_fn(ctx->channel_ctx, ctx->alloc, msgs, SC_LOOP_MSG_BUF, &count);
    if (err != SC_OK)
        return err;

    sc_channel_loop_touch(state);

    for (size_t i = 0; i < count && ctx->agent_ctx; i++) {
        char *response = NULL;
        err = ctx->dispatch_fn(ctx->agent_ctx, msgs[i].session_key, msgs[i].content, &response);
        if (response && ctx->alloc) {
            ctx->alloc->free(ctx->alloc->ctx, response, strlen(response) + 1);
        }
        if (messages_processed)
            (*messages_processed)++;
    }

    if (ctx->evict_fn && ctx->evict_ctx && ctx->idle_timeout_secs > 0) {
        ctx->evict_fn(ctx->evict_ctx, ctx->idle_timeout_secs);
    }
    return SC_OK;
}
