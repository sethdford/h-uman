/* src/memory/kv_deltakv.c — DeltaKV codec: int8 quantization with per-channel
 * min/max scaling.  Lossy but typically 4x compression for KV-cache data.
 *
 * Wire layout after the 24-byte HUKV header:
 *   [float min_k, float max_k, float min_v, float max_v]  (16 bytes)
 *   [int8_t quantized_data[n_elements]]                    (n_elements bytes)
 *
 * n_elements = n_layers * n_heads * head_dim * seq_len (per K and V channel).
 * Total encoded floats = n_layers * 2 * n_heads * head_dim * seq_len
 * where the factor of 2 accounts for K and V channels. */

#include "human/memory/kv_compressor.h"
#include <float.h>
#include <math.h>
#include <string.h>

static void write_f32_le(uint8_t *p, float v) {
    uint32_t u;
    memcpy(&u, &v, sizeof(u));
    p[0] = (uint8_t)(u);
    p[1] = (uint8_t)(u >> 8);
    p[2] = (uint8_t)(u >> 16);
    p[3] = (uint8_t)(u >> 24);
}

static float read_f32_le(const uint8_t *p) {
    uint32_t u = (uint32_t)p[0]
               | ((uint32_t)p[1] << 8)
               | ((uint32_t)p[2] << 16)
               | ((uint32_t)p[3] << 24);
    float v;
    memcpy(&v, &u, sizeof(v));
    return v;
}

static void minmax(const float *data, size_t n, float *lo, float *hi) {
    *lo = FLT_MAX;
    *hi = -FLT_MAX;
    for (size_t i = 0; i < n; i++) {
        if (data[i] < *lo) *lo = data[i];
        if (data[i] > *hi) *hi = data[i];
    }
    if (*lo == *hi) *hi = *lo + 1.0f;
}

static hu_error_t deltakv_encode(void *ctx, hu_allocator_t *alloc,
                                 const float *kv_data, size_t n_layers,
                                 size_t n_heads, size_t head_dim,
                                 size_t seq_len,
                                 uint8_t **out_blob, size_t *out_len) {
    (void)ctx;
    if (!alloc || !kv_data || !out_blob || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    size_t n_per_channel = n_layers * n_heads * head_dim * seq_len;
    if (n_per_channel == 0)
        return HU_ERR_INVALID_ARGUMENT;

    /* Input has 2 channels (K and V) interleaved as
     * [K floats..., V floats...] each of size n_per_channel. */
    const float *k_data = kv_data;
    const float *v_data = kv_data + n_per_channel;

    float k_min, k_max, v_min, v_max;
    minmax(k_data, n_per_channel, &k_min, &k_max);
    minmax(v_data, n_per_channel, &v_min, &v_max);

    /* header(24) + scales(16) + quantized_k(n) + quantized_v(n) */
    size_t payload_meta = 16;
    size_t blob_len = HU_KV_BLOB_HEADER_SIZE + payload_meta + 2 * n_per_channel;

    uint8_t *blob = alloc->alloc(alloc->ctx, blob_len);
    if (!blob) return HU_ERR_OUT_OF_MEMORY;

    hu_kv_blob_write_header(blob, HU_KV_CODEC_DELTAKV,
                            (uint32_t)n_layers, (uint32_t)n_heads,
                            (uint32_t)head_dim, (uint32_t)seq_len);

    uint8_t *meta = blob + HU_KV_BLOB_HEADER_SIZE;
    write_f32_le(meta + 0,  k_min);
    write_f32_le(meta + 4,  k_max);
    write_f32_le(meta + 8,  v_min);
    write_f32_le(meta + 12, v_max);

    int8_t *q_k = (int8_t *)(meta + payload_meta);
    int8_t *q_v = q_k + n_per_channel;

    float k_scale = (k_max - k_min) / 255.0f;
    float v_scale = (v_max - v_min) / 255.0f;

    for (size_t i = 0; i < n_per_channel; i++) {
        float nk = (k_data[i] - k_min) / k_scale;
        q_k[i] = (int8_t)(nk > 127.0f ? 127 : (nk < -128.0f ? -128 : (int8_t)roundf(nk - 128.0f)));
    }
    for (size_t i = 0; i < n_per_channel; i++) {
        float nv = (v_data[i] - v_min) / v_scale;
        q_v[i] = (int8_t)(nv > 127.0f ? 127 : (nv < -128.0f ? -128 : (int8_t)roundf(nv - 128.0f)));
    }

    *out_blob = blob;
    *out_len  = blob_len;
    return HU_OK;
}

static hu_error_t deltakv_decode(void *ctx, hu_allocator_t *alloc,
                                 const uint8_t *blob, size_t blob_len,
                                 float **out_kv, size_t *out_nl,
                                 size_t *out_nh, size_t *out_hd,
                                 size_t *out_sl) {
    (void)ctx;
    if (!alloc || !blob || !out_kv || !out_nl || !out_nh || !out_hd || !out_sl)
        return HU_ERR_INVALID_ARGUMENT;

    hu_kv_codec_id_t codec;
    uint32_t nl, nh, hd, sl;
    hu_error_t err = hu_kv_blob_read_header(blob, blob_len, &codec,
                                            &nl, &nh, &hd, &sl);
    if (err != HU_OK) return err;
    if (codec != HU_KV_CODEC_DELTAKV) return HU_ERR_INVALID_ARGUMENT;

    size_t n_per_channel = (size_t)nl * nh * hd * sl;
    size_t expected = HU_KV_BLOB_HEADER_SIZE + 16 + 2 * n_per_channel;
    if (blob_len < expected) return HU_ERR_INVALID_ARGUMENT;

    const uint8_t *meta = blob + HU_KV_BLOB_HEADER_SIZE;
    float k_min = read_f32_le(meta + 0);
    float k_max = read_f32_le(meta + 4);
    float v_min = read_f32_le(meta + 8);
    float v_max = read_f32_le(meta + 12);

    float k_scale = (k_max - k_min) / 255.0f;
    float v_scale = (v_max - v_min) / 255.0f;

    const int8_t *q_k = (const int8_t *)(meta + 16);
    const int8_t *q_v = q_k + n_per_channel;

    size_t total_floats = 2 * n_per_channel;
    float *kv = alloc->alloc(alloc->ctx, total_floats * sizeof(float));
    if (!kv) return HU_ERR_OUT_OF_MEMORY;

    float *k_out = kv;
    float *v_out = kv + n_per_channel;

    for (size_t i = 0; i < n_per_channel; i++)
        k_out[i] = ((float)(q_k[i] + 128)) * k_scale + k_min;
    for (size_t i = 0; i < n_per_channel; i++)
        v_out[i] = ((float)(q_v[i] + 128)) * v_scale + v_min;

    *out_kv = kv;
    *out_nl = nl;
    *out_nh = nh;
    *out_hd = hd;
    *out_sl = sl;
    return HU_OK;
}

static hu_kv_codec_id_t deltakv_codec_id(void *ctx) {
    (void)ctx;
    return HU_KV_CODEC_DELTAKV;
}

static void deltakv_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const hu_kv_compressor_vtable_t s_deltakv_vtable = {
    .encode   = deltakv_encode,
    .decode   = deltakv_decode,
    .codec_id = deltakv_codec_id,
    .deinit   = deltakv_deinit,
};

hu_error_t hu_kv_compressor_create_deltakv(hu_allocator_t *alloc,
                                           hu_kv_compressor_t *out) {
    if (!alloc || !out) return HU_ERR_INVALID_ARGUMENT;
    out->ctx    = NULL;
    out->vtable = &s_deltakv_vtable;
    return HU_OK;
}
