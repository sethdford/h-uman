#ifndef HU_AB_RESPONSE_H
#define HU_AB_RESPONSE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/channel.h"
#include <stdbool.h>
#include <stddef.h>

#define HU_AB_MAX_CANDIDATES 3

typedef struct hu_ab_candidate {
    char *response;
    size_t response_len;
    int quality_score;
    bool needs_revision;
} hu_ab_candidate_t;

typedef struct hu_ab_result {
    hu_ab_candidate_t candidates[HU_AB_MAX_CANDIDATES];
    size_t candidate_count;
    size_t best_idx;
} hu_ab_result_t;

/* Score multiple pre-generated response candidates and pick the best.
 * Does NOT generate responses — just evaluates and selects.
 * Caller provides the candidates (response + response_len), function fills in scores.
 * Returns HU_OK and sets best_idx to the highest-scoring candidate. */
hu_error_t hu_ab_evaluate(hu_allocator_t *alloc, hu_ab_result_t *result,
                        const hu_channel_history_entry_t *entries, size_t entry_count,
                        uint32_t max_chars);

void hu_ab_result_deinit(hu_ab_result_t *result, hu_allocator_t *alloc);

#endif
