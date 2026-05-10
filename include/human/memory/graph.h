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
    int64_t first_seen;       /* INGEST: first observation */
    int64_t last_seen;        /* INGEST: most recent observation */
    char *context;
    size_t context_len;
    /* Bitemporal fields (W1). Legacy rows get event_start = first_seen, event_end = 0,
     * confidence = 1.0, supersedes_id = 0, provenance = NULL on first read. */
    int64_t event_start;      /* EVENT: when the relation became true in the world; 0 = unknown */
    int64_t event_end;        /* EVENT: when it ceased; 0 = still true */
    float confidence;         /* 0.0-1.0; default 1.0 */
    int64_t supersedes_id;    /* prior relation this replaces; 0 = none */
    char *provenance;         /* source URI / channel / turn-id; nullable */
    size_t provenance_len;
    /* Optional endpoint names (e.g. verifier scan); hu_graph_relations_free frees when set. */
    char *source_name;
    size_t source_name_len;
    char *target_name;
    size_t target_name_len;
} hu_graph_relation_t;

/* Graph context (opaque, backed by SQLite) */
typedef struct hu_graph hu_graph_t;

/* Lifecycle */
hu_error_t hu_graph_open(hu_allocator_t *alloc, const char *db_path, size_t db_path_len,
                         hu_graph_t **out);
void hu_graph_close(hu_graph_t *g, hu_allocator_t *alloc);

/* Entity operations (contact_id scopes entities per-contact for privacy) */
hu_error_t hu_graph_upsert_entity(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                                  const char *name, size_t name_len, hu_entity_type_t type,
                                  const char *metadata_json, int64_t *out_id);
hu_error_t hu_graph_find_entity(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                                const char *name, size_t name_len, hu_graph_entity_t *out);

/* Relation operations */
hu_error_t hu_graph_upsert_relation(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                                    int64_t source_id, int64_t target_id,
                                    hu_relation_type_t type, float weight, const char *context,
                                    size_t context_len);

/* Bitemporal upsert (W1). Forwards to the deterministic conflict resolver before write.
 * - event_start: when the fact became true in the world. Pass 0 to default to now().
 * - event_end:   when it ceased. Pass 0 for "still true."
 * - confidence:  0.0-1.0. Pass < 0 to default to 1.0.
 * - provenance:  optional source attribution (channel/turn-id/URL). NULL/0 = none.
 * On supersession, the prior relation's event_end is set and the new row records
 * supersedes_id = prior.id.
 */
hu_error_t hu_graph_upsert_relation_ex(hu_graph_t *g, const char *contact_id,
                                       size_t contact_id_len, int64_t source_id, int64_t target_id,
                                       hu_relation_type_t type, float weight, int64_t event_start,
                                       int64_t event_end, float confidence, const char *context,
                                       size_t context_len, const char *provenance,
                                       size_t provenance_len);

/* P2G — upsert with full Bayesian belief (mean + variance).
 *
 * The W8 belief layer represents trust as (mean, variance) with
 * provenance. This function lets ingestion paths set BOTH dimensions
 * at write time, instead of writing variance=0 and then having to do
 * a follow-up `hu_graph_set_relation_belief` call.
 *
 * `belief_mean` ∈ [0, 1] — point estimate of truth probability;
 *     callers that pass < 0 get the legacy default of 1.0.
 * `belief_variance` ∈ [0, 0.25] — initial uncertainty; clamped on
 *     write. The W14 reverify runner grows this further as the
 *     relation ages.
 * `out_id` — optional output: the row id of the inserted relation.
 *     Pass NULL if the caller does not need the id.
 *
 * All other arguments mirror `hu_graph_upsert_relation_ex` exactly. */
hu_error_t hu_graph_upsert_relation_with_belief(
    hu_graph_t *g, const char *contact_id, size_t contact_id_len,
    int64_t source_id, int64_t target_id, hu_relation_type_t type,
    float weight, int64_t event_start, int64_t event_end,
    float belief_mean, float belief_variance,
    const char *context, size_t context_len,
    const char *provenance, size_t provenance_len,
    int64_t *out_id);

/* Window query: return relations whose event window overlaps [from_ts, to_ts]. */
hu_error_t hu_graph_relations_in_window(hu_graph_t *g, hu_allocator_t *alloc,
                                        const char *contact_id, size_t contact_id_len,
                                        int64_t from_ts, int64_t to_ts, size_t limit,
                                        hu_graph_relation_t **out, size_t *out_count);

/* Set the community_id of a single entity. Used by Leiden internally and by
 * AutoDream tests. NULL community is allowed (entity left unclustered). */
hu_error_t hu_graph_set_entity_community(hu_graph_t *g, int64_t entity_id, int64_t community_id);

/* Traversal */
hu_error_t hu_graph_neighbors(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                              size_t contact_id_len, int64_t entity_id, size_t max_hops,
                              size_t max_results, hu_graph_entity_t **out_entities,
                              hu_graph_relation_t **out_relations, size_t *out_count);

/* Build context: traverse from query entities and format as prompt text */
hu_error_t hu_graph_build_context(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                  size_t contact_id_len, const char *query, size_t query_len,
                                  size_t max_hops, size_t max_chars, char **out, size_t *out_len);

