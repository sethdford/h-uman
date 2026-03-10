/*
 * Superhuman commitment keeper service — builds context from commitment store.
 */
#include "human/agent/commitment.h"
#include "human/agent/commitment_store.h"
#include "human/agent/superhuman.h"
#include "human/agent/superhuman_commitment.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hu_error_t commitment_build_context(void *ctx, hu_allocator_t *alloc, char **out,
                                            size_t *out_len) {
    hu_superhuman_commitment_ctx_t *cctx = (hu_superhuman_commitment_ctx_t *)ctx;
    if (!cctx || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (!cctx->store)
        return HU_OK;

    const char *sess = cctx->session_id;
    size_t sess_len = sess ? cctx->session_id_len : 0;

    hu_commitment_t *list = NULL;
    size_t count = 0;
    hu_error_t err = hu_commitment_store_list_active(
        cctx->store, alloc, sess, sess_len, &list, &count);
    if (err != HU_OK)
        return err;

    char *buf = NULL;
    if (count > 0) {
        char msg[256];
        int n = snprintf(msg, sizeof(msg),
                         "You are tracking %zu active commitments for the user. Follow up "
                         "naturally on any that are overdue.",
                         count);
        if (n > 0 && (size_t)n < sizeof(msg)) {
            buf = hu_strndup(alloc, msg, (size_t)n);
            if (buf) {
                *out = buf;
                *out_len = (size_t)n;
            }
        }
    }

    if (list) {
        for (size_t i = 0; i < count; i++)
            hu_commitment_deinit(&list[i], alloc);
        alloc->free(alloc->ctx, list, count * sizeof(hu_commitment_t));
    }
    return HU_OK;
}

static hu_error_t commitment_observe(void *ctx, hu_allocator_t *alloc, const char *text,
                                      size_t text_len, const char *role, size_t role_len) {
    (void)ctx;
    (void)alloc;
    (void)text;
    (void)text_len;
    (void)role;
    (void)role_len;
    return HU_OK;
}

hu_error_t hu_superhuman_commitment_service(hu_superhuman_commitment_ctx_t *ctx,
                                             hu_superhuman_service_t *out) {
    if (!ctx || !out)
        return HU_ERR_INVALID_ARGUMENT;
    out->name = "Commitment Keeper";
    out->build_context = commitment_build_context;
    out->observe = commitment_observe;
    out->ctx = ctx;
    return HU_OK;
}
