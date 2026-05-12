/* src/memory/kv_compressor.c — Shared HUKV envelope read/write helpers.
 *
 * The 24-byte header is codec-agnostic; individual codecs (DeltaKV, SWAN)
 * call these helpers from their encode/decode implementations. */

#include "human/memory/kv_compressor.h"
#include <string.h>

static void write_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

void hu_kv_blob_write_header(uint8_t *buf, hu_kv_codec_id_t codec,
                             uint32_t n_layers, uint32_t n_heads,
                             uint32_t head_dim, uint32_t seq_len) {
    write_u32_le(buf + 0, HU_KV_BLOB_MAGIC);
    buf[4] = (uint8_t)HU_KV_BLOB_VERSION;
    buf[5] = (uint8_t)codec;
    buf[6] = 0; /* flags hi */
    buf[7] = 0; /* flags lo */
    write_u32_le(buf + 8,  n_layers);
    write_u32_le(buf + 12, n_heads);
    write_u32_le(buf + 16, head_dim);
    write_u32_le(buf + 20, seq_len);
}

hu_error_t hu_kv_blob_read_header(const uint8_t *blob, size_t blob_len,
                                  hu_kv_codec_id_t *out_codec,
                                  uint32_t *out_n_layers,
                                  uint32_t *out_n_heads,
                                  uint32_t *out_head_dim,
                                  uint32_t *out_seq_len) {
    if (!blob || blob_len < HU_KV_BLOB_HEADER_SIZE)
        return HU_ERR_INVALID_ARGUMENT;

    uint32_t magic = read_u32_le(blob);
    if (magic != HU_KV_BLOB_MAGIC)
        return HU_ERR_INVALID_ARGUMENT;

    uint8_t version = blob[4];
    if (version != HU_KV_BLOB_VERSION)
        return HU_ERR_INVALID_ARGUMENT;

    if (out_codec)     *out_codec     = (hu_kv_codec_id_t)blob[5];
    if (out_n_layers)  *out_n_layers  = read_u32_le(blob + 8);
    if (out_n_heads)   *out_n_heads   = read_u32_le(blob + 12);
    if (out_head_dim)  *out_head_dim  = read_u32_le(blob + 16);
    if (out_seq_len)   *out_seq_len   = read_u32_le(blob + 20);
    return HU_OK;
}
