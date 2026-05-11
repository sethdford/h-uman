#ifndef HU_LLAMACPP_DECODE_H
#define HU_LLAMACPP_DECODE_H

/*
 * Phase 1 (RL SOTA) — decode loop for the in-process llama.cpp provider.
 *
 * Owns the per-token loop:
 *   1. Pull current next-token logits via logits_provider
 *   2. Sample via the supplied sampler
 *   3. Feed the sampled token back to the model via advance
 *      (so the next iteration's logits reflect it)
 *   4. Repeat until EOS or max_tokens
 *
 * Decoupled from llama.h via two callbacks:
 *   - logits_provider: bound to llama_get_logits_ith(ctx, -1) in production,
 *                      bound to a mock that synthesizes per-step distributions
 *                      in unit tests.
 *   - advance:         bound to llama_decode(ctx, llama_batch_get_one(&tok, 1))
 *                      in production; in unit tests where the mock logits
 *                      already self-advance, this can be NULL.
 *
 * The advance callback is REQUIRED in production. Without it the loop
 * sees identical logits every step (frozen at the last decoded position)
 * and emits the same token N times. Unit tests that use a mock which
 * self-advances on each get may pass NULL.
 */

#include "human/core/error.h"
#include "human/providers/llamacpp_sampling.h"

#include <stddef.h>
#include <stdint.h>

typedef hu_error_t (*hu_llamacpp_logits_fn)(void *ctx, size_t batch_pos,
                                            float *out_logits,
                                            size_t vocab_size);

/* Append the just-sampled token to the model state. Returns HU_OK on
 * success; non-OK halts the decode loop and is propagated. */
typedef hu_error_t (*hu_llamacpp_advance_fn)(void *ctx, int32_t token);

typedef struct hu_llamacpp_decode_config {
    size_t  max_tokens;
    int32_t eos_token;
    size_t  vocab_size; /* if 0, falls back to a unit-test default */
    hu_llamacpp_logits_fn  logits_provider;
    hu_llamacpp_advance_fn advance; /* may be NULL ONLY if logits_provider self-advances */
    void *logits_ctx;
    void *advance_ctx;              /* often == logits_ctx; kept separate for flexibility */
    hu_llamacpp_sampler_t *sampler;
} hu_llamacpp_decode_config_t;

hu_error_t hu_llamacpp_decode_run(const hu_llamacpp_decode_config_t *cfg,
                                  int32_t *out_tokens, size_t *out_tokens_len);

#endif /* HU_LLAMACPP_DECODE_H */
