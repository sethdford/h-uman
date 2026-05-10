#ifndef HU_AGENT_CASE_BASED_H
#define HU_AGENT_CASE_BASED_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/memory.h"
#include <stddef.h>
#include <stdint.h>

/* W3 — Case-based recall.
 *
 * Given a current goal (verb + anchor entities), return the top-K past cases
 * the assistant has handled before, scored by entity-overlap and recency.
 *
 * "Cases" are persisted in a small `case_records` table that this module owns.
 * Plans, replies, scheduling decisions all become case records via
 * hu_case_record(). The retrieval is intentionally narrow: it does NOT do
 * vector similarity yet (W6 / MemRL territory). It does goal-verb match +
 * anchor-entity overlap + recency. That's already enough for the planner to
 * stop replanning known tasks from scratch. */

typedef struct hu_case_record {
    int64_t id;
    char *goal_verb;        /* not borrowed; freed by hu_case_records_free */
    size_t goal_verb_len;
    int64_t *anchor_entity_ids;  /* not borrowed; freed by hu_case_records_free */
    size_t anchor_count;
    char *plan_text;        /* not borrowed; nullable */
    size_t plan_text_len;
    char *outcome;          /* not borrowed; nullable. Free-form: "ok", "user pushed back", ... */
    size_t outcome_len;
    int64_t happened_at;
    float score;            /* set by recall(); 0 by record() */
} hu_case_record_t;

/* Persist a case the agent just handled. anchor_entity_ids may be NULL/0. */
hu_error_t hu_case_record(hu_memory_facade_t *m, const char *contact_id, size_t contact_id_len,
                          const char *goal_verb, size_t goal_verb_len,
                          const int64_t *anchor_entity_ids, size_t anchor_count,
                          const char *plan_text, size_t plan_text_len, const char *outcome,
                          size_t outcome_len, int64_t happened_at, int64_t *out_id);

/* Recall the top-K cases similar to (goal_verb, anchor_entity_ids). Score is
 * 0..1, combining anchor-entity overlap and recency. Returns 0 results
 * gracefully when no matches exist. */
hu_error_t hu_case_recall(hu_memory_facade_t *m, hu_allocator_t *alloc, const char *contact_id,
                          size_t contact_id_len, const char *goal_verb, size_t goal_verb_len,
                          const int64_t *anchor_entity_ids, size_t anchor_count, int64_t now_ms,
                          size_t top_k, hu_case_record_t **out, size_t *out_count);

void hu_case_records_free(hu_allocator_t *alloc, hu_case_record_t *records, size_t count);

#endif /* HU_AGENT_CASE_BASED_H */
