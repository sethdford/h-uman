/*
 * Phase 1 (RL SOTA) — sampling module unit tests.
 *
 * The sampling module is intentionally decoupled from llama.h: it
 * operates on a flat float array of logits + a known seed, so we can
 * test it without loading a real model. The decode loop wires it to
 * llama_get_logits_ith() output at runtime.
 *
 * Coverage:
 *   - greedy (temperature == 0) returns argmax
 *   - top_k == 1 forces argmax even with hot temperature
 *   - identical seed/params/logits -> identical token (cross-instance)
 *   - 8 distinct seeds at temperature 1.5 produce >= 2 distinct tokens
 *     (a sampler that always returns the same token is broken)
 *   - large vocab (V > 64) uses the qsort path; argmax still wins
 *   - NULL/zero arg rejection
 */

#include "human/providers/llamacpp_sampling.h"

#include "human/core/error.h"
#include "test_framework.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void test_sampling_greedy_returns_argmax(void) {
    float logits[5] = {0.1f, 0.5f, 0.2f, 0.9f, 0.4f};
    hu_llamacpp_sampling_params_t params = {
        .temperature = 0.0,
        .top_k = 0,
        .top_p = 1.0,
        .min_p = 0.0,
        .seed = 0,
    };
    hu_llamacpp_sampler_t sampler = {0};
    HU_ASSERT_EQ(hu_llamacpp_sampler_init(&sampler, &params), HU_OK);
    int32_t tok = -1;
    HU_ASSERT_EQ(hu_llamacpp_sampler_pick(&sampler, logits, 5, &tok), HU_OK);
    HU_ASSERT_EQ(tok, 3);
    hu_llamacpp_sampler_free(&sampler);
}

static void test_sampling_top_k_1_equals_argmax(void) {
    float logits[5] = {0.1f, 0.5f, 0.2f, 0.9f, 0.4f};
    hu_llamacpp_sampling_params_t params = {
        .temperature = 1.0,
        .top_k = 1,
        .top_p = 1.0,
        .min_p = 0.0,
        .seed = 12345,
    };
    hu_llamacpp_sampler_t sampler = {0};
    HU_ASSERT_EQ(hu_llamacpp_sampler_init(&sampler, &params), HU_OK);
    int32_t tok = -1;
    HU_ASSERT_EQ(hu_llamacpp_sampler_pick(&sampler, logits, 5, &tok), HU_OK);
    HU_ASSERT_EQ(tok, 3);
    hu_llamacpp_sampler_free(&sampler);
}

static void test_sampling_deterministic_with_fixed_seed(void) {
    /* Same seed + same logits + same params -> same token, every time.
     * Two distinct sampler instances must agree. */
    float logits[10];
    for (int i = 0; i < 10; i++) logits[i] = (float)(i % 4) * 0.1f;
    hu_llamacpp_sampling_params_t params = {
        .temperature = 0.7,
        .top_k = 0,
        .top_p = 0.95,
        .min_p = 0.05,
        .seed = 42,
    };
    int32_t tok_a = -1, tok_b = -1;
    hu_llamacpp_sampler_t sampler_a = {0}, sampler_b = {0};
    HU_ASSERT_EQ(hu_llamacpp_sampler_init(&sampler_a, &params), HU_OK);
    HU_ASSERT_EQ(hu_llamacpp_sampler_init(&sampler_b, &params), HU_OK);
    HU_ASSERT_EQ(hu_llamacpp_sampler_pick(&sampler_a, logits, 10, &tok_a), HU_OK);
    HU_ASSERT_EQ(hu_llamacpp_sampler_pick(&sampler_b, logits, 10, &tok_b), HU_OK);
    HU_ASSERT_EQ(tok_a, tok_b);
    hu_llamacpp_sampler_free(&sampler_a);
    hu_llamacpp_sampler_free(&sampler_b);
}

