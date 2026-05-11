#include "human/behavior/user_sim.h"

#include <stdlib.h>
#include <string.h>

typedef struct hu_user_sim_scripted_state {
    const char *const *lines;
    size_t line_count;
    size_t next;
} hu_user_sim_scripted_state_t;

static void scripted_deinit(void *ctx) {
    free(ctx);
}

static hu_error_t scripted_next(void *ctx, const hu_user_sim_turn_ctx_t *tctx, char **out,
                                 size_t *out_len) {
    (void)tctx;
    hu_user_sim_scripted_state_t *st = (hu_user_sim_scripted_state_t *)ctx;
    if (!st || !out || !out_len) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;
    *out_len = 0;
    if (st->next >= st->line_count) {
        return HU_OK;
    }
    const char *line = st->lines[st->next++];
    if (!line) {
        return HU_OK;
    }
    size_t n = strlen(line);
    char *buf = (char *)malloc(n + 1);
    if (!buf) {
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(buf, line, n + 1);
    *out = buf;
    *out_len = n;
    return HU_OK;
}

static const hu_user_sim_vtable_t HU_USER_SIM_SCRIPTED_VT = {
    .name = "scripted-lines",
    .next_user_message = scripted_next,
    .deinit = scripted_deinit,
};

hu_user_sim_t hu_user_sim_scripted(const char *const *lines, size_t line_count) {
    hu_user_sim_t sim = {0};
    if (!lines || line_count == 0) {
        return sim;
    }
    hu_user_sim_scripted_state_t *st =
        (hu_user_sim_scripted_state_t *)malloc(sizeof(hu_user_sim_scripted_state_t));
    if (!st) {
        return sim;
    }
    st->lines = lines;
    st->line_count = line_count;
    st->next = 0;
    sim.ctx = st;
    sim.vtable = &HU_USER_SIM_SCRIPTED_VT;
    return sim;
}

hu_error_t hu_user_sim_next(hu_user_sim_t *sim, const hu_user_sim_turn_ctx_t *tctx, char **out,
                            size_t *out_len) {
    if (!sim || !sim->vtable || !sim->vtable->next_user_message) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    return sim->vtable->next_user_message(sim->ctx, tctx, out, out_len);
}

void hu_user_sim_deinit(hu_user_sim_t *sim, hu_allocator_t *alloc) {
    (void)alloc;
    if (!sim || !sim->vtable || !sim->vtable->deinit) {
        return;
    }
    sim->vtable->deinit(sim->ctx);
    sim->ctx = NULL;
    sim->vtable = NULL;
}
