#ifndef HU_MEMORY_CROSS_GRAPH_H
#define HU_MEMORY_CROSS_GRAPH_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/graph.h"
#include <stdbool.h>
#include <stdint.h>

/* W3 — Cross-graph layer.
 *
 * h-uman ships several parallel subgraphs (entity, emotional, contact,
 * episodic). Today they are walked separately; multi-hop temporal questions
 * ("what was I feeling the last time we discussed X with Casey?") need an LLM
 * to bridge them. MAGMA's design — typed cross-edges between subgraph nodes
 * with bounded traversal — closes that gap with graph operations.
 *
 * A cross-edge is identified by (src_graph, src_id, dst_graph, dst_id,
 * relation). Subgraph names are short fixed strings ("entity", "emotion",
 * "contact", "episode") — chosen for readability, not extensibility, since
 * the closed set keeps the schema small. */

typedef struct hu_cross_edge {
    int64_t id;
    const char *src_graph;     /* not owned; valid for the lifetime of the row in memory */
    int64_t src_id;
    const char *dst_graph;     /* not owned */
    int64_t dst_id;
    const char *relation;      /* not owned */
    float confidence;
    int64_t event_start;
    int64_t event_end;
    float weight;
} hu_cross_edge_t;

/* Upsert a cross-edge. Idempotent on
 * (contact_id, src_graph, src_id, dst_graph, dst_id, relation). String fields
 * are NOT borrowed — they're copied into the DB. */
hu_error_t hu_cross_edge_upsert(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                                const char *src_graph, int64_t src_id, const char *dst_graph,
                                int64_t dst_id, const char *relation, float confidence,
                                int64_t event_start, int64_t event_end, float weight);

/* Bounded traversal from a starting node across cross-edges. Returns one row
 * per matching edge. event_window_{start,end} = 0 means unbounded (use with
 * caution on hot paths). Caller frees via hu_cross_edges_free. */
hu_error_t hu_cross_graph_traverse(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                   size_t contact_id_len, const char *start_graph,
                                   int64_t start_id, size_t max_hops, size_t max_results,
                                   int64_t event_window_start, int64_t event_window_end,
                                   hu_cross_edge_t **out, size_t *out_count);

/* Free an array returned by hu_cross_graph_traverse. */
void hu_cross_edges_free(hu_allocator_t *alloc, hu_cross_edge_t *edges, size_t count);

#endif /* HU_MEMORY_CROSS_GRAPH_H */
