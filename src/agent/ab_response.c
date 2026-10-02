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
        hu_quality_score_t score = hu_conversation_evaluate_quality_capped(
            c->response, c->response_len, entries, entry_count, max_chars, result->cap_from_stats);
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

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

hu_error_t hu_ab_record_selection(sqlite3 *db, size_t best_idx, int quality_score,
                                  size_t candidate_count, int64_t timestamp) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    const char *sql = "CREATE TABLE IF NOT EXISTS ab_selections "
                      "(id INTEGER PRIMARY KEY, best_idx INTEGER, quality_score INTEGER, "
                      "candidate_count INTEGER, timestamp INTEGER)";
    (void)sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_stmt *stmt = NULL;
    const char *ins =
        "INSERT INTO ab_selections (best_idx, quality_score, candidate_count, timestamp) "
        "VALUES (?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db, ins, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int(stmt, 1, (int)best_idx);
    sqlite3_bind_int(stmt, 2, quality_score);
    sqlite3_bind_int(stmt, 3, (int)candidate_count);
    sqlite3_bind_int64(stmt, 4, timestamp);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_IO;
}
#endif
