/* A/B response evaluation: score multiple candidates and pick the best. */
#include "human/agent/ab_response.h"
#include "human/context/conversation.h"
#include <string.h>

hu_error_t hu_ab_evaluate(hu_allocator_t *alloc, hu_ab_result_t *result,
                          const hu_channel_history_entry_t *entries, size_t entry_count,
                          uint32_t max_chars) {
    if (!alloc || !result)
        return HU_ERR_INVALID_ARGUMENT;

    result->best_idx = 0;
    if (result->candidate_count == 0)
        return HU_OK;

    size_t n = result->candidate_count;
    if (n > HU_AB_MAX_CANDIDATES)
        n = HU_AB_MAX_CANDIDATES;
    for (size_t i = 0; i < n; i++) {
        hu_ab_candidate_t *c = &result->candidates[i];
        if (!c->response)
            continue;
        hu_quality_score_t score = hu_conversation_evaluate_quality(
            c->response, c->response_len, entries, entry_count, max_chars);
        c->quality_score = score.total;
        c->needs_revision = score.needs_revision;
    }

    /* Find highest-scoring candidate */
    int best_score = -1;
    for (size_t i = 0; i < n; i++) {
        if (result->candidates[i].quality_score > best_score) {
            best_score = result->candidates[i].quality_score;
            result->best_idx = i;
        }
    }

    return HU_OK;
}

void hu_ab_result_deinit(hu_ab_result_t *result, hu_allocator_t *alloc) {
    if (!result || !alloc)
        return;
    for (size_t i = 0; i < result->candidate_count && i < HU_AB_MAX_CANDIDATES; i++) {
        hu_ab_candidate_t *c = &result->candidates[i];
        if (c->response) {
            alloc->free(alloc->ctx, c->response, c->response_len + 1);
            c->response = NULL;
            c->response_len = 0;
        }
    }
    result->candidate_count = 0;
    result->best_idx = 0;
}
