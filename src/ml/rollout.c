/* src/ml/rollout.c — Phase 4 Task 2 (RL SOTA)
 *
 * HUML rollout: multinomial sampling on a hu_model_t (toy GPT) with a
 * cross-platform-deterministic xorshift64 PRNG (R13).
 *
 * The MLX factory stub returns HU_ERR_NOT_SUPPORTED here; Task 8 lands
 * the real subprocess delegate.
 *
 * Determinism contract (R13): each rollout's PRNG state is derived from
 * splitmix64(seed XOR rollout_index) at sample() time, so same seed +
 * same N produce byte-identical batches across glibc / Apple libc /
 * any other platform — we never touch libc rand() / rand_r() (both of
 * which produce platform-divergent sequences from the same seed).
 *
 * This is GRPO scaffolding: the trainer (Task 5) calls sample() once
 * per prompt per training step and reads back token_ids + sum_logprob.
 */
#include "human/ml/rollout.h"
#include "human/core/error.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────────────
 *  Cross-platform PRNG (xorshift64 + splitmix64 seed mixer) — R13.
 * ────────────────────────────────────────────────────────────────────── */

/* SplitMix64 — derives a per-rollout seed from (caller_seed, rollout_index).
 * Reference: Vigna 2014, "Further scramblings of Marsaglia's xorshift
 * generators". Used by JDK's SplittableRandom. Same constants on every
 * platform → identical streams. */
