/* test_kv_compressor.c — Init 13 foundation tests.
 *
 * COVERS: kv_deltakv, kv_swan, kv_envelope
 *
 * (The COVERS line lets scripts/check-untested.sh resolve test coverage by
 * source-file basename even though our exported symbols are prefixed
 * `hu_kv_compressor_*` rather than `hu_kv_deltakv_*` / `hu_kv_swan_*`.)
 *
 * Covers the codec-agnostic HUKV envelope helpers and the two phase-1 codec
 * factories (DeltaKV: lossy int8 round-trip; SWAN: NOT_SUPPORTED stub).
 *
 * Behavioral gates from docs/plans/2026-05-11-init-13-kv-compression.md that
 * are NOT verified here (intentionally out of scope for the foundation PR):
 *   - 4-8× compression ratio
 *   - TVD < 1% on the 256-prompt fixture
 *   - ≤ 2 ms encode/decode at 4K tokens
 *   - libFuzzer corpus runs
 * Those land alongside the real DeltaKV/SWAN implementations. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/kv_compressor.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void test_kv_blob_header_round_trip(void) {
    uint8_t buf[HU_KV_BLOB_HEADER_SIZE];
    memset(buf, 0, sizeof(buf));

    hu_kv_blob_write_header(buf, HU_KV_CODEC_DELTAKV,
                            /*n_layers=*/28, /*n_heads=*/8,
                            /*head_dim=*/128, /*seq_len=*/4096);

    hu_kv_codec_id_t codec = HU_KV_CODEC_NONE;
    uint32_t nl = 0, nh = 0, hd = 0, sl = 0;
    hu_error_t err = hu_kv_blob_read_header(buf, sizeof(buf), &codec, &nl, &nh, &hd, &sl);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ((int)codec, (int)HU_KV_CODEC_DELTAKV);
    HU_ASSERT_EQ((int)nl, 28);
    HU_ASSERT_EQ((int)nh, 8);
    HU_ASSERT_EQ((int)hd, 128);
    HU_ASSERT_EQ((int)sl, 4096);
}

