/* src/agent/turn/turn_ctx.c — see include/human/agent/turn.h. */
#include "human/agent/turn.h"
#include <string.h>

static void turn_ctx_release(hu_allocator_t *alloc, char **s, size_t *len) {
    if (*s)
        alloc->free(alloc->ctx, *s, *len + 1);
    *s = NULL;
    *len = 0;
}

hu_turn_ctx_t *hu_turn_ctx_new(hu_agent_t *agent, const char *msg, size_t msg_len,
                               char **response_out, size_t *response_len_out) {
    if (!agent || !agent->alloc)
        return NULL;
    hu_turn_ctx_t *turn_ctx =
        (hu_turn_ctx_t *)agent->alloc->alloc(agent->alloc->ctx, sizeof(*turn_ctx));
    if (!turn_ctx)
        return NULL;
    memset(turn_ctx, 0, sizeof(*turn_ctx));
    turn_ctx->alloc = agent->alloc;
    turn_ctx->in.agent = agent;
    turn_ctx->in.msg = msg;
    turn_ctx->in.msg_len = msg_len;
    turn_ctx->in.response_out = response_out;
    turn_ctx->in.response_len_out = response_len_out;
    return turn_ctx;
}

void hu_turn_ctx_free(hu_turn_ctx_t *turn_ctx) {
    if (!turn_ctx)
        return;
    hu_allocator_t *alloc = turn_ctx->alloc;
    turn_ctx_release(alloc, &turn_ctx->retrieval.memory_ctx, &turn_ctx->retrieval.memory_ctx_len);
    turn_ctx_release(alloc, &turn_ctx->retrieval.graph_ctx, &turn_ctx->retrieval.graph_ctx_len);
    alloc->free(alloc->ctx, turn_ctx, sizeof(*turn_ctx));
}
