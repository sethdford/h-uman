/* include/human/ml/rollout.h — Phase 4 Task 2 (RL SOTA)
 *
 * hu_rollout_t — sample N completions per prompt from a hu_model_t policy.
 *
 * Separate from hu_provider_t (D2 of the Phase 4 plan): providers expose a
 * text-in / text-out chat() contract. GRPO needs token IDs + per-completion
 * sum-log-prob (= π_θ_old(o_i | q) at sample time, used for the PPO ratio
 * clip ρ_i = exp(logπ_θ_now − sum_logprob)). hu_rollout_t is a lean leaf
 * vtable that exposes exactly those two outputs and nothing else.
 *
 * Backends (mirrors Phase 2/3):
 *   HUML — in-process toy GPT, multinomial sampling on temperature-scaled
 *          softmax. Cross-platform-deterministic via xorshift64 seeded from
 *          (caller_seed, rollout_index) pairs (R13 — libc rand() sequences
 *          differ across glibc/Apple libc on the same seed).
 *   MLX  — Apple-only subprocess delegate. Phase 4 Task 8 lands the impl;
 *          Task 2 declares the symbol returning HU_ERR_NOT_SUPPORTED.
 *
 * Caller-owned cleanup contract: sample() allocates each completion's
 * token_ids via the caller-supplied allocator; caller frees the whole
 * batch via hu_rollout_free_completions(alloc, completions, n). The
 * completions array itself is caller-owned (typically stack or heap by
 * the caller); only the token_ids buffers inside it are sample()-owned.
 */
#ifndef HU_ML_ROLLOUT_H
#define HU_ML_ROLLOUT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/model.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One sampled completion: the GENERATED suffix only (prompt is NOT echoed
 * back), plus the sum of log π_θ_old(o_t | o_<t, q) at sample time. The
 * sum_logprob field IS the π_θ_old log-prob the GRPO ratio clip needs —
 * no separate model snapshot required (D5 of the Phase 4 plan). */
typedef struct {
    int32_t *token_ids;     /* allocator-owned; freed by hu_rollout_free_completions */
    size_t   n_tokens;      /* number of generated tokens (0..max_new_tokens) */
    size_t   token_ids_cap; /* actual capacity in elements (for size-aware free) */
    double   sum_logprob;   /* Σ_t log π_θ(o_t | o_<t, q) accumulated at sample time */
} hu_rollout_completion_t;

/* Vtable: one operation (sample N completions for one prompt) plus the
 * standard name/deinit pair. NOT thread-safe (the underlying model.forward
 * is not thread-safe — it scribbles in shared activation buffers). */
typedef struct hu_rollout_vtable {
    /* Sample n_rollouts completions for ONE prompt. Each entry of
     * out_completions[0..n_rollouts) is filled with allocator-owned
     * token_ids (caller frees via hu_rollout_free_completions).
     *
     * Stops each rollout at: (a) sampled token == 0 (EOS by toy-GPT
     * convention; the EOS token is included in token_ids and counted
     * in n_tokens — its log-prob is part of the policy's decision and
     * is included in sum_logprob), or (b) max_new_tokens reached.
     *
     * On error mid-batch the impl frees any partially-populated
     * completions and returns the error code; caller must NOT call
     * hu_rollout_free_completions on a failed batch. */
    hu_error_t (*sample)(void *ctx, hu_allocator_t *alloc,
                         const int32_t *prompt, size_t prompt_len,
                         size_t n_rollouts, size_t max_new_tokens,
                         double temperature,
                         hu_rollout_completion_t *out_completions);
    const char *(*name)(void *ctx);
    void        (*deinit)(void *ctx, hu_allocator_t *alloc);
} hu_rollout_vtable_t;

typedef struct {
    void *ctx;
    const hu_rollout_vtable_t *vtable;
} hu_rollout_t;

/* HUML factory: wraps a borrowed hu_model_t. Caller owns the model's
 * lifetime; rollout deinit() does NOT touch the model.
 *
 * `seed` is the create-time base seed. Each rollout in a sample() batch
 * derives its independent PRNG state from splitmix64(seed XOR rollout_index)
 * — guarantees both reproducibility (same seed + same N → byte-identical
 * batch) AND independence between rollouts within one batch (distinct
 * rollouts use uncorrelated streams). */
hu_error_t hu_rollout_create_huml(hu_allocator_t *alloc, hu_model_t *model,
                                   uint64_t seed, hu_rollout_t *out);

/* MLX factory: Apple-only subprocess delegate to scripts/grpo_mlx_train.py
 * sampling mode. Phase 4 Task 8 lands the impl; Task 2 returns
 * HU_ERR_NOT_SUPPORTED so the symbol exists for the GRPO trainer's
 * factory-dispatch path (Task 5) without breaking link. */
hu_error_t hu_rollout_create_mlx(hu_allocator_t *alloc, const char *model_id,
                                  uint64_t seed, hu_rollout_t *out);

/* Free token_ids inside each completion (does NOT free the completions
 * array itself — caller owns that). Safe to call on partially-populated
 * arrays: skips entries where token_ids == NULL. */
void hu_rollout_free_completions(hu_allocator_t *alloc,
                                  hu_rollout_completion_t *completions,
                                  size_t n);

#ifdef __cplusplus
}
#endif
#endif /* HU_ML_ROLLOUT_H */
