#ifndef HU_MEMORY_CONFLICT_RESOLVER_H
#define HU_MEMORY_CONFLICT_RESOLVER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/graph.h"
#include <stdbool.h>
#include <stdint.h>

/* W1 — Write-time conflict resolver.
 *
 * Runs synchronously on every hu_graph_upsert_relation_ex call. Deterministic,
 * no LLM at write time (LLM-driven resolution is W2's AutoDream cycle, gated
 * behind HU_ENABLE_LLM_CONFLICT). Goal: keep P95 write latency low while
 * preserving prior facts on supersession instead of overwriting.
 *
 * Resolution policy summary:
 *   - SUPERSEDE  if relation type is single-valued (e.g. WORKS_AT, LIVES_IN)
 *                AND there is an open prior relation with the same source.
 *                Old row gets event_end = new.event_start, new gets
 *                supersedes_id = old.id.
 *   - BRANCH     if relation type is multi-valued (e.g. KNOWS, INTERESTED_IN).
 *                Both kept.
 *   - FLAG       if new.confidence < 0.5 AND there is a strong existing belief
 *                (existing.confidence >= 0.8). New is stored but flagged for
 *                LLM-driven review during the next AutoDream cycle.
 *   - NONE       otherwise.
 */

typedef enum hu_conflict_resolution {
    HU_CONFLICT_NONE,
    HU_CONFLICT_SUPERSEDE,
    HU_CONFLICT_BRANCH,
    HU_CONFLICT_FLAG,
} hu_conflict_resolution_t;

/* Pure classifier: given a proposed relation and the existing strongest open
 * relation (or NULL if none), choose a resolution. Does NOT touch the DB. */
hu_conflict_resolution_t hu_conflict_classify(const hu_graph_relation_t *proposed,
                                              const hu_graph_relation_t *existing);

/* Returns true if the relation type is single-valued (one-current-truth). */
bool hu_conflict_relation_is_single_valued(hu_relation_type_t type);

/* Apply the chosen resolution to the database. The caller is expected to have
 * already inserted `proposed`. On SUPERSEDE, this sets the prior row's
 * event_end and the proposed row's supersedes_id. On BRANCH/NONE/FLAG, no
 * additional writes happen — the caller's INSERT already completed. */
hu_error_t hu_conflict_apply(hu_graph_t *g, hu_conflict_resolution_t decision,
                             int64_t proposed_id, int64_t existing_id, int64_t cutover_ts);

/* Convenience: human-readable label for logs / tests / UI. */
const char *hu_conflict_resolution_str(hu_conflict_resolution_t r);

/* W8 Phase 5 — Semantic-judge fallback.
 *
 * The strict classifier above keys on (source_id, type, target_id). When
 * the strict peek finds no row with the same (source_id, type), a "same
 * fact" may already exist in the graph under a different relation type
 * or a paraphrased context (e.g. "lead engineer at Acme" vs "head of
 * engineering at Acme"). This helper takes a set of candidate existing
 * relations and runs `hu_belief_semantic_conflict` on each candidate's
 * `context` against the proposed `context`. Returns the FIRST match in
 * candidate order:
 *   - PARAPHRASE → HU_CONFLICT_SUPERSEDE + *out_matched_existing_id
 *   - CONTRADICT → HU_CONFLICT_FLAG + *out_matched_existing_id
 *   - no match   → HU_CONFLICT_NONE (*out_matched_existing_id = 0)
 *
 * Heuristic-only (no provider round-trips); deterministic under tests.
 * If `proposed` is NULL, `candidates` is NULL, or `n_candidates == 0`,
 * returns HU_CONFLICT_NONE with `*out_matched_existing_id = 0`.
 * `out_matched_existing_id` may be NULL — the resolution is still
 * returned but the id is silently dropped. */
hu_conflict_resolution_t hu_conflict_classify_semantic(
    const hu_graph_relation_t *proposed,
    const hu_graph_relation_t *candidates,
    size_t n_candidates,
    int64_t *out_matched_existing_id);

#endif /* HU_MEMORY_CONFLICT_RESOLVER_H */
