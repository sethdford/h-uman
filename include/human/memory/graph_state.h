#ifndef HU_MEMORY_GRAPH_STATE_H
#define HU_MEMORY_GRAPH_STATE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/graph.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* State-first read view over bitemporal relations.
 *
 * The write side (W1 conflict resolver) closes a superseded relation by
 * setting event_end and links the replacement via supersedes_id. Until
 * 2026-09-04 every prompt-facing reader ignored both columns and rendered
 * "user works_at Vanguard" beside "user works_at Raymond James" as if both
 * held — the exact shape of the event-state detections in the n=40 human
 * gate ("done moving all settled in now"). This module is the one place
 * that turns a raw relation set into what is TRUE AS OF a moment, plus the
 * explicit supersession the model needs to say "was X, now Y".
 *
 * Pure: no DB, no I/O. Entries point INTO the caller's relation array. */

typedef struct hu_graph_state_entry {
    const hu_graph_relation_t *rel;  /* relation to render */
    const hu_graph_relation_t *prev; /* relation `rel` superseded, when in the set */
    bool current;                    /* true: holds as of as_of_ms; false: ended (history) */
} hu_graph_state_entry_t;

/* A relation holds at `as_of_ms` when its event window contains it:
 * event_start <= as_of (0 = unknown start, treated as always) and
 * event_end == 0 (still true) or event_end > as_of. */
bool hu_graph_state_is_current(const hu_graph_relation_t *r, int64_t as_of_ms);

/* Resolve `rels` into its state view as of `as_of_ms`.
 *
 * Groups: single-valued types (WORKS_AT, LIVES_IN — see
 * hu_conflict_relation_is_single_valued) group by (source_id, type);
 * multi-valued types group by (source_id, type, target_id).
 *   - Current rows are kept; a single-valued group keeps only its head
 *     (latest event_start, ties by id).
 *   - A non-current row is kept as HISTORY (current=false) only when its
 *     group has no current row in the set, and only the latest-ended one.
 *   - `prev` is the row whose id == rel->supersedes_id when present in
 *     the set, else the latest-ended non-current row of the same group.
 * Output preserves input order. `*out` is allocated with `alloc`
 * (count * sizeof(entry)); on n == 0 sets *out = NULL, *out_count = 0. */
hu_error_t hu_graph_state_resolve(hu_allocator_t *alloc, const hu_graph_relation_t *rels, size_t n,
                                  int64_t as_of_ms, hu_graph_state_entry_t **out,
                                  size_t *out_count);

/* Format a millisecond timestamp as "Mon YYYY" (UTC) for prompt text, e.g.
 * "Aug 2026". Returns the number of bytes written (0 on ms <= 0 or a
 * too-small buffer). */
size_t hu_graph_state_format_month(int64_t ms, char *buf, size_t cap);

#endif /* HU_MEMORY_GRAPH_STATE_H */
