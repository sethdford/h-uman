#ifndef SC_MEMORY_GRAPH_H
#define SC_MEMORY_GRAPH_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>
#include <stdint.h>

/* Entity types */
typedef enum sc_entity_type {
    SC_ENTITY_PERSON,
    SC_ENTITY_PLACE,
    SC_ENTITY_ORGANIZATION,
    SC_ENTITY_EVENT,
    SC_ENTITY_TOPIC,
    SC_ENTITY_EMOTION,
    SC_ENTITY_UNKNOWN
} sc_entity_type_t;

/* Relation types */
typedef enum sc_relation_type {
    SC_REL_KNOWS,
    SC_REL_FAMILY_OF,
    SC_REL_WORKS_AT,
    SC_REL_LIVES_IN,
    SC_REL_INTERESTED_IN,
    SC_REL_DISCUSSED_WITH,
    SC_REL_FEELS_ABOUT,
    SC_REL_PROMISED_TO,
    SC_REL_SHARED_EXPERIENCE,
    SC_REL_RELATED_TO
} sc_relation_type_t;

typedef struct sc_graph_entity {
    int64_t id;
    char *name;
    size_t name_len;
    sc_entity_type_t type;
    int64_t first_seen;
    int64_t last_seen;
    int32_t mention_count;
    char *metadata_json;
} sc_graph_entity_t;

typedef struct sc_graph_relation {
    int64_t id;
    int64_t source_id;
    int64_t target_id;
    sc_relation_type_t type;
    float weight;
    int64_t first_seen;
    int64_t last_seen;
    char *context;
    size_t context_len;
} sc_graph_relation_t;

/* Graph context (opaque, backed by SQLite) */
typedef struct sc_graph sc_graph_t;

/* Lifecycle */
sc_error_t sc_graph_open(sc_allocator_t *alloc, const char *db_path, size_t db_path_len,
                         sc_graph_t **out);
void sc_graph_close(sc_graph_t *g, sc_allocator_t *alloc);

/* Entity operations */
sc_error_t sc_graph_upsert_entity(sc_graph_t *g, const char *name, size_t name_len,
                                  sc_entity_type_t type, const char *metadata_json,
                                  int64_t *out_id);
sc_error_t sc_graph_find_entity(sc_graph_t *g, const char *name, size_t name_len,
                                sc_graph_entity_t *out);

/* Relation operations */
sc_error_t sc_graph_upsert_relation(sc_graph_t *g, int64_t source_id, int64_t target_id,
                                    sc_relation_type_t type, float weight, const char *context,
                                    size_t context_len);

/* Traversal */
sc_error_t sc_graph_neighbors(sc_graph_t *g, sc_allocator_t *alloc, int64_t entity_id,
                              size_t max_hops, size_t max_results, sc_graph_entity_t **out_entities,
                              sc_graph_relation_t **out_relations, size_t *out_count);

/* Build context: traverse from query entities and format as prompt text */
sc_error_t sc_graph_build_context(sc_graph_t *g, sc_allocator_t *alloc, const char *query,
                                  size_t query_len, size_t max_hops, size_t max_chars, char **out,
                                  size_t *out_len);

/* Build context with contact-aware header (cross-contact knowledge synthesis) */
sc_error_t sc_graph_build_contact_context(sc_graph_t *g, sc_allocator_t *alloc, const char *query,
                                          size_t query_len, const char *contact_id,
                                          size_t contact_id_len, size_t max_hops, size_t max_chars,
                                          char **out, size_t *out_len);

/* Community detection: group entities by co-occurrence into topic clusters */
sc_error_t sc_graph_build_communities(sc_graph_t *g, sc_allocator_t *alloc, size_t max_communities,
                                      size_t max_chars, char **out, size_t *out_len);

/* Temporal events: query events in time range (returns markdown-formatted text) */
sc_error_t sc_graph_query_temporal(sc_graph_t *g, sc_allocator_t *alloc, int64_t from_ts,
                                   int64_t to_ts, size_t limit, char **out, size_t *out_len);

/* Causal links: query cause-effect for an entity (returns markdown-formatted text) */
sc_error_t sc_graph_query_causal(sc_graph_t *g, sc_allocator_t *alloc, int64_t entity_id,
                                 size_t max_results, char **out, size_t *out_len);

/* Free arrays returned by neighbors */
void sc_graph_entities_free(sc_allocator_t *alloc, sc_graph_entity_t *entities, size_t count);
void sc_graph_relations_free(sc_allocator_t *alloc, sc_graph_relation_t *relations, size_t count);

/* Helper: parse entity type from string */
sc_entity_type_t sc_entity_type_from_string(const char *s, size_t len);
const char *sc_entity_type_to_string(sc_entity_type_t t);

/* Helper: parse relation type from string */
sc_relation_type_t sc_relation_type_from_string(const char *s, size_t len);
const char *sc_relation_type_to_string(sc_relation_type_t t);

#endif /* SC_MEMORY_GRAPH_H */
