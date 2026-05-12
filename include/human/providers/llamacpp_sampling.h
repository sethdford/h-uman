#ifndef HU_LLAMACPP_SAMPLING_H
#define HU_LLAMACPP_SAMPLING_H

/*
 * Phase 1 (RL SOTA) — pure-C sampling for the in-process llama.cpp
 * provider. Decoupled from llama.h so the unit tests can run without
 * a real model: callers feed in raw float logits and get back a token
 * index.
 *
 * Order of operations matches the standard llama.cpp / vLLM pipeline:
 *   1. logits / temperature  (skip if temperature == 0 -> greedy)
 *   2. top-k truncation      (skip if top_k == 0)
 *   3. top-p nucleus         (skip if top_p >= 1.0)
 *   4. min-p threshold       (skip if min_p == 0.0)
 *   5. softmax + multinomial draw (or argmax for greedy)
 *
 * PRNG: SplitMix64 + Xoshiro256** seeded from `seed`. State lives
 * inside hu_llamacpp_sampler_t so concurrent samplers don't collide
 * and so each sampler is reproducible from its seed.
 */

#include "human/core/error.h"

#include <stddef.h>
#include <stdint.h>

typedef struct hu_llamacpp_sampling_params {
    double   temperature; /* 0.0 -> greedy. Otherwise > 0. */
    int32_t  top_k;       /* 0 -> disabled. >0 -> keep top_k. */
    double   top_p;       /* 1.0 -> disabled. (0,1) -> nucleus. */
    double   min_p;       /* 0.0 -> disabled. (0,1) -> threshold vs max prob. */
    uint64_t seed;        /* Reproducibility key; 0 -> system random. */
} hu_llamacpp_sampling_params_t;

typedef struct hu_llamacpp_sampler {
    hu_llamacpp_sampling_params_t params;
    /* Xoshiro256** state. */
    uint64_t s[4];
    /* Scratch buffers, lazily resized on first pick. */
    float   *probs;
    int32_t *idx;
    size_t   buf_capacity;
} hu_llamacpp_sampler_t;

hu_error_t hu_llamacpp_sampler_init(hu_llamacpp_sampler_t *sampler,
                                    const hu_llamacpp_sampling_params_t *params);

hu_error_t hu_llamacpp_sampler_pick(hu_llamacpp_sampler_t *sampler,
                                    const float *logits, size_t vocab_size,
                                    int32_t *out_token);

void hu_llamacpp_sampler_free(hu_llamacpp_sampler_t *sampler);

#endif /* HU_LLAMACPP_SAMPLING_H */
