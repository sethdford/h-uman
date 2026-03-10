#ifndef HU_MEMORY_GRAPH_H
#define HU_MEMORY_GRAPH_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

/* Entity types */
typedef enum hu_entity_type {
    HU_ENTITY_PERSON,
    HU_ENTITY_PLACE,
    HU_ENTITY_ORGANIZATION,
    HU_ENTITY_EVENT,
    HU_ENTITY_TOPIC,
    HU_ENTITY_EMOTION,
    HU_ENTITY_UNKNOWN
} hu_entity_type_t;

/* Relation types */
typedef enum hu_relation_type {
    HU_REL_KNOWS,
    HU_REL_FAMILY_OF,
    HU_REL_WORKS_AT,
    HU_REL_LIVES_IN,
    HU_REL_INTERESTED_IN,
    HU_REL_DISCUSSED_WITH,
    HU_REL_FEELS_ABOUT,
    HU_REL_PROMISED_TO,
    HU_REL_SHARED_EXPERIENCE,
    HU_REL_RELATED_TO
} hu_relation_type_t;

typedef struct hu_graph_entity {
    int64_t id;
    char *name;
    size_t name_len;
    hu_entity_type_t type;
    int64_t first_seen;
    int64_t last_seen;
    int32_t mention_count;
    char *metadata_json;
} hu_graph_entity_t;

typedef struct hu_graph_relation {
    int64_t id;
    int64_t source_id;
    int64_t target_id;
    hu_relation_type_t type;
    float weight;
    int64_t first_seen;
    int64_t last_seen;
    char *context;
    size_t context_len;
} hu_graph_relation_t;

/* Graph context (opaque, backed by SQLite) */
typedef struct hu_graph hu_graph_t;

/* Lifecycle */
hu_error_t hu_graph_open(hu_allocator_t *alloc, const char *db_path, size_t db_path_len,
                         hu_graph_t **out);
void hu_graph_close(hu_graph_t *g, hu_allocator_t *alloc);

/* Entity operations */
hu_error_t hu_graph_upsert_entity(hu_graph_t *g, const char *name, size_t name_len,
                                  hu_entity_type_t type, const char *metadata_json,
                                  int64_t *out_id);
hu_error_t hu_graph_find_entity(hu_graph_t *g, const char *name, size_t name_len,
                                hu_graph_entity_t *out);

/* Relation operations */
hu_error_t hu_graph_upsert_relation(hu_graph_t *g, int64_t source_id, int64_t target_id,
                                    hu_relation_type_t type, float weight, const char *context,
                                    size_t context_len);

/* Traversal */
hu_error_t hu_graph_neighbors(hu_graph_t *g, hu_allocator_t *alloc, int64_t entity_id,
                              size_t max_hops, size_t max_results, hu_graph_entity_t **out_entities,
                              hu_graph_relation_t **out_relations, size_t *out_count);

/* Build context: traverse from query entities and format as prompt text */
hu_error_t hu_graph_build_context(hu_graph_t *g, hu_allocator_t *alloc, const char *query,
                                  size_t query_len, size_t max_hops, size_t max_chars, char **out,
                                  size_t *out_len);

/* Build context with contact-aware header (cross-contact knowledge synthesis) */
hu_error_t hu_graph_build_contact_context(hu_graph_t *g, hu_allocator_t *alloc, const char *query,
                                          size_t query_len, const char *contact_id,
                                          size_t contact_id_len, size_t max_hops, size_t max_chars,
                                          char **out, size_t *out_len);

/* Community detection: group entities by co-occurrence into topic clusters */
hu_error_t hu_graph_build_communities(hu_graph_t *g, hu_allocator_t *alloc, size_t max_communities,
                                      size_t max_chars, char **out, size_t *out_len);

/* Temporal events: query events in time range (returns markdown-formatted text) */
hu_error_t hu_graph_query_temporal(hu_graph_t *g, hu_allocator_t *alloc, int64_t from_ts,
                                   int64_t to_ts, size_t limit, char **out, size_t *out_len);

/* Causal links: query cause-effect for an entity (returns markdown-formatted text) */
hu_error_t hu_graph_query_causal(hu_graph_t *g, hu_allocator_t *alloc, int64_t entity_id,
                                 size_t max_results, char **out, size_t *out_len);

/* List all entities (limited to top N by mention_count) */
hu_error_t hu_graph_list_entities(hu_graph_t *g, hu_allocator_t *alloc, size_t limit,
                                  hu_graph_entity_t **out, size_t *out_count);

/* List all relations (limited to top N by weight) */
hu_error_t hu_graph_list_relations(hu_graph_t *g, hu_allocator_t *alloc, size_t limit,
                                   hu_graph_relation_t **out, size_t *out_count);

/* Free arrays returned by neighbors */
void hu_graph_entities_free(hu_allocator_t *alloc, hu_graph_entity_t *entities, size_t count);
void hu_graph_relations_free(hu_allocator_t *alloc, hu_graph_relation_t *relations, size_t count);

/* Ebbinghaus recall tracking: record that an entity was recalled */
hu_error_t hu_graph_record_recall(hu_graph_t *g, int64_t entity_id);

/* Ebbinghaus retention score: compute recall probability (0.0-1.0) */
double hu_graph_retention_score(int64_t last_recalled_ts, int32_t recall_count, int64_t now_ts);

/* Conflict-aware reconsolidation: detect and resolve contradictions */
bool hu_graph_detect_conflict(hu_graph_t *g, hu_allocator_t *alloc, const char *entity_name,
                              size_t name_len, const char *new_context, size_t new_context_len);
hu_error_t hu_graph_reconsolidate(hu_graph_t *g, hu_allocator_t *alloc, const char *entity_name,
                                  size_t name_len, const char *new_context, size_t new_context_len);

/* Leiden-style hierarchical community detection */
hu_error_t hu_graph_leiden_communities(hu_graph_t *g, hu_allocator_t *alloc, size_t max_communities,
                                       size_t max_iterations, char **out, size_t *out_len);

/* Temporal event management */
hu_error_t hu_graph_add_temporal_event(hu_graph_t *g, int64_t entity_id, const char *description,
                                       size_t desc_len, int64_t occurred_at, int64_t duration_sec);

/* Causal link management */
hu_error_t hu_graph_add_causal_link(hu_graph_t *g, int64_t action_entity_id,
                                    int64_t outcome_entity_id, const char *context,
                                    size_t context_len, float confidence);

/* Helper: parse entity type from string */
hu_entity_type_t hu_entity_type_from_string(const char *s, size_t len);
const char *hu_entity_type_to_string(hu_entity_type_t t);

/* Helper: parse relation type from string */
hu_relation_type_t hu_relation_type_from_string(const char *s, size_t len);
const char *hu_relation_type_to_string(hu_relation_type_t t);

#endif /* HU_MEMORY_GRAPH_H */
