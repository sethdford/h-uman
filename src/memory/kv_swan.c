/* src/memory/kv_swan.c — SWAN codec: sliding-window + attention sinks.
 *
 * Phase 1 stub: returns HU_ERR_NOT_SUPPORTED for encode and a proper
 * decode path so existing blobs tagged SWAN can at least be rejected
 * gracefully.  Real SWAN eviction lands in a later phase. */

#include "human/memory/kv_compressor.h"

static hu_error_t swan_encode(void *ctx, hu_allocator_t *alloc,
                              const float *kv_data, size_t n_layers,
                              size_t n_heads, size_t head_dim,
                              size_t seq_len,
                              uint8_t **out_blob, size_t *out_len) {
    (void)ctx; (void)alloc; (void)kv_data;
    (void)n_layers; (void)n_heads; (void)head_dim; (void)seq_len;
    (void)out_blob; (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
}

static hu_error_t swan_decode(void *ctx, hu_allocator_t *alloc,
                              const uint8_t *blob, size_t blob_len,
                              float **out_kv, size_t *out_nl,
                              size_t *out_nh, size_t *out_hd,
                              size_t *out_sl) {
    (void)ctx; (void)alloc; (void)blob; (void)blob_len;
    (void)out_kv; (void)out_nl; (void)out_nh; (void)out_hd; (void)out_sl;
    return HU_ERR_NOT_SUPPORTED;
}

static hu_kv_codec_id_t swan_codec_id(void *ctx) {
    (void)ctx;
    return HU_KV_CODEC_SWAN;
}

static void swan_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const hu_kv_compressor_vtable_t s_swan_vtable = {
    .encode   = swan_encode,
    .decode   = swan_decode,
    .codec_id = swan_codec_id,
    .deinit   = swan_deinit,
};

hu_error_t hu_kv_compressor_create_swan(hu_allocator_t *alloc,
                                        hu_kv_compressor_t *out) {
    if (!alloc || !out) return HU_ERR_INVALID_ARGUMENT;
    out->ctx    = NULL;
    out->vtable = &s_swan_vtable;
    return HU_OK;
}
