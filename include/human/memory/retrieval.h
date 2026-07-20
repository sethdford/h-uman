#ifndef HU_MEMORY_RETRIEVAL_H
#define HU_MEMORY_RETRIEVAL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/memory/graph.h"
#include "human/memory/vector.h"
#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Retrieval modes and options
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_retrieval_mode {
    HU_RETRIEVAL_KEYWORD,
    HU_RETRIEVAL_SEMANTIC,
    HU_RETRIEVAL_HYBRID,
} hu_retrieval_mode_t;

typedef struct hu_retrieval_options {
    hu_retrieval_mode_t mode;
    size_t limit;
    double min_score;
    bool use_reranking;
    double temporal_decay_factor; /* 0.0 = no decay, 1.0 = strong decay */
    /* Wave B namespace (borrowed). When require_contact_namespace is true,
     * contact_id must be non-empty or retrieve returns HU_ERR_INVALID_ARGUMENT.
     * When contact_id is set, results are filtered to keys prefixed
     * contact:<id>: — unscoped rows are excluded (fail-closed isolation). */
    const char *contact_id;
    size_t contact_id_len;
    const char *session_id;
    size_t session_id_len;
    bool require_contact_namespace;
} hu_retrieval_options_t;

typedef struct hu_retrieval_result {
    hu_memory_entry_t *entries;
    size_t count;
    double *scores;
} hu_retrieval_result_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Retrieval engine vtable
 * ────────────────────────────────────────────────────────────────────────── */

struct hu_retrieval_vtable;

typedef struct hu_retrieval_engine {
    void *ctx;
    const struct hu_retrieval_vtable *vtable;
} hu_retrieval_engine_t;

typedef struct hu_retrieval_vtable {
    hu_error_t (*retrieve)(void *ctx, hu_allocator_t *alloc, const char *query, size_t query_len,
                           const hu_retrieval_options_t *opts, hu_retrieval_result_t *out);
    void (*deinit)(void *ctx, hu_allocator_t *alloc);
} hu_retrieval_vtable_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Factory and helpers
 * ────────────────────────────────────────────────────────────────────────── */

hu_retrieval_engine_t hu_retrieval_create(hu_allocator_t *alloc, hu_memory_t *backend);

hu_retrieval_engine_t hu_retrieval_create_with_vector(hu_allocator_t *alloc, hu_memory_t *backend,
                                                      hu_embedder_t *embedder,
                                                      hu_vector_store_t *vector_store);

void hu_retrieval_result_free(hu_allocator_t *alloc, hu_retrieval_result_t *r);

hu_error_t hu_retrieval_index_entry(hu_retrieval_engine_t *engine, hu_allocator_t *alloc,
                                    const char *key, size_t key_len, const char *content,
                                    size_t content_len);

void hu_retrieval_set_graph(hu_retrieval_engine_t *engine, hu_graph_t *graph);

/* Internal retrieval strategies (used by engine) */
hu_error_t hu_semantic_retrieve(hu_allocator_t *alloc, hu_embedder_t *embedder,
                                hu_vector_store_t *vector_store, const char *query,
                                size_t query_len, const hu_retrieval_options_t *opts,
                                hu_retrieval_result_t *out);

hu_error_t hu_keyword_retrieve(hu_allocator_t *alloc, hu_memory_t *backend, const char *query,
                               size_t query_len, const hu_retrieval_options_t *opts,
                               hu_retrieval_result_t *out);

hu_error_t hu_hybrid_retrieve(hu_allocator_t *alloc, hu_memory_t *backend, hu_embedder_t *embedder,
                              hu_vector_store_t *vector_store, hu_graph_t *graph, const char *query,
                              size_t query_len, const hu_retrieval_options_t *opts,
                              hu_retrieval_result_t *out);

/* Temporal decay: apply to base_score; entries without timestamp unchanged */
double hu_temporal_decay_score(double base_score, double decay_factor, const char *timestamp,
                               size_t timestamp_len);

/* Namespace helpers (Wave B contact/session isolation). */
hu_error_t hu_retrieval_check_namespace(const hu_retrieval_options_t *opts);
bool hu_retrieval_entry_in_contact_scope(const hu_memory_entry_t *e, const char *contact_id,
                                         size_t contact_id_len);
hu_error_t hu_retrieval_filter_by_namespace(hu_allocator_t *alloc, hu_retrieval_result_t *r,
                                            const hu_retrieval_options_t *opts);

/* MMR reranking: diversifies results. Modifies entries/scores in place. */
hu_error_t hu_mmr_rerank(hu_allocator_t *alloc, const char *query, size_t query_len,
                         hu_memory_entry_t *entries, double *scores, size_t count, double lambda);

#endif /* HU_MEMORY_RETRIEVAL_H */
