#ifndef HU_HYPEREDGE_H
#define HU_HYPEREDGE_H

/* W8 — Hyperedge: n-ary fact storage backed by SQLite.
 *
 * A hyperedge represents a fact binding >= 2 entities in a single conjunction,
 * e.g. "Alice met Bob at Acme on Friday about funding" (4 entities + metadata).
 * Binary edges (hu_graph_relation_t) remain the default; use hyperedges when
 * 3+ entities are inherently bound.
 *
 * Schema: `hyperedges` + `hyperedge_members` tables (see ensure_schema in .c).
 * SQLite-only: all functions return HU_ERR_NOT_SUPPORTED when SQLite is absent.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/belief.h"
#include "human/memory/memory.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One participant in a hyperedge with a semantic role. */
typedef struct hu_hyperedge_member {
    int64_t entity_id;
    char role[32]; /* "subject", "object", "location", "time", "topic" */
} hu_hyperedge_member_t;

/* A hyperedge: n-ary relation with a belief posterior and event window.
 * `members` is caller-owned on write; facade-owned (and freed via
 * hu_hyperedges_free) on read results. */
typedef struct hu_hyperedge {
    int64_t id;
    char relation_label[64]; /* "met_at", "discussed", "funded", ... */
    hu_hyperedge_member_t *members;
    size_t members_count;
    hu_belief_t belief;
    int64_t event_start;
    int64_t event_end;
    char *provenance; /* nullable; owned by struct on read results */
} hu_hyperedge_t;

/* Insert or update a hyperedge.
 * contact_id/cid_len: owning contact (may be empty string / 0 for global).
 * he->members must be non-NULL with members_count >= 1.
 * he->id is ignored on input; *out_id receives the assigned row id.
 * On conflict (same contact_id + relation_label + member set), the belief and
 * event window are updated via the Welford-style belief update. */
hu_error_t hu_hyperedge_upsert(hu_memory_t *m, const char *contact_id, size_t cid_len,
                               const hu_hyperedge_t *he, int64_t *out_id);

/* Return all hyperedges containing entity_id as a member.
 * Results are allocated from `alloc`; caller must free with hu_hyperedges_free. */
hu_error_t hu_hyperedge_query_by_member(hu_memory_t *m, hu_allocator_t *alloc,
                                         int64_t entity_id,
                                         hu_hyperedge_t **out, size_t *out_count);

/* Free a result array from hu_hyperedge_query_by_member. */
void hu_hyperedges_free(hu_allocator_t *alloc, hu_hyperedge_t *edges, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* HU_HYPEREDGE_H */
