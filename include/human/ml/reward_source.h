/* include/human/ml/reward_source.h — Phase 4 Task 4 (RL SOTA)
 *
 * hu_reward_source_t — "score N completions for one prompt" leaf vtable.
 *
 * Why this lives in its own file (D-rationale): the GRPO trainer needs
 * to attach a scalar reward to each rollout sampled from hu_rollout_t,
 * but the reward source itself is a SEPARATE concern from GRPO's
 * policy/ref/KL machinery — three different production backends
 * (synthetic fn, Phase 3 hu_reward_model_t, Phase 5 llm-judge) want to
 * plug in via the same shape. A leaf vtable here keeps the GRPO trainer
 * blind to which backend it's calling and keeps the reward-source impls
 * independently testable.
 *
 * Three backends:
 *   HU_REWARD_SOURCE_SYNTHETIC — count "good" token IDs (1..5) minus
 *     "bad" (26..30) in each completion. Pure function, no allocations
 *     beyond the create-time ctx. Same convention as Phase 3 Task 3's
 *     make_synthetic_pairs and the umbrella plan §11 Q3 cold-start path
 *     (used when < 200 preference pairs are available to train a RM).
 *   HU_REWARD_SOURCE_RM       — composes a Phase 3 hu_reward_model_t
 *     (loaded from disk by CLI Task 9 via hu_reward_model_load). The
 *     reward source BORROWS the hu_reward_model_t pointer; the caller
 *     owns its lifecycle and must outlive this reward source. Per
 *     completion, the RM backend renders prompt+response token IDs as
 *     space-separated decimal strings (same convention as Phase 3 KTO
 *     Task 4) and calls rm->vtable->score().
 *   HU_REWARD_SOURCE_JUDGE    — Phase 5 territory; create_judge() stubs
 *     to HU_ERR_NOT_SUPPORTED until Phase 5 lands the real llm-judge
 *     impl (umbrella §10 R3 mitigation — multi-judge consensus).
 *
 * R9 reward-hacking pin (umbrella §10): there is no implicit default
 * for the reward source. CLI Task 9 (`human ml grpo-train`) MUST error
 * with HU_ERR_INVALID_ARGUMENT if neither --reward-fn nor --reward-model
 * is supplied. Picking the wrong source silently is the named risk;
 * explicit beats convenient.
 */
#ifndef HU_ML_REWARD_SOURCE_H
#define HU_ML_REWARD_SOURCE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/reward_model.h"
#include "human/ml/rollout.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HU_REWARD_SOURCE_SYNTHETIC = 1, /* count tokens 1..5 minus tokens 26..30 */
    HU_REWARD_SOURCE_RM        = 2, /* hu_reward_model_t.score_batch */
    HU_REWARD_SOURCE_JUDGE     = 3, /* Phase 5; stub returns NOT_SUPPORTED */
} hu_reward_source_kind_t;

struct hu_reward_source;

typedef struct hu_reward_source_vtable {
    /* Score n completions sampled for ONE prompt. out_rewards[i] receives
     * the scalar reward for completions[i]. The reward source may use
     * prompt_ids (RM backend, judge backend) or ignore them (synthetic).
     *
     * Returns:
     *   HU_OK                   on success — every out_rewards[i] written.
     *   HU_ERR_INVALID_ARGUMENT on NULL self / NULL out_rewards / NULL
     *                           completions when n > 0 / impl-specific
     *                           shape errors (e.g., RM rejects empty
     *                           completion text).
     *   HU_ERR_NOT_SUPPORTED    judge backend always; deferred to Phase 5.
     *   any error code returned by an underlying backend (RM score()).
     *
     * On error, out_rewards contents are unspecified. */
    hu_error_t (*score)(struct hu_reward_source *self,
                        const int32_t *prompt_ids, size_t prompt_len,
                        const hu_rollout_completion_t *completions, size_t n,
                        double *out_rewards);
    /* Stable backend name for logs/metrics. Never NULL after a successful
     * factory call. */
    const char *(*name)(struct hu_reward_source *self);
    /* Tear down the reward source. After deinit returns, `self->vtable`
     * is NULL and `self->ctx` is NULL — the struct is safe to reuse for
     * a fresh factory call. The RM backend does NOT free its borrowed
     * hu_reward_model_t. */
    void (*deinit)(struct hu_reward_source *self);
} hu_reward_source_vtable_t;

typedef struct hu_reward_source {
    const hu_reward_source_vtable_t *vtable;
    void *ctx;
} hu_reward_source_t;

/* Synthetic factory — score = (#tokens in [1..5]) - (#tokens in [26..30])
 * per completion. Pure function; no heap allocations needed beyond a
 * trivial (or zero-sized) ctx record. Used by:
 *   - tests/test_reward_source.c     — token-counting pin
 *   - tests/test_grpo.c              — HUML E2E rollout (Task 7)
 *   - cli_grpo.c                     — `--reward-fn synthetic` cold-start
 *
 * `alloc` is recorded for symmetric deinit; the synthetic backend never
 * allocates from it at score() time. */
hu_error_t hu_reward_source_create_synthetic(hu_allocator_t *alloc,
                                              hu_reward_source_t *out);

/* RM factory — wraps a Phase 3 hu_reward_model_t. The trainer BORROWS
 * `rm` (caller owns lifecycle and must outlive this reward source). The
 * reward source's score() renders each completion's prompt+token IDs as
 * a space-separated decimal string (mirrors Phase 3 KTO Task 4) and
 * forwards to rm->vtable->score().
 *
 * Returns HU_ERR_INVALID_ARGUMENT on NULL inputs or if `rm->vtable` /
 * `rm->vtable->score` is missing. */
hu_error_t hu_reward_source_create_rm(hu_allocator_t *alloc,
                                       hu_reward_model_t *rm,
                                       hu_reward_source_t *out);

/* Judge factory — Phase 5 placeholder. Always returns HU_ERR_NOT_SUPPORTED
 * in Phase 4; Phase 5 Task X will wire the real multi-judge consensus
 * impl per umbrella §10 R3. The symbol exists in Phase 4 so the GRPO
 * CLI can dispatch on `--reward-source judge` and surface a meaningful
 * "not yet" error rather than a link failure. */
hu_error_t hu_reward_source_create_judge(hu_allocator_t *alloc,
                                          hu_reward_source_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HU_ML_REWARD_SOURCE_H */
