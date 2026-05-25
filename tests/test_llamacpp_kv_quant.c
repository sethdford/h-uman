/*
 * Phase 1 (Gemma throughput program) — KV-quant string parsing contract.
 *
 * The enum mapping is the only piece of Phase 1 that's testable without
 * libllama linked. The actual GGML type assignment at
 * src/providers/llamacpp.c:llama_init_from_model() exercises only under
 * the rl_sota preset; this suite pins the operator-facing contract so a
 * future refactor can't silently change which strings map to which
 * quant level.
 *
 * Contract:
 *   - "fp16" / "f16" / "16"          → HU_KV_QUANT_FP16   (default)
 *   - "q8_0" / "q8" / "8"            → HU_KV_QUANT_Q8_0
 *   - "q4_0" / "q4" / "4"            → HU_KV_QUANT_Q4_0
 *   - NULL / "" / unknown            → HU_KV_QUANT_FP16, recognized=false
 *   - case-insensitive
 *   - to_string is the round-trip inverse for the canonical names
 */

#include "human/providers/llamacpp.h"

#include "test_framework.h"

#include <stdbool.h>
#include <string.h>

static void test_kv_quant_from_string_defaults_to_fp16_on_null(void) {
    bool recognized = true;
    HU_ASSERT_EQ(hu_kv_quant_from_string(NULL, &recognized), HU_KV_QUANT_FP16);
    HU_ASSERT_FALSE(recognized);
}

static void test_kv_quant_from_string_defaults_to_fp16_on_empty(void) {
    bool recognized = true;
    HU_ASSERT_EQ(hu_kv_quant_from_string("", &recognized), HU_KV_QUANT_FP16);
    HU_ASSERT_FALSE(recognized);
}

static void test_kv_quant_from_string_canonical_q8_0(void) {
    bool recognized = false;
    HU_ASSERT_EQ(hu_kv_quant_from_string("q8_0", &recognized), HU_KV_QUANT_Q8_0);
    HU_ASSERT_TRUE(recognized);
}

static void test_kv_quant_from_string_shorthand_q8(void) {
    /* Operators conflate mlx-lm "q8" with llama.cpp "q8_0". Accepting
     * both keeps the operator-facing schema friendly. */
    HU_ASSERT_EQ(hu_kv_quant_from_string("q8", NULL), HU_KV_QUANT_Q8_0);
    HU_ASSERT_EQ(hu_kv_quant_from_string("8", NULL), HU_KV_QUANT_Q8_0);
}

static void test_kv_quant_from_string_canonical_q4_0(void) {
    bool recognized = false;
    HU_ASSERT_EQ(hu_kv_quant_from_string("q4_0", &recognized), HU_KV_QUANT_Q4_0);
    HU_ASSERT_TRUE(recognized);
    HU_ASSERT_EQ(hu_kv_quant_from_string("q4", NULL), HU_KV_QUANT_Q4_0);
    HU_ASSERT_EQ(hu_kv_quant_from_string("4", NULL), HU_KV_QUANT_Q4_0);
}

static void test_kv_quant_from_string_fp16_aliases(void) {
    HU_ASSERT_EQ(hu_kv_quant_from_string("fp16", NULL), HU_KV_QUANT_FP16);
    HU_ASSERT_EQ(hu_kv_quant_from_string("f16", NULL), HU_KV_QUANT_FP16);
    HU_ASSERT_EQ(hu_kv_quant_from_string("16", NULL), HU_KV_QUANT_FP16);
}

static void test_kv_quant_from_string_is_case_insensitive(void) {
    HU_ASSERT_EQ(hu_kv_quant_from_string("Q8_0", NULL), HU_KV_QUANT_Q8_0);
    HU_ASSERT_EQ(hu_kv_quant_from_string("FP16", NULL), HU_KV_QUANT_FP16);
    HU_ASSERT_EQ(hu_kv_quant_from_string("Q4_0", NULL), HU_KV_QUANT_Q4_0);
}

static void test_kv_quant_from_string_unknown_returns_fp16_unrecognized(void) {
    /* Adversarial input — must not silently quantize. The operator wants
     * to see "unrecognized" so they can fix the typo, not get a quiet
     * fallback to a value they didn't request. */
    bool recognized = true;
    HU_ASSERT_EQ(hu_kv_quant_from_string("q2_0", &recognized), HU_KV_QUANT_FP16);
    HU_ASSERT_FALSE(recognized);
    HU_ASSERT_EQ(hu_kv_quant_from_string("int8", NULL), HU_KV_QUANT_FP16);
    HU_ASSERT_EQ(hu_kv_quant_from_string("bf16", NULL), HU_KV_QUANT_FP16);
    HU_ASSERT_EQ(hu_kv_quant_from_string("garbage", NULL), HU_KV_QUANT_FP16);
}

static void test_kv_quant_to_string_returns_canonical_lowercase(void) {
    HU_ASSERT_STR_EQ(hu_kv_quant_to_string(HU_KV_QUANT_FP16), "fp16");
    HU_ASSERT_STR_EQ(hu_kv_quant_to_string(HU_KV_QUANT_Q8_0), "q8_0");
    HU_ASSERT_STR_EQ(hu_kv_quant_to_string(HU_KV_QUANT_Q4_0), "q4_0");
}

static void test_kv_quant_round_trip_canonical_names(void) {
    /* to_string → from_string → to_string must be the identity on the
     * canonical wire form. Pins the operator-facing schema: if anyone
     * adds a new variant, this test forces them to also keep the
     * round-trip property. */
    hu_kv_quant_t levels[] = {HU_KV_QUANT_FP16, HU_KV_QUANT_Q8_0, HU_KV_QUANT_Q4_0};
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        const char *canonical = hu_kv_quant_to_string(levels[i]);
        bool recognized = false;
        HU_ASSERT_EQ(hu_kv_quant_from_string(canonical, &recognized), levels[i]);
        HU_ASSERT_TRUE(recognized);
    }
}

static void test_kv_quant_from_string_accepts_null_out_recognized(void) {
    /* The out_recognized pointer is optional. Pinned so a NULL never
     * crashes — the recognition signal is purely advisory. */
    HU_ASSERT_EQ(hu_kv_quant_from_string("q8_0", NULL), HU_KV_QUANT_Q8_0);
    HU_ASSERT_EQ(hu_kv_quant_from_string("garbage", NULL), HU_KV_QUANT_FP16);
}

void run_llamacpp_kv_quant_tests(void) {
    HU_TEST_SUITE("llamacpp_kv_quant");
    HU_RUN_TEST(test_kv_quant_from_string_defaults_to_fp16_on_null);
    HU_RUN_TEST(test_kv_quant_from_string_defaults_to_fp16_on_empty);
    HU_RUN_TEST(test_kv_quant_from_string_canonical_q8_0);
    HU_RUN_TEST(test_kv_quant_from_string_shorthand_q8);
    HU_RUN_TEST(test_kv_quant_from_string_canonical_q4_0);
    HU_RUN_TEST(test_kv_quant_from_string_fp16_aliases);
    HU_RUN_TEST(test_kv_quant_from_string_is_case_insensitive);
    HU_RUN_TEST(test_kv_quant_from_string_unknown_returns_fp16_unrecognized);
    HU_RUN_TEST(test_kv_quant_to_string_returns_canonical_lowercase);
    HU_RUN_TEST(test_kv_quant_round_trip_canonical_names);
    HU_RUN_TEST(test_kv_quant_from_string_accepts_null_out_recognized);
}
