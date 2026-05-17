#ifndef HU_EVAL_JUDGE_EXTERNAL_H
#define HU_EVAL_JUDGE_EXTERNAL_H

/*
 * Phase 5 Task 3 — `hu_eval_judge_external_t` vtable + factories.
 *
 * Pairwise external-LLM judge bridge. The eval gate (Task 5) and the
 * competitive harness (Task 9) hand the judge two candidate responses
 * to the same prompt and ask "which is better?". Three production
 * factories are planned:
 *
 *   1. canned          — deterministic cycle through a caller-provided
 *                        verdict array. Used by tests and as the default
 *                        for CI runs (no network, no subprocess, no flake).
 *   2. apple_fm        — Swift bridge subprocess (Task 7).
 *   3. gemini_nano     — headless-Chrome bridge subprocess (Task 8).
 *
 * The vtable shape is stable across all three so the gate / harness
 * never branch on judge identity. Tasks 7+8 land their real impls
 * behind `#define HU_EVAL_JUDGE_HAVE_APPLE_FM_IMPL` /
 * `#define HU_EVAL_JUDGE_HAVE_GEMINI_NANO_IMPL` tokens; until then the
 * factories return `HU_ERR_NOT_SUPPORTED` cleanly (per Phase 4 critic
 * L3 — strict C11 conditional compilation, NOT __attribute__((weak))).
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pairwise input: one prompt, two candidate responses. */
typedef struct hu_eval_judge_pair {
    const char *prompt;
    const char *response_a; /* candidate A (e.g., trained adapter output) */
    const char *response_b; /* candidate B (e.g., baseline output) */
} hu_eval_judge_pair_t;

/* Verdict: which response wins, with confidence and an optional rationale.
 *
 * Memory contract: when emitted by the canned factory, `rationale` is
 * always either NULL or a pointer into the factory's owned deep-copied
 * verdict array — callers MUST NOT free it. When emitted by Task 7/8
 * factories, the same rule applies (the bridge owns the rationale
 * string for the lifetime of the judge). The deinit cleanup releases
 * everything in one shot. */
typedef struct hu_eval_judge_verdict {
    int prefer_a;            /* 1 = A wins, 0 = B wins, -1 = tie */
    double confidence;       /* 0.0 - 1.0 */
    const char *rationale;   /* may be NULL; do not free in caller */
} hu_eval_judge_verdict_t;

struct hu_eval_judge_external;

typedef struct hu_eval_judge_external_vtable {
    hu_error_t (*judge)(struct hu_eval_judge_external *self,
                        const hu_eval_judge_pair_t *pair,
                        hu_eval_judge_verdict_t *out);
    const char *(*name)(struct hu_eval_judge_external *self);
    void (*deinit)(struct hu_eval_judge_external *self);
} hu_eval_judge_external_vtable_t;

typedef struct hu_eval_judge_external {
    const hu_eval_judge_external_vtable_t *vtable;
    void *ctx;
} hu_eval_judge_external_t;

/* Canned-response factory — for tests and hermetic CI runs.
 *
 * The caller provides a pointer to an array of verdicts plus its length.
 * The factory deep-copies the array (and any non-NULL rationale strings)
 * so the caller's storage may be stack-local or freed immediately after
 * the create call. Successive judge() calls cycle deterministically:
 * `verdicts[i % n_verdicts]`, advancing `i` after each call. */
typedef struct hu_eval_judge_canned_config {
    const hu_eval_judge_verdict_t *verdicts;
    size_t n_verdicts;
} hu_eval_judge_canned_config_t;

hu_error_t hu_eval_judge_create_canned(hu_allocator_t *alloc,
                                       const hu_eval_judge_canned_config_t *cfg,
                                       hu_eval_judge_external_t *out);

/* Apple Foundation Models factory (Task 7 lands the real impl behind
 * `HU_EVAL_JUDGE_HAVE_APPLE_FM_IMPL`). Until then returns
 * `HU_ERR_NOT_SUPPORTED`. */
hu_error_t hu_eval_judge_create_apple_fm(hu_allocator_t *alloc,
                                         hu_eval_judge_external_t *out);

/* Gemini Nano factory (Task 8 lands the real impl behind
 * `HU_EVAL_JUDGE_HAVE_GEMINI_NANO_IMPL`). Until then returns
 * `HU_ERR_NOT_SUPPORTED`. */
hu_error_t hu_eval_judge_create_gemini_nano(hu_allocator_t *alloc,
                                            hu_eval_judge_external_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HU_EVAL_JUDGE_EXTERNAL_H */