static uint64_t splitmix64(uint64_t z) {
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

/* xorshift64 — Marsaglia 2003, period 2^64 - 1. Cheap, deterministic,
 * adequate for multinomial sampling in a unit test. */
static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    /* The all-zero state is degenerate; splitmix64() of any input is
     * non-zero except for the single fixed point splitmix64(0)==0, which
     * we already guard against in hu_rollout_create_huml. */
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

/* Uniform double in [0, 1) from xorshift64's high-quality high bits. */
static double next_uniform(uint64_t *state) {
    /* Take the top 53 bits (the mantissa width of an IEEE 754 double)
     * and divide by 2^53. Reference: Marsaglia 2003 §"FLOAT". */
    return (double)(xorshift64(state) >> 11) / (double)(UINT64_C(1) << 53);
}

/* ──────────────────────────────────────────────────────────────────────
 *  Sampling primitives.
 * ────────────────────────────────────────────────────────────────────── */

/* Apply temperature-scaled softmax: probs[i] = exp((logits[i] - max)/T) / Z.
 * Defensive: T < 1e-8 falls back to T = 1.0 (no scaling) to avoid divide-
 * by-zero. The max-subtract is the standard log-sum-exp stabilizer. */
static void temperature_softmax(const double *logits, size_t v, double temp,
                                double *out_probs) {
    if (temp < 1e-8) temp = 1.0;
    double max_l = logits[0];
    for (size_t i = 1; i < v; i++)
        if (logits[i] > max_l) max_l = logits[i];

    double sum = 0.0;
    for (size_t i = 0; i < v; i++) {
        out_probs[i] = exp((logits[i] - max_l) / temp);
        sum += out_probs[i];
    }
    if (sum < 1e-30) sum = 1e-30;
    for (size_t i = 0; i < v; i++) out_probs[i] /= sum;
}

/* Multinomial sample via inverse-CDF on a normalized prob vector.
 * Numerical floor: if cumulative sum doesn't quite reach u (rounding),
 * return v-1. */
static size_t multinomial_sample(uint64_t *state, const double *probs, size_t v) {
    double u = next_uniform(state);
    double cum = 0.0;
    for (size_t i = 0; i < v; i++) {
        cum += probs[i];
        if (u < cum) return i;
    }
    return v - 1;
}

/* ──────────────────────────────────────────────────────────────────────
 *  HUML backend.
 * ────────────────────────────────────────────────────────────────────── */

typedef struct {
    hu_model_t *policy_ref; /* borrowed; deinit does NOT touch this */
    uint64_t    base_seed;  /* mixed with rollout index per sample() call */
} rollout_huml_ctx_t;

/* Free token_ids buffers for the first n_done completions (used on
 * mid-batch failure to avoid leaks). */
static void free_partial(hu_allocator_t *alloc,
                         hu_rollout_completion_t *out, size_t n_done) {
    for (size_t k = 0; k < n_done; k++) {
        if (out[k].token_ids) {
            alloc->free(alloc->ctx, out[k].token_ids,
                        out[k].token_ids_cap * sizeof(int32_t));
            out[k].token_ids = NULL;
            out[k].n_tokens = 0;
            out[k].token_ids_cap = 0;
        }
    }
}

/* Sample one rollout into out[i]. Owns the per-rollout `seq` working
 * buffer and the output `gen` buffer; cleans both up on its own error
 * paths and bubbles the error code back to the batch loop. */
static hu_error_t sample_one_rollout(hu_allocator_t *alloc, hu_model_t *model,
                                     const int32_t *prompt, size_t prompt_len,
                                     size_t max_new_tokens, double temperature,
                                     uint64_t state,
                                     hu_rollout_completion_t *out) {
    /* Working buffer: holds [prompt | generated_so_far] for each forward
     * call. Size = prompt_len + max_new_tokens (max possible). */
    size_t seq_cap = prompt_len + max_new_tokens;
    int32_t *seq = (int32_t *)alloc->alloc(alloc->ctx, seq_cap * sizeof(int32_t));
    if (!seq) return HU_ERR_OUT_OF_MEMORY;
    memcpy(seq, prompt, prompt_len * sizeof(int32_t));
    size_t n = prompt_len;
    size_t gen_n = 0;
    double sum_lp = 0.0;
    double *probs = NULL; /* allocated lazily once we know V from forward output */
    size_t  probs_cap = 0;

    for (size_t step = 0; step < max_new_tokens; step++) {
        hu_ml_tensor_t input = {
            .data = seq,
            .shape = {1, n, 0, 0},
            .ndim = 2,
            .dtype = HU_ML_DTYPE_I32,
            .size_bytes = n * sizeof(int32_t),
        };
        hu_ml_tensor_t output = {0};
        hu_error_t fe = model->vtable->forward(model->ctx, &input, &output);
        if (fe != HU_OK) {
            if (probs) alloc->free(alloc->ctx, probs, probs_cap * sizeof(double));
            alloc->free(alloc->ctx, seq, seq_cap * sizeof(int32_t));
            return fe;
        }

        /* output.data is float[1, n, V]; we want logits at the last
         * position to predict the next token. (Same contract as
         * src/ml/policy_logprobs.c uses.) */
        size_t V = output.shape[2];
        if (V == 0) {
            alloc->free(alloc->ctx, output.data, output.size_bytes);
            if (probs) alloc->free(alloc->ctx, probs, probs_cap * sizeof(double));
            alloc->free(alloc->ctx, seq, seq_cap * sizeof(int32_t));
            return HU_ERR_PROVIDER_RESPONSE;
        }
        if (V > probs_cap) {
            if (probs) alloc->free(alloc->ctx, probs, probs_cap * sizeof(double));
            probs = (double *)alloc->alloc(alloc->ctx, V * sizeof(double));
            if (!probs) {
                alloc->free(alloc->ctx, output.data, output.size_bytes);
                alloc->free(alloc->ctx, seq, seq_cap * sizeof(int32_t));
                return HU_ERR_OUT_OF_MEMORY;
            }
            probs_cap = V;
        }

        const float *logits_f = (const float *)output.data;
        const float *last_pos = logits_f + (n - 1) * V;
        double *logits_d = probs; /* reuse buffer: write logits, then overwrite with probs */
        for (size_t i = 0; i < V; i++) logits_d[i] = (double)last_pos[i];
        temperature_softmax(logits_d, V, temperature, probs);

        size_t tok = multinomial_sample(&state, probs, V);
        if (tok >= V) tok = V - 1; /* belt + braces */
        seq[n++] = (int32_t)tok;
        gen_n++;
        if (probs[tok] > 1e-300) sum_lp += log(probs[tok]);

        alloc->free(alloc->ctx, output.data, output.size_bytes);

        /* EOS: token id 0 is the toy-GPT end-of-sequence convention.
         * The EOS token IS counted in n_tokens and its log-prob IS
         * accumulated — it's part of the policy's decision sequence. */
        if (tok == 0) break;
    }
    if (probs) alloc->free(alloc->ctx, probs, probs_cap * sizeof(double));

    /* Copy generated suffix into a tight buffer owned by the caller-
     * facing completions array. We always allocate at least 1 element
     * even when gen_n == 0 to avoid 0-byte allocations and to keep
     * token_ids non-NULL as a defensive contract. */
    size_t gen_cap = gen_n > 0 ? gen_n : 1;
    int32_t *gen = (int32_t *)alloc->alloc(alloc->ctx, gen_cap * sizeof(int32_t));
    if (!gen) {
        alloc->free(alloc->ctx, seq, seq_cap * sizeof(int32_t));
        return HU_ERR_OUT_OF_MEMORY;
    }
    if (gen_n > 0) memcpy(gen, seq + prompt_len, gen_n * sizeof(int32_t));
    out->token_ids = gen;
    out->n_tokens = gen_n;
    out->token_ids_cap = gen_cap;
    out->sum_logprob = sum_lp;

    alloc->free(alloc->ctx, seq, seq_cap * sizeof(int32_t));
    return HU_OK;
}

static hu_error_t rollout_huml_sample(void *vctx, hu_allocator_t *alloc,
                                      const int32_t *prompt, size_t prompt_len,
                                      size_t n_rollouts, size_t max_new_tokens,
                                      double temperature,
                                      hu_rollout_completion_t *out) {
    if (!vctx || !alloc || !prompt || prompt_len == 0 || n_rollouts == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;
    rollout_huml_ctx_t *c = (rollout_huml_ctx_t *)vctx;
    if (!c->policy_ref || !c->policy_ref->vtable || !c->policy_ref->vtable->forward)
        return HU_ERR_INVALID_ARGUMENT;

    /* Pre-zero the output array — defensive even if the caller already
     * memset it, so free_partial / hu_rollout_free_completions never
     * see uninitialized token_ids pointers. */
    for (size_t i = 0; i < n_rollouts; i++) {
        out[i].token_ids = NULL;
        out[i].n_tokens = 0;
        out[i].token_ids_cap = 0;
        out[i].sum_logprob = 0.0;
    }

    for (size_t i = 0; i < n_rollouts; i++) {
        /* Per-rollout PRNG state derived from (base_seed, i). splitmix64
         * is the standard mixer for cases like this — uncorrelated
         * streams across rollout indices, byte-identical batch for
         * the same (base_seed, n_rollouts) pair. */
        uint64_t state = splitmix64(c->base_seed ^ (uint64_t)i);
        if (state == 0) state = UINT64_C(0xCAFEBABE); /* xorshift64 forbids 0 */

        hu_error_t err = sample_one_rollout(alloc, c->policy_ref, prompt, prompt_len,
                                            max_new_tokens, temperature, state, &out[i]);
        if (err != HU_OK) {
            free_partial(alloc, out, i); /* free [0..i); out[i] never populated */
            return err;
        }
    }
    return HU_OK;
}

static const char *rollout_huml_name(void *vctx) {
    (void)vctx;
    return "rollout_huml";
}

static void rollout_huml_deinit(void *vctx, hu_allocator_t *alloc) {
    if (!vctx || !alloc) return;
    alloc->free(alloc->ctx, vctx, sizeof(rollout_huml_ctx_t));
}

static const hu_rollout_vtable_t rollout_huml_vtable = {
    .sample = rollout_huml_sample,
    .name = rollout_huml_name,
    .deinit = rollout_huml_deinit,
};

/* ──────────────────────────────────────────────────────────────────────
 *  Public factories + cleanup.
 * ────────────────────────────────────────────────────────────────────── */

hu_error_t hu_rollout_create_huml(hu_allocator_t *alloc, hu_model_t *model,
                                   uint64_t seed, hu_rollout_t *out) {
    if (!alloc || !model || !out) return HU_ERR_INVALID_ARGUMENT;
    if (!model->vtable || !model->vtable->forward) return HU_ERR_INVALID_ARGUMENT;

    rollout_huml_ctx_t *c = (rollout_huml_ctx_t *)alloc->alloc(
        alloc->ctx, sizeof(rollout_huml_ctx_t));
    if (!c) return HU_ERR_OUT_OF_MEMORY;
    c->policy_ref = model;
    /* base_seed of 0 is allowed at the API surface; splitmix64(0) returns
     * 0 which xorshift64 cannot consume, but the per-rollout fallback in
     * rollout_huml_sample handles that case. */
    c->base_seed = seed;

    out->ctx = c;
    out->vtable = &rollout_huml_vtable;
    return HU_OK;
}

hu_error_t hu_rollout_create_mlx(hu_allocator_t *alloc, const char *model_id,
                                  uint64_t seed, hu_rollout_t *out) {
    /* Phase 4 Task 8 lands the real subprocess delegate. Until then the
     * factory exists so the GRPO trainer's MLX dispatch path (Task 5)
     * compiles and links cleanly. */
    (void)alloc;
    (void)model_id;
    (void)seed;
    (void)out;
    return HU_ERR_NOT_SUPPORTED;
}

void hu_rollout_free_completions(hu_allocator_t *alloc,
                                  hu_rollout_completion_t *completions,
                                  size_t n) {
    if (!alloc || !completions) return;
    for (size_t i = 0; i < n; i++) {
        if (completions[i].token_ids) {
            alloc->free(alloc->ctx, completions[i].token_ids,
                        completions[i].token_ids_cap * sizeof(int32_t));
            completions[i].token_ids = NULL;
            completions[i].n_tokens = 0;
            completions[i].token_ids_cap = 0;
        }
    }
}
