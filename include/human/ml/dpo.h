#ifndef HU_ML_DPO_H
#define HU_ML_DPO_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

typedef struct hu_preference_pair {
    char prompt[2048];
    size_t prompt_len;
    char chosen[4096];
    size_t chosen_len;
    char rejected[4096];
    size_t rejected_len;
    double margin;
    int64_t timestamp;
    char source[64];
    size_t source_len;
} hu_preference_pair_t;

typedef struct hu_dpo_collector {
    hu_allocator_t *alloc;
#ifdef HU_ENABLE_SQLITE
    sqlite3 *db;
#else
    void *db;
#endif
    size_t pair_count;
    size_t max_pairs;
} hu_dpo_collector_t;

hu_error_t hu_dpo_collector_create(hu_allocator_t *alloc,
#ifdef HU_ENABLE_SQLITE
                                   sqlite3 *db,
#else
                                   void *db,
#endif
                                   size_t max_pairs, hu_dpo_collector_t *out);
void hu_dpo_collector_deinit(hu_dpo_collector_t *collector);
hu_error_t hu_dpo_init_tables(hu_dpo_collector_t *collector);

hu_error_t hu_dpo_record_pair(hu_dpo_collector_t *collector,
                              const hu_preference_pair_t *pair);

hu_error_t hu_dpo_record_from_feedback(hu_dpo_collector_t *collector,
                                       const char *prompt, size_t prompt_len,
                                       const char *response, size_t response_len,
                                       bool positive);

hu_error_t hu_dpo_record_from_retry(hu_dpo_collector_t *collector,
                                    const char *prompt, size_t prompt_len,
                                    const char *rejected, size_t rejected_len,
                                    const char *chosen, size_t chosen_len);

hu_error_t hu_dpo_export_jsonl(hu_dpo_collector_t *collector,
                               const char *path, size_t path_len,
                               size_t *exported_count);

hu_error_t hu_dpo_pair_count(hu_dpo_collector_t *collector, size_t *out);
hu_error_t hu_dpo_clear(hu_dpo_collector_t *collector);

typedef struct hu_dpo_export {
    hu_preference_pair_t *pairs;
    size_t count;
} hu_dpo_export_t;

void hu_dpo_export_free(hu_allocator_t *alloc, hu_dpo_export_t *export_data);

/* Build a prompt fragment from top-margin preference pairs (few-shot injection). */
hu_error_t hu_dpo_get_best_examples(hu_dpo_collector_t *collector, hu_allocator_t *alloc,
                                    size_t max_examples, char **out_prompt_fragment,
                                    size_t *out_len);

/* Result of a single judge-scored preference-pair batch.
 *
 * Phase 0 rename: the canonical name is `hu_dpo_judge_result_t`. The
 * legacy alias `hu_dpo_train_result_t` is preserved as a deprecated
 * typedef so existing code keeps compiling. The struct shape is
 * identical — only the name changed. See `hu_dpo_judge_step` below
 * for why the rename matters. */
typedef struct hu_dpo_judge_result {
    double loss;              /* Average aggregated judge loss across the batch */
    double alignment_score;   /* Fraction of pairs where chosen > rejected (0.0-1.0) */
    size_t pairs_evaluated;   /* Number of pairs processed */
    size_t pairs_aligned;     /* Number the judge ranked chosen > rejected */
} hu_dpo_judge_result_t;

/* Run one judge-scored preference-pair evaluation pass.
 *
 * NOT real DPO. There is no policy log-prob, no reference-model
 * log-prob, no gradient on policy weights. This function asks an
 * external LLM (the "judge") to score each preference pair and
 * aggregates the scores into a synthetic loss for reporting.
 * Real DPO with policy gradients lands in Phase 2 as
 * `hu_dpo_real_step` (see docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md
 * §1.5.2 issue #5). Until then, the name `_judge_step` keeps the
 * M3 narrative honest; the legacy `_train_step` is preserved as a
 * deprecated inline shim below.
 *
 * `beta`: judge-loss temperature (typically 0.1–0.5). `batch_size`: max
 * pairs per step (0 = all). Requires HU_ENABLE_SQLITE. Without it,
 * returns HU_ERR_NOT_SUPPORTED. */
hu_error_t hu_dpo_judge_step(hu_dpo_collector_t *collector, hu_allocator_t *alloc,
                             hu_provider_t *provider, const char *model, size_t model_len,
                             double beta, size_t batch_size,
                             hu_dpo_judge_result_t *out);

/* Deprecated: renamed to `hu_dpo_judge_result_t` in Phase 0. The struct
 * shape is unchanged; this typedef preserves source compatibility for
 * out-of-tree callers. */
typedef hu_dpo_judge_result_t hu_dpo_train_result_t;

/* Deprecated: renamed to `hu_dpo_judge_step` in Phase 0. The shim
 * forwards every argument verbatim so the result is bit-identical to
 * a direct call (pinned by tests/test_dpo_judge_naming.c). */
__attribute__((deprecated("renamed to hu_dpo_judge_step in Phase 0; "
                          "this is a cloud-LLM judge step, not policy-gradient "
                          "DPO. Real DPO is hu_dpo_real_step in Phase 2.")))
static inline hu_error_t hu_dpo_train_step(hu_dpo_collector_t *collector,
                                           hu_allocator_t *alloc,
                                           hu_provider_t *provider,
                                           const char *model, size_t model_len,
                                           double beta, size_t batch_size,
                                           hu_dpo_train_result_t *out) {
    return hu_dpo_judge_step(collector, alloc, provider, model, model_len,
                             beta, batch_size, out);
}

#endif /* HU_ML_DPO_H */
