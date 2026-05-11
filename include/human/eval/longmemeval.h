#ifndef HU_EVAL_LONGMEMEVAL_H
#define HU_EVAL_LONGMEMEVAL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* B7 — LongMemEval-style offline scoring helper.
 *
 * Inputs: a candidate response string + a list of expected keywords +
 * (optional) a category. Output: a 0..100 score and an abstention flag.
 *
 * Scoring model (deterministic, no LLM):
 *
 *   - Keyword recall: fraction of expected keywords present
 *     (case-insensitive substring) in the response.
 *   - Category-specific adjustments:
 *       * `abstention`        — empty / "I don't know" responses score
 *                               full marks; otherwise zero (refuse to
 *                               speculate is the goal).
 *       * `temporal`/`single_hop`/`multi_hop` — keyword recall as-is.
 *   - Score = round(recall * 100), clamped to 0..100.
 *
 * The runner consumes the JSON pack at
 * `eval_suites/longmemeval/longmemeval.json`. */

typedef struct hu_longmemeval_score {
    int score;             /* 0..100 */
    bool abstained;        /* response was a deliberate "I don't know" */
    size_t keywords_seen;
    size_t keywords_total;
} hu_longmemeval_score_t;

/* Score a single item. `keywords` is a NULL-terminated list of expected
 * keyword strings. NULL/empty arguments are tolerated. */
hu_error_t hu_longmemeval_score_item(const char *category, const char *response,
                                     size_t response_len, const char *const *keywords,
                                     size_t keyword_count, hu_longmemeval_score_t *out);

/* Aggregate runner: load the JSON pack and score every item by passing the
 * `candidate_answer` field as the response (i.e. an offline self-test that
 * verifies the keyword extractor is well-calibrated against the golden
 * answers). Returns mean score and per-item passes. */
hu_error_t hu_longmemeval_run_pack_self_test(hu_allocator_t *alloc, const char *json_path,
                                             unsigned *out_total, unsigned *out_passed,
                                             int *out_mean_score);

#endif /* HU_EVAL_LONGMEMEVAL_H */
