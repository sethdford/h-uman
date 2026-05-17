#ifndef HU_KV_COMPRESSOR_H
#define HU_KV_COMPRESSOR_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "HUKV" little-endian magic for the on-wire blob envelope. */
#define HU_KV_BLOB_MAGIC   0x564B5548u /* 'H','U','K','V' */
#define HU_KV_BLOB_VERSION 1u

/* Wire-format header layout (24 bytes, little-endian, packed):
 *   offset 0:  uint32  magic        (HU_KV_BLOB_MAGIC)
 *   offset 4:  uint8   version      (HU_KV_BLOB_VERSION)
 *   offset 5:  uint8   codec_id     (hu_kv_codec_id_t)
 *   offset 6:  uint16  flags        (must be 0 in v1)
 *   offset 8:  uint32  n_layers
 *   offset 12: uint32  n_heads
 *   offset 16: uint32  head_dim
 *   offset 20: uint32  seq_len
 *   offset 24: payload (codec-specific)
 */
#define HU_KV_BLOB_HEADER_SIZE 24u

typedef enum hu_kv_codec_id {
    HU_KV_CODEC_NONE    = 0, /* passthrough — raw fp32 */
    HU_KV_CODEC_DELTAKV = 1, /* low-rank residual coding */
    HU_KV_CODEC_SWAN    = 2, /* sliding-window + attention sinks */
} hu_kv_codec_id_t;

typedef struct hu_kv_compressor_vtable {
    hu_error_t (*encode)(void *ctx, hu_allocator_t *alloc,
                         const float *kv_data, size_t n_layers,
                         size_t n_heads, size_t head_dim, size_t seq_len,
                         uint8_t **out_blob, size_t *out_blob_len);

    hu_error_t (*decode)(void *ctx, hu_allocator_t *alloc,
                         const uint8_t *blob, size_t blob_len,
                         float **out_kv, size_t *out_n_layers,
                         size_t *out_n_heads, size_t *out_head_dim,
                         size_t *out_seq_len);

    hu_kv_codec_id_t (*codec_id)(void *ctx);

    void (*deinit)(void *ctx, hu_allocator_t *alloc);
} hu_kv_compressor_vtable_t;

typedef struct hu_kv_compressor {
    void                            *ctx;
    const hu_kv_compressor_vtable_t *vtable;
} hu_kv_compressor_t;

/* ── Envelope helpers (shared by all codecs) ──────────────────────────── */

/* Pack a 24-byte header into buf (must be >= HU_KV_BLOB_HEADER_SIZE). */
void hu_kv_blob_write_header(uint8_t *buf, hu_kv_codec_id_t codec,
                             uint32_t n_layers, uint32_t n_heads,
                             uint32_t head_dim, uint32_t seq_len);

/* Parse and validate an envelope header from a blob.  Returns HU_OK on
 * success, HU_ERR_INVALID_ARGUMENT on bad magic / version / truncation. */
hu_error_t hu_kv_blob_read_header(const uint8_t *blob, size_t blob_len,
                                  hu_kv_codec_id_t *out_codec,
                                  uint32_t *out_n_layers,
                                  uint32_t *out_n_heads,
                                  uint32_t *out_head_dim,
                                  uint32_t *out_seq_len);

/* ── Factories ────────────────────────────────────────────────────────── */

hu_error_t hu_kv_compressor_create_deltakv(hu_allocator_t *alloc,
                                           hu_kv_compressor_t *out);

hu_error_t hu_kv_compressor_create_swan(hu_allocator_t *alloc,
                                        hu_kv_compressor_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HU_KV_COMPRESSOR_H */
