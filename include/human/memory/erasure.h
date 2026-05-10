#ifndef HU_MEMORY_ERASURE_H
#define HU_MEMORY_ERASURE_H

#include "human/core/error.h"
#include "human/memory/graph.h"
#include <stddef.h>
#include <stdint.h>

/* W4 — Targeted GDPR-compliant erasure.
 *
 * Removes a single user-specified target across every memory surface that
 * could leak it. Cascading deletes from one entity through:
 *   - relations where it appears as source or target
 *   - cross_edges connected to that entity
 *   - case_records anchored to it
 *   - quarantine_relations referring to it
 *   - community_summaries that mention its name
 *
 * The single hu_memory_erase_entity() entry point produces an audit report so
 * the daemon can log "you asked me to forget X, here's what was removed." */

typedef struct hu_erase_report {
    int64_t entity_id;
    size_t relations_deleted;
    size_t cross_edges_deleted;
    size_t case_records_deleted;
    size_t quarantine_deleted;
    size_t community_summaries_invalidated;
    bool entity_deleted;
} hu_erase_report_t;

/* Erase the entity and every reference to it. Returns HU_ERR_NOT_FOUND if no
 * entity has the given id. */
hu_error_t hu_memory_erase_entity(hu_graph_t *graph, int64_t entity_id,
                                  hu_erase_report_t *out_report);

/* Erase every memory row whose provenance contains the given substring. Used
 * for "forget everything I told you on date X" or "forget everything from
 * channel Y" requests. */
hu_error_t hu_memory_erase_by_provenance(hu_graph_t *graph, const char *provenance_substring,
                                         size_t substring_len, hu_erase_report_t *out_report);

#endif /* HU_MEMORY_ERASURE_H */
