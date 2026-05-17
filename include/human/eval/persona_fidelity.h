#ifndef HU_EVAL_PERSONA_FIDELITY_H
#define HU_EVAL_PERSONA_FIDELITY_H

/* persona_fidelity — multi-turn composite fidelity scoring.
 *
 * Composes three existing primitives into one number that answers
 * "after N turns, did the agent sound like this persona?":
 *
 *   1. style match        — hu_communication_style_fidelity_score
 *                            (per-turn lowercase / abbreviation / length axes)
 *   2. trait coverage     — hu_consistency_score_prompt_alignment
 *                            (response references persona traits + preferred
 *                            vocab; penalizes avoided vocab)
 *   3. line consistency   — hu_consistency_score_line
 *                            (turn N agrees in style/length with turn N-1)
 *
 * The composite weighting is opinionated; see persona_fidelity.c for the
 * default constants and the rationale (and where to tune them).
 *
 * Two entry points:
 *
 *   - hu_persona_fidelity_score_l1(...)        single response set
 *   - hu_persona_fidelity_ab_score(...)        A/B over two response sets,
 *                                              with stderr-based improvement
 *                                              verdict for CI promotion gates
 *
 * Both are deterministic, free, and provider-free — safe to call on every
 * PR. The L2 LLM-judge wrapper below opts into a provider call against
 * docs/eval/fidelity_rubric.json for nightly / pre-release gates.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/eval_judge.h"
#include "human/memory/personal_model.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result of an L1 fidelity score over a single response set. */
typedef struct hu_persona_fidelity_score {
    /* Per-axis means across `turns_scored` turns. Each axis is [0, 1]. */
    float style_match_mean;
    float trait_coverage_mean;
    float line_consistency_mean;

    /* Composite, weighted average of the three axes (see weights in .c). */
    float composite;

    /* Spread diagnostics over `turns_scored`. */
    float composite_stderr; /* sqrt(var / n); 0 when n<=1 */
    float min_turn;         /* min per-turn composite; 0 when n==0 */
    float max_turn;         /* max per-turn composite; 0 when n==0 */

    /* Bookkeeping. */
    size_t turns_scored;
    size_t turns_skipped; /* NULL/empty responses */
} hu_persona_fidelity_score_t;

/* Score a multi-turn response set against a learned style + persona fingerprint.
 *
 * `target`             — style fingerprint (typically from hu_personal_model.style)
 * `responses[]`/`lens[]` — N assistant responses in conversation order
 * `traits[]`           — persona traits (e.g. "warm", "direct"); may be NULL
 * `preferred_vocab[]`  — persona's lexical preferences; may be NULL
 * `avoided_vocab[]`    — persona's anti-vocabulary; may be NULL
 * `out`                — populated on HU_OK
 *
 * Returns HU_ERR_INVALID_ARGUMENT on NULL `target`, NULL `out`,
 * `n == 0`, or `target->sample_count == 0` (no fingerprint to score against).
 *
 * A response is skipped when `responses[i] == NULL || lens[i] == 0`. If every
 * response is skipped, returns HU_OK with `turns_scored == 0` and all means
 * zeroed — callers should treat that as "no comparison possible". */
hu_error_t hu_persona_fidelity_score_l1(const hu_communication_style_t *target,
                                        const char *const *responses, const size_t *lens, size_t n,
                                        const char *const *traits, size_t traits_count,
                                        const char *const *preferred_vocab, size_t preferred_count,
                                        const char *const *avoided_vocab, size_t avoided_count,
                                        hu_persona_fidelity_score_t *out);

/* A/B verdict over two response sets (typically pre-LoRA vs post-LoRA,
 * or no-persona vs persona-equipped). */
typedef struct hu_persona_fidelity_ab {
    hu_persona_fidelity_score_t set_a;
    hu_persona_fidelity_score_t set_b;
    float delta;        /* set_b.composite - set_a.composite */
    float delta_stderr; /* sqrt(stderr_a^2 + stderr_b^2) */
    /* True when delta >= min_improvement_stderr * delta_stderr AND
     * both sets have turns_scored > 0. The default of 1.0 stderr is
     * intentionally lenient compared to the 1.96 (95% CI) statistical
     * convention — fidelity gates are an early-warning signal, not a
     * publication-quality claim. Tighten to 1.96 once the eval has
     * enough turns to stop being noisy. */
    bool improved;
} hu_persona_fidelity_ab_t;

hu_error_t hu_persona_fidelity_ab_score(const hu_communication_style_t *target,
                                        const char *const *responses_a, const size_t *lens_a,
                                        size_t n_a, const char *const *responses_b,
                                        const size_t *lens_b, size_t n_b, const char *const *traits,
                                        size_t traits_count, const char *const *preferred_vocab,
                                        size_t preferred_count, const char *const *avoided_vocab,
                                        size_t avoided_count, float min_improvement_stderr,
                                        hu_persona_fidelity_ab_t *out);

/* L2 — LLM-judge wrapper bound to docs/eval/fidelity_rubric.json.
 *
 * Issues a single judge call: "does `response` sound like the persona
 * described by `persona_description`?" Uses the provided rubric text
 * directly — the caller reads the rubric file (the canonical location is
 * "docs/eval/fidelity_rubric.json"). This keeps the eval library free of
 * filesystem assumptions and lets tests pass synthetic rubrics.
 *
 * Wraps `hu_eval_judge_check` with a stock question template + the supplied
 * rubric. Score, pass/fail, and reasoning land in `out`.
 *
 * Caller must hu_eval_judge_result_free(alloc, out) on HU_OK.
 *
 * Returns HU_ERR_INVALID_ARGUMENT on NULL `provider`, NULL `out`, or empty
 * `rubric_text`. Provider errors propagate from hu_eval_judge_check. */
hu_error_t hu_persona_fidelity_judge(hu_allocator_t *alloc, hu_provider_t *provider,
                                     const char *model, size_t model_len,
                                     const char *persona_description, size_t persona_desc_len,
                                     const char *response, size_t response_len,
                                     const char *rubric_text, size_t rubric_text_len,
                                     int pass_threshold, hu_eval_judge_cache_t *cache,
                                     hu_eval_judge_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HU_EVAL_PERSONA_FIDELITY_H */
