#ifndef SC_MEMORY_RETRIEVAL_H
#define SC_MEMORY_RETRIEVAL_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"
#include "seaclaw/memory/vector.h"
#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Retrieval modes and options
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum sc_retrieval_mode {
    SC_RETRIEVAL_KEYWORD,
    SC_RETRIEVAL_SEMANTIC,
    SC_RETRIEVAL_HYBRID,
} sc_retrieval_mode_t;

typedef struct sc_retrieval_options {
    sc_retrieval_mode_t mode;
    size_t limit;
    double min_score;
    bool use_reranking;
    double temporal_decay_factor; /* 0.0 = no decay, 1.0 = strong decay */
} sc_retrieval_options_t;

typedef struct sc_retrieval_result {
    sc_memory_entry_t *entries;
    size_t count;
    double *scores;
} sc_retrieval_result_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Retrieval engine vtable
 * ────────────────────────────────────────────────────────────────────────── */

struct sc_retrieval_vtable;

typedef struct sc_retrieval_engine {
    void *ctx;
    const struct sc_retrieval_vtable *vtable;
} sc_retrieval_engine_t;

typedef struct sc_retrieval_vtable {
    sc_error_t (*retrieve)(void *ctx, sc_allocator_t *alloc, const char *query, size_t query_len,
                           const sc_retrieval_options_t *opts, sc_retrieval_result_t *out);
    void (*deinit)(void *ctx, sc_allocator_t *alloc);
} sc_retrieval_vtable_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Factory and helpers
 * ────────────────────────────────────────────────────────────────────────── */

sc_retrieval_engine_t sc_retrieval_create(sc_allocator_t *alloc, sc_memory_t *backend);

sc_retrieval_engine_t sc_retrieval_create_with_vector(sc_allocator_t *alloc, sc_memory_t *backend,
                                                      sc_embedder_t *embedder,
                                                      sc_vector_store_t *vector_store);

void sc_retrieval_result_free(sc_allocator_t *alloc, sc_retrieval_result_t *r);

sc_error_t sc_retrieval_index_entry(sc_retrieval_engine_t *engine, sc_allocator_t *alloc,
                                    const char *key, size_t key_len, const char *content,
                                    size_t content_len);

/* Internal retrieval strategies (used by engine) */
sc_error_t sc_semantic_retrieve(sc_allocator_t *alloc, sc_embedder_t *embedder,
                                sc_vector_store_t *vector_store, const char *query,
                                size_t query_len, const sc_retrieval_options_t *opts,
                                sc_retrieval_result_t *out);

sc_error_t sc_keyword_retrieve(sc_allocator_t *alloc, sc_memory_t *backend, const char *query,
                               size_t query_len, const sc_retrieval_options_t *opts,
                               sc_retrieval_result_t *out);

sc_error_t sc_hybrid_retrieve(sc_allocator_t *alloc, sc_memory_t *backend, sc_embedder_t *embedder,
                              sc_vector_store_t *vector_store, const char *query, size_t query_len,
                              const sc_retrieval_options_t *opts, sc_retrieval_result_t *out);

/* Temporal decay: apply to base_score; entries without timestamp unchanged */
double sc_temporal_decay_score(double base_score, double decay_factor, const char *timestamp,
                               size_t timestamp_len);

/* MMR reranking: diversifies results. Modifies entries/scores in place. */
sc_error_t sc_mmr_rerank(sc_allocator_t *alloc, const char *query, size_t query_len,
                         sc_memory_entry_t *entries, double *scores, size_t count, double lambda);

#endif /* SC_MEMORY_RETRIEVAL_H */