static void test_sampling_different_seeds_can_differ(void) {
    /* 8 distinct seeds, hot temperature: at least 2 distinct tokens
     * must appear. A sampler that ignores the PRNG would fail this. */
    float logits[10];
    for (int i = 0; i < 10; i++) logits[i] = (float)i * 0.05f;
    int32_t observed[8];
    for (int s = 0; s < 8; s++) {
        hu_llamacpp_sampling_params_t params = {
            .temperature = 1.5,
            .top_k = 0,
            .top_p = 1.0,
            .min_p = 0.0,
            .seed = (uint64_t)(s * 1000003u + 7u),
        };
        hu_llamacpp_sampler_t sampler = {0};
        HU_ASSERT_EQ(hu_llamacpp_sampler_init(&sampler, &params), HU_OK);
        HU_ASSERT_EQ(hu_llamacpp_sampler_pick(&sampler, logits, 10, &observed[s]), HU_OK);
        hu_llamacpp_sampler_free(&sampler);
    }
    int distinct = 0;
    for (int i = 0; i < 8; i++) {
        bool fresh = true;
        for (int j = 0; j < i; j++)
            if (observed[i] == observed[j]) { fresh = false; break; }
        if (fresh) distinct++;
    }
    HU_ASSERT_TRUE(distinct >= 2);
}

static void test_sampling_qsort_path_argmax_with_large_vocab(void) {
    /* vocab_size > 64 forces the qsort path. Synthesize 128 logits
     * with a clear winner at index 100; greedy must return it,
     * proving the qsort + thread-local logits pointer round-trip. */
    enum { V = 128 };
    float logits[V];
    for (int i = 0; i < V; i++) logits[i] = (float)i * 0.001f;
    logits[100] = 999.0f;
    hu_llamacpp_sampling_params_t params = {.temperature = 0.0, .top_p = 1.0};
    hu_llamacpp_sampler_t sampler = {0};
    HU_ASSERT_EQ(hu_llamacpp_sampler_init(&sampler, &params), HU_OK);
    int32_t tok = -1;
    HU_ASSERT_EQ(hu_llamacpp_sampler_pick(&sampler, logits, V, &tok), HU_OK);
    HU_ASSERT_EQ(tok, 100);

    /* Now stochastic with top_k=2, hot temperature, fixed seed: the
     * qsort path keeps tokens 100 and 127 (next-largest). Either is
     * acceptable; we just need the call not to crash and to stay
     * within the kept set. */
    params.temperature = 1.0; params.top_k = 2; params.seed = 999;
    hu_llamacpp_sampler_free(&sampler);
    HU_ASSERT_EQ(hu_llamacpp_sampler_init(&sampler, &params), HU_OK);
    HU_ASSERT_EQ(hu_llamacpp_sampler_pick(&sampler, logits, V, &tok), HU_OK);
    HU_ASSERT_TRUE(tok == 100 || tok == 127);
    hu_llamacpp_sampler_free(&sampler);
}

static void test_sampling_rejects_null_args(void) {
    hu_llamacpp_sampler_t sampler = {0};
    hu_llamacpp_sampling_params_t params = {.temperature = 1.0, .top_p = 1.0};
    HU_ASSERT_EQ(hu_llamacpp_sampler_init(NULL, &params), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_llamacpp_sampler_init(&sampler, NULL), HU_ERR_INVALID_ARGUMENT);

    HU_ASSERT_EQ(hu_llamacpp_sampler_init(&sampler, &params), HU_OK);
    int32_t tok = 0;
    float l[1] = {0.0f};
    HU_ASSERT_EQ(hu_llamacpp_sampler_pick(NULL, l, 1, &tok), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_llamacpp_sampler_pick(&sampler, NULL, 1, &tok), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_llamacpp_sampler_pick(&sampler, l, 0, &tok), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_llamacpp_sampler_pick(&sampler, l, 1, NULL), HU_ERR_INVALID_ARGUMENT);
    hu_llamacpp_sampler_free(&sampler);
}

void run_llamacpp_sampling_tests(void) {
    HU_RUN_TEST(test_sampling_greedy_returns_argmax);
    HU_RUN_TEST(test_sampling_top_k_1_equals_argmax);
    HU_RUN_TEST(test_sampling_deterministic_with_fixed_seed);
    HU_RUN_TEST(test_sampling_different_seeds_can_differ);
    HU_RUN_TEST(test_sampling_qsort_path_argmax_with_large_vocab);
    HU_RUN_TEST(test_sampling_rejects_null_args);
}