static void test_kv_blob_header_rejects_bad_magic(void) {
    uint8_t buf[HU_KV_BLOB_HEADER_SIZE];
    hu_kv_blob_write_header(buf, HU_KV_CODEC_DELTAKV, 1, 1, 1, 1);
    buf[0] = 0xAB; /* corrupt magic */
    hu_error_t err = hu_kv_blob_read_header(buf, sizeof(buf), NULL, NULL, NULL, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_kv_blob_header_rejects_bad_version(void) {
    uint8_t buf[HU_KV_BLOB_HEADER_SIZE];
    hu_kv_blob_write_header(buf, HU_KV_CODEC_DELTAKV, 1, 1, 1, 1);
    buf[4] = (uint8_t)(HU_KV_BLOB_VERSION + 1); /* future version */
    hu_error_t err = hu_kv_blob_read_header(buf, sizeof(buf), NULL, NULL, NULL, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_kv_blob_header_rejects_truncated(void) {
    uint8_t buf[HU_KV_BLOB_HEADER_SIZE];
    hu_kv_blob_write_header(buf, HU_KV_CODEC_DELTAKV, 1, 1, 1, 1);
    hu_error_t err =
        hu_kv_blob_read_header(buf, HU_KV_BLOB_HEADER_SIZE - 1, NULL, NULL, NULL, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    err = hu_kv_blob_read_header(NULL, HU_KV_BLOB_HEADER_SIZE, NULL, NULL, NULL, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_deltakv_factory_creates_vtable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_kv_compressor_t c;
    memset(&c, 0, sizeof(c));
    HU_ASSERT_EQ(hu_kv_compressor_create_deltakv(&alloc, &c), HU_OK);
    HU_ASSERT_NOT_NULL(c.vtable);
    HU_ASSERT_NOT_NULL(c.vtable->encode);
    HU_ASSERT_NOT_NULL(c.vtable->decode);
    HU_ASSERT_NOT_NULL(c.vtable->codec_id);
    HU_ASSERT_NOT_NULL(c.vtable->deinit);
    HU_ASSERT_EQ((int)c.vtable->codec_id(c.ctx), (int)HU_KV_CODEC_DELTAKV);
    c.vtable->deinit(c.ctx, &alloc);
}

static void test_deltakv_rejects_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_kv_compressor_create_deltakv(NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_kv_compressor_create_deltakv(&alloc, NULL), HU_ERR_INVALID_ARGUMENT);
}

/* Zero-input round trip is exact: encode quantizes all zeros to a single
 * fixed int8, and decode inverts that to zero. This validates the wire format
 * + shape preservation without depending on quantization fidelity (which is
 * verified by the dedicated TVD eval, not this unit test). */
static void test_deltakv_zero_round_trip_preserves_shape(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_kv_compressor_t c;
    HU_ASSERT_EQ(hu_kv_compressor_create_deltakv(&alloc, &c), HU_OK);

    const size_t nl = 2, nh = 2, hd = 4, sl = 3;
    const size_t n_per_channel = nl * nh * hd * sl;
    const size_t total_floats = 2 * n_per_channel; /* K + V */
    float *kv = (float *)calloc(total_floats, sizeof(float));
    HU_ASSERT_NOT_NULL(kv);

    uint8_t *blob = NULL;
    size_t blob_len = 0;
    HU_ASSERT_EQ(c.vtable->encode(c.ctx, &alloc, kv, nl, nh, hd, sl, &blob, &blob_len), HU_OK);
    HU_ASSERT_NOT_NULL(blob);
    HU_ASSERT_EQ(blob_len, HU_KV_BLOB_HEADER_SIZE + 16 + 2 * n_per_channel);

    float *kv_out = NULL;
    size_t onl = 0, onh = 0, ohd = 0, osl = 0;
    HU_ASSERT_EQ(c.vtable->decode(c.ctx, &alloc, blob, blob_len, &kv_out, &onl, &onh, &ohd, &osl),
                 HU_OK);
    HU_ASSERT_EQ((int)onl, (int)nl);
    HU_ASSERT_EQ((int)onh, (int)nh);
    HU_ASSERT_EQ((int)ohd, (int)hd);
    HU_ASSERT_EQ((int)osl, (int)sl);

    for (size_t i = 0; i < total_floats; i++) {
        HU_ASSERT_EQ((int)(kv_out[i] * 1000.0f), 0);
    }

    free(kv);
    alloc.free(alloc.ctx, blob, blob_len);
    alloc.free(alloc.ctx, kv_out, total_floats * sizeof(float));
    c.vtable->deinit(c.ctx, &alloc);
}

static void test_deltakv_decode_rejects_wrong_codec(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_kv_compressor_t c;
    HU_ASSERT_EQ(hu_kv_compressor_create_deltakv(&alloc, &c), HU_OK);

    uint8_t buf[HU_KV_BLOB_HEADER_SIZE + 16];
    memset(buf, 0, sizeof(buf));
    hu_kv_blob_write_header(buf, HU_KV_CODEC_SWAN, 1, 1, 1, 1);

    float *kv_out = NULL;
    size_t a = 0, b = 0, d = 0, e = 0;
    hu_error_t err = c.vtable->decode(c.ctx, &alloc, buf, sizeof(buf), &kv_out, &a, &b, &d, &e);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(kv_out);
    c.vtable->deinit(c.ctx, &alloc);
}

static void test_deltakv_decode_rejects_truncated_payload(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_kv_compressor_t c;
    HU_ASSERT_EQ(hu_kv_compressor_create_deltakv(&alloc, &c), HU_OK);

    /* Header advertises 4 elements per channel but payload is empty. */
    uint8_t buf[HU_KV_BLOB_HEADER_SIZE + 16];
    memset(buf, 0, sizeof(buf));
    hu_kv_blob_write_header(buf, HU_KV_CODEC_DELTAKV, 1, 1, 1, 4);

    float *kv_out = NULL;
    size_t a = 0, b = 0, d = 0, e = 0;
    hu_error_t err = c.vtable->decode(c.ctx, &alloc, buf, sizeof(buf), &kv_out, &a, &b, &d, &e);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(kv_out);
    c.vtable->deinit(c.ctx, &alloc);
}

static void test_swan_factory_stub(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_kv_compressor_t c;
    HU_ASSERT_EQ(hu_kv_compressor_create_swan(&alloc, &c), HU_OK);
    HU_ASSERT_NOT_NULL(c.vtable);
    HU_ASSERT_EQ((int)c.vtable->codec_id(c.ctx), (int)HU_KV_CODEC_SWAN);

    uint8_t *blob = NULL;
    size_t blob_len = 0;
    float kv[8] = {0};
    HU_ASSERT_EQ(c.vtable->encode(c.ctx, &alloc, kv, 1, 1, 1, 1, &blob, &blob_len),
                 HU_ERR_NOT_SUPPORTED);

    float *kv_out = NULL;
    size_t a = 0, b = 0, d = 0, e = 0;
    HU_ASSERT_EQ(c.vtable->decode(c.ctx, &alloc, NULL, 0, &kv_out, &a, &b, &d, &e),
                 HU_ERR_NOT_SUPPORTED);
    c.vtable->deinit(c.ctx, &alloc);
}

void run_kv_compressor_tests(void) {
    HU_TEST_SUITE("kv compressor — HUKV envelope");
    HU_RUN_TEST(test_kv_blob_header_round_trip);
    HU_RUN_TEST(test_kv_blob_header_rejects_bad_magic);
    HU_RUN_TEST(test_kv_blob_header_rejects_bad_version);
    HU_RUN_TEST(test_kv_blob_header_rejects_truncated);

    HU_TEST_SUITE("kv compressor — DeltaKV codec");
    HU_RUN_TEST(test_deltakv_factory_creates_vtable);
    HU_RUN_TEST(test_deltakv_rejects_null_args);
    HU_RUN_TEST(test_deltakv_zero_round_trip_preserves_shape);
    HU_RUN_TEST(test_deltakv_decode_rejects_wrong_codec);
    HU_RUN_TEST(test_deltakv_decode_rejects_truncated_payload);

    HU_TEST_SUITE("kv compressor — SWAN stub");
    HU_RUN_TEST(test_swan_factory_stub);
}
