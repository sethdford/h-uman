#ifndef HU_EMOTIONAL_GRAPH_H
#define HU_EMOTIONAL_GRAPH_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/stm.h"
#include <stddef.h>
#include <stdint.h>

#define HU_EGRAPH_MAX_EDGES 64
#define HU_EGRAPH_MAX_NODES 32

typedef struct hu_egraph_node {
    char *topic;
    size_t topic_len;
} hu_egraph_node_t;

typedef struct hu_egraph_edge {
    size_t topic_idx;
    hu_emotion_tag_t emotion;
    double intensity; /* average intensity across occurrences */
    uint32_t occurrence_count;
} hu_egraph_edge_t;

typedef struct hu_emotional_graph {
    hu_allocator_t alloc;
    hu_egraph_node_t nodes[HU_EGRAPH_MAX_NODES];
    size_t node_count;
    hu_egraph_edge_t edges[HU_EGRAPH_MAX_EDGES];
    size_t edge_count;
} hu_emotional_graph_t;

hu_error_t hu_egraph_init(hu_emotional_graph_t *g, hu_allocator_t alloc);

/* Record an emotional association: topic X evokes emotion Y at intensity Z.
 * If the topic+emotion edge already exists, update the running average. */
hu_error_t hu_egraph_record(hu_emotional_graph_t *g, const char *topic, size_t topic_len,
                            hu_emotion_tag_t emotion, double intensity);

/* Query the dominant emotion for a given topic.
 * Returns the emotion with the highest average intensity.
 * If the topic isn't found, returns HU_EMOTION_NEUTRAL with intensity 0. */
hu_emotion_tag_t hu_egraph_query(const hu_emotional_graph_t *g, const char *topic, size_t topic_len,
                                 double *avg_intensity);

/* Build a context string for the prompt that maps topics to emotions.
 * Output like: "Topics and their emotional associations:\n- work → stress (high)\n- cooking → joy (moderate)\n"
 * Returns NULL if no edges. Caller owns returned string. */
char *hu_egraph_build_context(hu_allocator_t *alloc, const hu_emotional_graph_t *g, size_t *out_len);

/* Populate the graph from STM buffer. Scans turns for topics (primary_topic)
 * and emotions, creating edges between them. */
hu_error_t hu_egraph_populate_from_stm(hu_emotional_graph_t *g, const hu_stm_buffer_t *buf);

void hu_egraph_deinit(hu_emotional_graph_t *g);

#endif
