/*
 * Superhuman silence interpreter service — detects meaningful pauses.
 */
#include "human/agent/superhuman.h"
#include "human/agent/superhuman_silence.h"
#include "human/core/string.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ms(void) {
    return (uint64_t)time(NULL) * 1000;
}

static hu_error_t silence_build_context(void *ctx, hu_allocator_t *alloc, char **out,
                                         size_t *out_len) {
    hu_superhuman_silence_ctx_t *sctx = (hu_superhuman_silence_ctx_t *)ctx;
    if (!sctx || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (!sctx->has_prev)
        return HU_OK;

    uint64_t gap = sctx->last_ts_ms > sctx->prev_ts_ms ? sctx->last_ts_ms - sctx->prev_ts_ms
                                                       : sctx->prev_ts_ms - sctx->last_ts_ms;
    if (gap <= HU_SILENCE_GAP_MS)
        return HU_OK;

    static const char MSG[] =
        "A meaningful pause occurred. The user may be processing, reflecting, or uncertain. "
        "Don't rush to fill silence.";
    *out = hu_strndup(alloc, MSG, sizeof(MSG) - 1);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    *out_len = sizeof(MSG) - 1;
    return HU_OK;
}

static hu_error_t silence_observe(void *ctx, hu_allocator_t *alloc, const char *text,
                                   size_t text_len, const char *role, size_t role_len) {
    hu_superhuman_silence_ctx_t *sctx = (hu_superhuman_silence_ctx_t *)ctx;
    if (!sctx)
        return HU_ERR_INVALID_ARGUMENT;

    uint64_t ts = now_ms();
    sctx->prev_ts_ms = sctx->last_ts_ms;
    sctx->last_ts_ms = ts;
    sctx->has_prev = (sctx->prev_ts_ms != 0);

    (void)alloc;
    (void)text;
    (void)text_len;
    (void)role;
    (void)role_len;
    return HU_OK;
}

hu_error_t hu_superhuman_silence_service(hu_superhuman_silence_ctx_t *ctx,
                                          hu_superhuman_service_t *out) {
    if (!ctx || !out)
        return HU_ERR_INVALID_ARGUMENT;
    out->name = "Silence Interpreter";
    out->build_context = silence_build_context;
    out->observe = silence_observe;
    out->ctx = ctx;
    return HU_OK;
}
