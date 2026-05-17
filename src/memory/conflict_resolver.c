#include "human/memory/conflict_resolver.h"
#include "human/memory/belief.h"

#ifdef HU_ENABLE_SQLITE
#include "human/memory/sql_transaction.h"
#include <sqlite3.h>
#endif

#include <stddef.h>

/* Single-valued relation types: only one can be "currently true" for a given
 * source. New observations supersede the prior open one rather than coexist.
 * Multi-valued relations (KNOWS, INTERESTED_IN, etc.) branch instead. */
bool hu_conflict_relation_is_single_valued(hu_relation_type_t type) {
    switch (type) {
        case HU_REL_WORKS_AT:
        case HU_REL_LIVES_IN:
            return true;
        case HU_REL_KNOWS:
        case HU_REL_FAMILY_OF:
        case HU_REL_INTERESTED_IN:
        case HU_REL_DISCUSSED_WITH:
        case HU_REL_FEELS_ABOUT:
        case HU_REL_PROMISED_TO:
        case HU_REL_SHARED_EXPERIENCE:
        case HU_REL_RELATED_TO:
            return false;
    }
    return false;
}

hu_conflict_resolution_t hu_conflict_classify(const hu_graph_relation_t *proposed,
                                              const hu_graph_relation_t *existing) {
    if (!proposed)
        return HU_CONFLICT_NONE;

    /* No prior row of this shape -> trivial NONE. */
    if (!existing || existing->id == 0)
        return HU_CONFLICT_NONE;

    /* Low-confidence new fact contradicting a high-confidence existing one is
     * suspicious. Don't drop it (could still be true), but flag for review. */
    if (proposed->confidence < 0.5f && existing->confidence >= 0.8f)
        return HU_CONFLICT_FLAG;

    /* Single-valued type AND existing is still open ("currently true") AND
     * proposed names a different target -> supersession. */
    if (hu_conflict_relation_is_single_valued(proposed->type) && existing->event_end == 0 &&
        existing->target_id != proposed->target_id) {
        return HU_CONFLICT_SUPERSEDE;
    }

    /* Multi-valued type or no contradiction -> branch (both kept). */
    return HU_CONFLICT_BRANCH;
}

const char *hu_conflict_resolution_str(hu_conflict_resolution_t r) {
    switch (r) {
        case HU_CONFLICT_NONE:      return "NONE";
        case HU_CONFLICT_SUPERSEDE: return "SUPERSEDE";
        case HU_CONFLICT_BRANCH:    return "BRANCH";
        case HU_CONFLICT_FLAG:      return "FLAG";
    }
    return "UNKNOWN";
}

/* W8 Phase 5 — semantic-judge fallback.
 *
 * Pure helper, no DB I/O. Walks `candidates` in order, runs
 * `hu_belief_semantic_conflict` on each candidate's context vs the
 * proposed context. Returns the first match.
 *
 * Why first-match instead of "best score": the candidate set is already
 * filtered upstream (the caller broadens the peek SQL to "open
 * relations on (contact_id, source_id)" and orders by recency). The
 * most recently observed paraphrase is the right thing to supersede.
 *
 * Skips candidates with empty context — there's nothing to compare. */
hu_conflict_resolution_t hu_conflict_classify_semantic(
    const hu_graph_relation_t *proposed,
    const hu_graph_relation_t *candidates,
    size_t n_candidates,
    int64_t *out_matched_existing_id) {
    if (out_matched_existing_id)
        *out_matched_existing_id = 0;
    if (!proposed || !candidates || n_candidates == 0)
        return HU_CONFLICT_NONE;
    if (!proposed->context || proposed->context_len == 0)
        return HU_CONFLICT_NONE;

    for (size_t i = 0; i < n_candidates; i++) {
        const hu_graph_relation_t *ex = &candidates[i];
        if (!ex->context || ex->context_len == 0 || ex->id <= 0)
            continue;
        hu_belief_conflict_t verdict = hu_belief_semantic_conflict(
            proposed->context, proposed->context_len,
            ex->context, ex->context_len);
        if (verdict == HU_BELIEF_CONFLICT_PARAPHRASE) {
            if (out_matched_existing_id)
                *out_matched_existing_id = ex->id;
            return HU_CONFLICT_SUPERSEDE;
        }
        if (verdict == HU_BELIEF_CONFLICT_CONTRADICT) {
            if (out_matched_existing_id)
                *out_matched_existing_id = ex->id;
            return HU_CONFLICT_FLAG;
        }
    }
    return HU_CONFLICT_NONE;
}

#ifdef HU_ENABLE_SQLITE

/* graph.c exposes a tiny accessor so we don't drag the full struct here. */

hu_error_t hu_conflict_apply(hu_graph_t *g, hu_conflict_resolution_t decision,
                             int64_t proposed_id, int64_t existing_id, int64_t cutover_ts) {
    if (!g)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = hu_graph_sqlite_connection(g);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;

    if (decision != HU_CONFLICT_SUPERSEDE)
        return HU_OK;

    if (proposed_id <= 0 || existing_id <= 0 || existing_id == proposed_id)
        return HU_ERR_INVALID_ARGUMENT;

    hu_sql_txn_t txn = {0};
    if (hu_sql_txn_begin(&txn, db) != HU_OK)
        return HU_ERR_IO;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db, "UPDATE relations SET event_end = ? WHERE id = ? AND event_end = 0", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_IO;
    }
    sqlite3_bind_int64(stmt, 1, cutover_ts);
    sqlite3_bind_int64(stmt, 2, existing_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_IO;
    }

    stmt = NULL;
    rc = sqlite3_prepare_v2(db, "UPDATE relations SET supersedes_id = ? WHERE id = ?", -1, &stmt,
                            NULL);
    if (rc != SQLITE_OK) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_IO;
    }
    sqlite3_bind_int64(stmt, 1, existing_id);
    sqlite3_bind_int64(stmt, 2, proposed_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_IO;
    }

    return hu_sql_txn_commit(&txn);
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t hu_conflict_apply(hu_graph_t *g, hu_conflict_resolution_t decision,
                             int64_t proposed_id, int64_t existing_id, int64_t cutover_ts) {
    (void)g;
    (void)decision;
    (void)proposed_id;
    (void)existing_id;
    (void)cutover_ts;
    return HU_ERR_NOT_SUPPORTED;
}

#endif
