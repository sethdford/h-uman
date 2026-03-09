/* A/B response evaluation: score multiple candidates and pick the best. */
#include "seaclaw/agent/ab_response.h"
#include "seaclaw/context/conversation.h"
#include <string.h>

sc_error_t sc_ab_evaluate(sc_allocator_t *alloc, sc_ab_result_t *result,
                          const sc_channel_history_entry_t *entries, size_t entry_count,
                          uint32_t max_chars) {
    if (!alloc || !result)
        return SC_ERR_INVALID_ARGUMENT;

    result->best_idx = 0;
    if (result->candidate_count == 0)
        return SC_OK;

    size_t n = result->candidate_count;
    if (n > SC_AB_MAX_CANDIDATES)
        n = SC_AB_MAX_CANDIDATES;
    for (size_t i = 0; i < n; i++) {
        sc_ab_candidate_t *c = &result->candidates[i];
        if (!c->response)
            continue;
        sc_quality_score_t score = sc_conversation_evaluate_quality(
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

    return SC_OK;
}

void sc_ab_result_deinit(sc_ab_result_t *result, sc_allocator_t *alloc) {
    if (!result || !alloc)
        return;
    for (size_t i = 0; i < result->candidate_count && i < SC_AB_MAX_CANDIDATES; i++) {
        sc_ab_candidate_t *c = &result->candidates[i];
        if (c->response) {
            alloc->free(alloc->ctx, c->response, c->response_len + 1);
            c->response = NULL;
            c->response_len = 0;
        }
    }
    result->candidate_count = 0;
    result->best_idx = 0;
}