/* Build context with contact-aware header (filters by contact_id) */
hu_error_t hu_graph_build_contact_context(hu_graph_t *g, hu_allocator_t *alloc, const char *query,
                                          size_t query_len, const char *contact_id,
                                          size_t contact_id_len, size_t max_hops, size_t max_chars,
                                          char **out, size_t *out_len);

/* Community detection: group entities by co-occurrence into topic clusters */
hu_error_t hu_graph_build_communities(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                      size_t contact_id_len, size_t max_communities,
                                      size_t max_chars, char **out, size_t *out_len);

/* Temporal events: query events in time range (returns markdown-formatted text) */
hu_error_t hu_graph_query_temporal(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                   size_t contact_id_len, int64_t from_ts, int64_t to_ts,
                                   size_t limit, char **out, size_t *out_len);

/* Causal links: query cause-effect for an entity (returns markdown-formatted text) */
hu_error_t hu_graph_query_causal(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                 size_t contact_id_len, int64_t entity_id, size_t max_results,
                                 char **out, size_t *out_len);

/* List all entities for a contact (limited to top N by mention_count) */
hu_error_t hu_graph_list_entities(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                  size_t contact_id_len, size_t limit, hu_graph_entity_t **out,
                                  size_t *out_count);

/* List all relations for a contact (limited to top N by weight) */
hu_error_t hu_graph_list_relations(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                   size_t contact_id_len, size_t limit,
                                   hu_graph_relation_t **out, size_t *out_count);

/* Open relations only (`event_end = 0`), ordered by `last_seen` descending.
 * Joins source/target entity names for the same `contact_id` scope. */
hu_error_t hu_graph_list_relations_verifier_scan(hu_graph_t *g, hu_allocator_t *alloc,
                                                 const char *contact_id, size_t contact_id_len,
                                                 size_t limit, hu_graph_relation_t **out,
                                                 size_t *out_count);

/* Free arrays returned by neighbors */
void hu_graph_entities_free(hu_allocator_t *alloc, hu_graph_entity_t *entities, size_t count);
void hu_graph_relations_free(hu_allocator_t *alloc, hu_graph_relation_t *relations, size_t count);

/* W14 belief-reverify support — write back a refined scalar confidence
 * on an existing relation row. Variance is forced to 0 (treats the
 * update as deterministic). `last_seen_now_ms` advances `last_seen`
 * so re-verification counts as recency. NO-OP and HU_OK on
 * relation_id <= 0. For full Bayesian (mean, variance) updates use
 * hu_graph_set_relation_belief instead. */
hu_error_t hu_graph_set_relation_confidence(hu_graph_t *g, int64_t relation_id,
                                            float confidence, int64_t last_seen_now_ms);

/* W8 P2A — write back a full Bayesian posterior (mean + variance).
 * Both `mean` and `variance` are clamped to safe ranges:
 * mean ∈ [0,1], variance ∈ [0, 0.25] (Beta posterior cap). Mirrors
 * the value into the legacy `confidence` column so existing readers
 * see the new mean. NO-OP and HU_OK on relation_id <= 0. */
hu_error_t hu_graph_set_relation_belief(hu_graph_t *g, int64_t relation_id,
                                        float mean, float variance,
                                        int64_t last_seen_now_ms);

/* W8 P2A — read the (mean, variance) belief for a single relation
 * row. Returns HU_ERR_NOT_FOUND if relation_id is missing. */
hu_error_t hu_graph_get_relation_belief(hu_graph_t *g, int64_t relation_id,
                                        float *out_mean, float *out_variance);

/* Ebbinghaus recall tracking: record that an entity was recalled */
hu_error_t hu_graph_record_recall(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                                  int64_t entity_id);

/* Ebbinghaus retention score: compute recall probability (0.0-1.0) */
double hu_graph_retention_score(int64_t last_recalled_ts, int32_t recall_count, int64_t now_ts);

/* Conflict-aware reconsolidation: detect and resolve contradictions */
bool hu_graph_detect_conflict(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                              size_t contact_id_len, const char *entity_name, size_t name_len,
                              const char *new_context, size_t new_context_len);
hu_error_t hu_graph_reconsolidate(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                  size_t contact_id_len, const char *entity_name, size_t name_len,
                                  const char *new_context, size_t new_context_len);

/* Leiden-style hierarchical community detection */
hu_error_t hu_graph_leiden_communities(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                       size_t contact_id_len, size_t max_communities,
                                       size_t max_iterations, char **out, size_t *out_len);

/* Temporal event management */
hu_error_t hu_graph_add_temporal_event(hu_graph_t *g, const char *contact_id,
                                       size_t contact_id_len, int64_t entity_id,
                                       const char *description, size_t desc_len,
                                       int64_t occurred_at, int64_t duration_sec);

/* Causal link management */
hu_error_t hu_graph_add_causal_link(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                                    int64_t action_entity_id, int64_t outcome_entity_id,
                                    const char *context, size_t context_len, float confidence);

/* Helper: parse entity type from string */
hu_entity_type_t hu_entity_type_from_string(const char *s, size_t len);
const char *hu_entity_type_to_string(hu_entity_type_t t);

/* Helper: parse relation type from string */
hu_relation_type_t hu_relation_type_from_string(const char *s, size_t len);
const char *hu_relation_type_to_string(hu_relation_type_t t);

#endif /* HU_MEMORY_GRAPH_H */
