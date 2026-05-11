/*
 * Phase 1 (RL SOTA) — sampling implementation. See the header for the
 * pipeline order; this file is pure C with libc only so it links into
 * human_core regardless of whether libllama is present.
 *
 * Critic-pinned invariants:
 *  - The qsort comparator is a file-scope function; the per-call logits
 *    pointer is passed via a static __thread slot (qsort can't take a
 *    closure). __thread keeps concurrent samplers from clobbering each
 *    other. The "struct local with static fn" trick from earlier
 *    drafts was non-conforming C11 on Clang.
 *  - ensure_buffers() commits the first realloc before attempting the
 *    second; if probs grew but idx couldn't, we still have a valid
 *    sampler->probs pointer (the next call retries with the larger
 *    capacity instead of leaking). No use-after-free path.
 */

#include "human/providers/llamacpp_sampling.h"

#include "human/core/error.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* SplitMix64 — used to seed the four-lane Xoshiro256** state from a
 * single 64-bit seed without correlating any of the four lanes. */
static uint64_t splitmix64_next(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static uint64_t xoshiro256ss_next(uint64_t s[4]) {
    const uint64_t result = (((s[1] * 5ull) << 7) | ((s[1] * 5ull) >> 57)) * 9ull;
    const uint64_t t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t; s[3] = (s[3] << 45) | (s[3] >> 19);
    return result;
}

static double xoshiro256ss_unit(uint64_t s[4]) {
    /* Top 53 bits -> double mantissa, mapped into [0, 1). */
    return (double)(xoshiro256ss_next(s) >> 11) * (1.0 / 9007199254740992.0);
}

hu_error_t hu_llamacpp_sampler_init(hu_llamacpp_sampler_t *sampler,
                                    const hu_llamacpp_sampling_params_t *params) {
    if (!sampler || !params) return HU_ERR_INVALID_ARGUMENT;
    memset(sampler, 0, sizeof(*sampler));
    sampler->params = *params;
    uint64_t seed = params->seed;
    if (seed == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        seed = ((uint64_t)ts.tv_sec << 32) ^ (uint64_t)ts.tv_nsec;
    }
    uint64_t mix = seed;
    sampler->s[0] = splitmix64_next(&mix);
    sampler->s[1] = splitmix64_next(&mix);
    sampler->s[2] = splitmix64_next(&mix);
    sampler->s[3] = splitmix64_next(&mix);
    return HU_OK;
}

void hu_llamacpp_sampler_free(hu_llamacpp_sampler_t *sampler) {
    if (!sampler) return;
    free(sampler->probs);
    free(sampler->idx);
    sampler->probs = NULL;
    sampler->idx = NULL;
    sampler->buf_capacity = 0;
}

static __thread const float *s_cmp_logits_tls;

static int cmp_logit_desc(const void *a, const void *b) {
    int32_t ia = *(const int32_t *)a;
    int32_t ib = *(const int32_t *)b;
    if (s_cmp_logits_tls[ia] > s_cmp_logits_tls[ib]) return -1;
    if (s_cmp_logits_tls[ia] < s_cmp_logits_tls[ib]) return 1;
    return 0;
}

static int ensure_buffers(hu_llamacpp_sampler_t *sampler, size_t vocab_size) {
    if (sampler->buf_capacity >= vocab_size) return 0;
    float *new_probs = (float *)realloc(sampler->probs, sizeof(float) * vocab_size);
    if (!new_probs) return -1;
    sampler->probs = new_probs;
    int32_t *new_idx = (int32_t *)realloc(sampler->idx, sizeof(int32_t) * vocab_size);
    if (!new_idx) return -1;
    sampler->idx = new_idx;
    sampler->buf_capacity = vocab_size;
    return 0;
}

hu_error_t hu_llamacpp_sampler_pick(hu_llamacpp_sampler_t *sampler,
                                    const float *logits, size_t vocab_size,
                                    int32_t *out_token) {
    if (!sampler || !logits || vocab_size == 0 || !out_token)
        return HU_ERR_INVALID_ARGUMENT;
    if (ensure_buffers(sampler, vocab_size) != 0)
        return HU_ERR_OUT_OF_MEMORY;

    if (sampler->params.temperature == 0.0 || sampler->params.top_k == 1) {
        size_t best = 0;
        float best_v = logits[0];
        for (size_t i = 1; i < vocab_size; i++)
            if (logits[i] > best_v) { best_v = logits[i]; best = i; }
        *out_token = (int32_t)best;
        return HU_OK;
    }

    for (size_t i = 0; i < vocab_size; i++) sampler->idx[i] = (int32_t)i;
    if (vocab_size <= 64) {
        /* Insertion sort for tiny vocabularies (unit tests use V<=10).
         * The qsort path below is exercised by the V=128 coverage test. */
        for (size_t i = 1; i < vocab_size; i++)
            for (size_t j = i; j > 0 &&
                               logits[sampler->idx[j-1]] < logits[sampler->idx[j]]; j--) {
                int32_t tmp = sampler->idx[j-1];
                sampler->idx[j-1] = sampler->idx[j];
                sampler->idx[j] = tmp;
            }
    } else {
        s_cmp_logits_tls = logits;
        qsort(sampler->idx, vocab_size, sizeof(int32_t), cmp_logit_desc);
        s_cmp_logits_tls = NULL;
    }

    size_t k = vocab_size;
    if (sampler->params.top_k > 0 && (size_t)sampler->params.top_k < vocab_size)
        k = (size_t)sampler->params.top_k;

    double t = sampler->params.temperature;
    if (t <= 0.0) t = 1.0;
    double max_l = (double)logits[sampler->idx[0]];
    double sum = 0.0;
    for (size_t i = 0; i < k; i++) {
        double e = exp(((double)logits[sampler->idx[i]] - max_l) / t);
        sampler->probs[i] = (float)e;
        sum += e;
    }
    if (sum > 0.0)
        for (size_t i = 0; i < k; i++)
            sampler->probs[i] = (float)(sampler->probs[i] / sum);

    if (sampler->params.top_p < 1.0 && sampler->params.top_p > 0.0) {
        double cum = 0.0;
        size_t cutoff = k;
        for (size_t i = 0; i < k; i++) {
            cum += sampler->probs[i];
            if (cum >= sampler->params.top_p) { cutoff = i + 1; break; }
        }
        k = cutoff;
        double s2 = 0.0;
        for (size_t i = 0; i < k; i++) s2 += sampler->probs[i];
        if (s2 > 0.0)
            for (size_t i = 0; i < k; i++)
                sampler->probs[i] = (float)(sampler->probs[i] / s2);
    }

    if (sampler->params.min_p > 0.0) {
        float max_p = sampler->probs[0];
        size_t cutoff = k;
        for (size_t i = 0; i < k; i++)
            if (sampler->probs[i] < (float)((double)max_p * sampler->params.min_p)) {
                cutoff = i; break;
            }
        if (cutoff < 1) cutoff = 1;
        k = cutoff;
        double s2 = 0.0;
        for (size_t i = 0; i < k; i++) s2 += sampler->probs[i];
        if (s2 > 0.0)
            for (size_t i = 0; i < k; i++)
                sampler->probs[i] = (float)(sampler->probs[i] / s2);
    }

    double r = xoshiro256ss_unit(sampler->s);
    double cum = 0.0;
    for (size_t i = 0; i < k; i++) {
        cum += sampler->probs[i];
        if (r <= cum) { *out_token = sampler->idx[i]; return HU_OK; }
    }
    *out_token = sampler->idx[k - 1];
    return HU_OK;
}
