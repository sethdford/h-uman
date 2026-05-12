/*
 * Phase 1 (RL SOTA) — decode loop unit tests with a mock logits
 * provider. The real decode loop pulls logits from
 * llama_get_logits_ith(); these tests inject a deterministic logits
 * function so the loop can be exercised without a real model.
 *
 * Coverage:
 *   - happy path: produces 1..N tokens with mock that bumps the
 *     argmax index each step and a NULL advance (unit-test shortcut)
 *   - EOS halts BEFORE appending the EOS token
 *   - advance is called exactly once per sampled non-EOS token
 *   - advance failure halts the loop and propagates the error
 *   - NULL/missing required-arg rejection
 */

#include "human/providers/llamacpp_decode.h"
#include "human/providers/llamacpp_sampling.h"

#include "human/core/error.h"
#include "test_framework.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct mock_logits_state {
    int32_t next_token;
    int32_t eos_token;
    size_t  vocab_size;
} mock_logits_state_t;

static hu_error_t mock_logits_provider(void *ctx, size_t batch_pos,
                                       float *out_logits,
                                       size_t vocab_size) {
    (void)batch_pos;
    mock_logits_state_t *s = (mock_logits_state_t *)ctx;
    if (vocab_size != s->vocab_size) return HU_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < vocab_size; i++) out_logits[i] = 0.0f;
    out_logits[s->next_token] = 10.0f;
    s->next_token++;
    return HU_OK;
}

static int s_advance_call_count;
static int32_t s_advance_last_token;
static hu_error_t mock_advance(void *ctx, int32_t token) {
    (void)ctx;
    s_advance_call_count++;
    s_advance_last_token = token;
    return HU_OK;
}

static int s_failing_advance_seen;
static hu_error_t failing_advance_helper(void *ctx, int32_t token) {
    (void)token;
    int *fail_after = (int *)ctx;
    s_failing_advance_seen++;
    return (s_failing_advance_seen > *fail_after) ? HU_ERR_PROVIDER_RESPONSE
                                                  : HU_OK;
}

static void test_decode_produces_expected_token_sequence(void) {
    mock_logits_state_t state = {.next_token = 1, .eos_token = 99, .vocab_size = 100};
    hu_llamacpp_sampling_params_t params = {.temperature = 0.0, .top_p = 1.0};
    hu_llamacpp_sampler_t sampler = {0};
    hu_llamacpp_sampler_init(&sampler, &params);
    int32_t produced[10] = {0};
    size_t produced_len = 0;
    hu_llamacpp_decode_config_t cfg = {
        .max_tokens = 10, .eos_token = 99, .vocab_size = 100,
        .logits_provider = mock_logits_provider,
        .logits_ctx = &state,
        .advance = NULL,
        .sampler = &sampler,
    };
    HU_ASSERT_EQ(hu_llamacpp_decode_run(&cfg, produced, &produced_len), HU_OK);
    HU_ASSERT_EQ(produced_len, (size_t)10);
    for (int i = 0; i < 10; i++) HU_ASSERT_EQ(produced[i], i + 1);
    hu_llamacpp_sampler_free(&sampler);
}

static void test_decode_halts_on_eos(void) {
    mock_logits_state_t state = {.next_token = 1, .eos_token = 3, .vocab_size = 100};
    hu_llamacpp_sampling_params_t params = {.temperature = 0.0, .top_p = 1.0};
    hu_llamacpp_sampler_t sampler = {0};
    hu_llamacpp_sampler_init(&sampler, &params);
    int32_t produced[10] = {0};
    size_t produced_len = 0;
    hu_llamacpp_decode_config_t cfg = {
        .max_tokens = 10, .eos_token = 3, .vocab_size = 100,
        .logits_provider = mock_logits_provider,
        .logits_ctx = &state,
        .advance = NULL,
        .sampler = &sampler,
    };
    HU_ASSERT_EQ(hu_llamacpp_decode_run(&cfg, produced, &produced_len), HU_OK);
    /* Loop produces 1, 2, then sees 3 == EOS and halts WITHOUT appending it. */
    HU_ASSERT_EQ(produced_len, (size_t)2);
    HU_ASSERT_EQ(produced[0], 1);
    HU_ASSERT_EQ(produced[1], 2);
    hu_llamacpp_sampler_free(&sampler);
}

