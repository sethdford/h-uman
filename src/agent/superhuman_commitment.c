/*
 * Superhuman commitment keeper service — builds context from commitment store.
 */
#include "seaclaw/agent/commitment.h"
#include "seaclaw/agent/commitment_store.h"
#include "seaclaw/agent/superhuman.h"
#include "seaclaw/agent/superhuman_commitment.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sc_error_t commitment_build_context(void *ctx, sc_allocator_t *alloc, char **out,
                                            size_t *out_len) {
    sc_superhuman_commitment_ctx_t *cctx = (sc_superhuman_commitment_ctx_t *)ctx;
    if (!cctx || !alloc || !out || !out_len)
        return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (!cctx->store)
        return SC_OK;

    sc_commitment_t *list = NULL;
    size_t count = 0;
    sc_error_t err = sc_commitment_store_list_active(
        cctx->store, alloc, cctx->session_id ? cctx->session_id : NULL,
        cctx->session_id_len, &list, &count);
    if (err != SC_OK)
        return err;

    char *buf = NULL;
    if (count > 0) {
        char msg[256];
        int n = snprintf(msg, sizeof(msg),
                         "You are tracking %zu active commitments for the user. Follow up "
                         "naturally on any that are overdue.",
                         count);
        if (n > 0 && (size_t)n < sizeof(msg)) {
            buf = sc_strndup(alloc, msg, (size_t)n);
            if (buf) {
                *out = buf;
                *out_len = (size_t)n;
            }
        }
    }

    if (list) {
        for (size_t i = 0; i < count; i++)
            sc_commitment_deinit(&list[i], alloc);
        alloc->free(alloc->ctx, list, count * sizeof(sc_commitment_t));
    }
    return SC_OK;
}

static sc_error_t commitment_observe(void *ctx, sc_allocator_t *alloc, const char *text,
                                      size_t text_len, const char *role, size_t role_len) {
    (void)ctx;
    (void)alloc;
    (void)text;
    (void)text_len;
    (void)role;
    (void)role_len;
    return SC_OK;
}

sc_error_t sc_superhuman_commitment_service(sc_superhuman_commitment_ctx_t *ctx,
                                             sc_superhuman_service_t *out) {
    if (!ctx || !out)
        return SC_ERR_INVALID_ARGUMENT;
    out->name = "Commitment Keeper";
    out->build_context = commitment_build_context;
    out->observe = commitment_observe;
    out->ctx = ctx;
    return SC_OK;
}