static void test_decode_calls_advance_once_per_token(void) {
    /* Pin the production contract: when advance is supplied, it must
     * be called exactly once per non-EOS sampled token, with that token. */
    mock_logits_state_t state = {.next_token = 1, .eos_token = 99, .vocab_size = 100};
    hu_llamacpp_sampling_params_t params = {.temperature = 0.0, .top_p = 1.0};
    hu_llamacpp_sampler_t sampler = {0};
    hu_llamacpp_sampler_init(&sampler, &params);
    int32_t produced[5] = {0};
    size_t produced_len = 0;
    s_advance_call_count = 0;
    s_advance_last_token = -1;
    hu_llamacpp_decode_config_t cfg = {
        .max_tokens = 5, .eos_token = 99, .vocab_size = 100,
        .logits_provider = mock_logits_provider,
        .logits_ctx = &state,
        .advance = mock_advance,
        .advance_ctx = &state,
        .sampler = &sampler,
    };
    HU_ASSERT_EQ(hu_llamacpp_decode_run(&cfg, produced, &produced_len), HU_OK);
    HU_ASSERT_EQ(produced_len, (size_t)5);
    HU_ASSERT_EQ(s_advance_call_count, 5);
    HU_ASSERT_EQ(s_advance_last_token, 5); /* last produced token is 5 */
    hu_llamacpp_sampler_free(&sampler);
}

static void test_decode_advance_failure_halts_loop(void) {
    /* If advance returns non-OK after N successes, the loop must halt
     * and propagate the error. */
    mock_logits_state_t state = {.next_token = 1, .eos_token = 99, .vocab_size = 100};
    hu_llamacpp_sampling_params_t params = {.temperature = 0.0, .top_p = 1.0};
    hu_llamacpp_sampler_t sampler = {0};
    hu_llamacpp_sampler_init(&sampler, &params);
    int fail_after = 2;
    s_failing_advance_seen = 0;
    int32_t produced[5] = {0};
    size_t produced_len = 0;
    hu_llamacpp_decode_config_t cfg = {
        .max_tokens = 5, .eos_token = 99, .vocab_size = 100,
        .logits_provider = mock_logits_provider,
        .logits_ctx = &state,
        .advance = failing_advance_helper,
        .advance_ctx = &fail_after,
        .sampler = &sampler,
    };
    HU_ASSERT_EQ(hu_llamacpp_decode_run(&cfg, produced, &produced_len),
                 HU_ERR_PROVIDER_RESPONSE);
    /* Tokens 1, 2 advanced OK; token 3 sampled + appended, then advance
     * failed on the 3rd call. produced_len reflects what was appended. */
    HU_ASSERT_EQ(produced_len, (size_t)3);
    hu_llamacpp_sampler_free(&sampler);
}

static void test_decode_rejects_null_args(void) {
    int32_t produced[1];
    size_t produced_len;
    hu_llamacpp_sampling_params_t params = {.temperature = 0.0, .top_p = 1.0};
    hu_llamacpp_sampler_t sampler = {0};
    hu_llamacpp_sampler_init(&sampler, &params);
    HU_ASSERT_EQ(hu_llamacpp_decode_run(NULL, produced, &produced_len),
                 HU_ERR_INVALID_ARGUMENT);
    /* Missing logits_provider. */
    hu_llamacpp_decode_config_t bad = {
        .max_tokens = 1, .eos_token = 0,
        .logits_provider = NULL, .sampler = &sampler,
    };
    HU_ASSERT_EQ(hu_llamacpp_decode_run(&bad, produced, &produced_len),
                 HU_ERR_INVALID_ARGUMENT);
    hu_llamacpp_sampler_free(&sampler);
}

void run_llamacpp_decode_tests(void) {
    HU_RUN_TEST(test_decode_produces_expected_token_sequence);
    HU_RUN_TEST(test_decode_halts_on_eos);
    HU_RUN_TEST(test_decode_calls_advance_once_per_token);
    HU_RUN_TEST(test_decode_advance_failure_halts_loop);
    HU_RUN_TEST(test_decode_rejects_null_args);
}
