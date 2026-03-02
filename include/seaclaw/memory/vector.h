#ifndef SC_MEMORY_VECTOR_H
#define SC_MEMORY_VECTOR_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

#define SC_EMBEDDING_DIM 384  /* small model default */

typedef struct sc_embedding {
    float *values;
    size_t dim;
} sc_embedding_t;

typedef struct sc_vector_entry {
    const char *id;
    size_t id_len;
    sc_embedding_t embedding;
    const char *content;
    size_t content_len;
    float score;
} sc_vector_entry_t;

/* Embedding provider vtable */
struct sc_embedder_vtable;

typedef struct sc_embedder {
    void *ctx;
    const struct sc_embedder_vtable *vtable;
} sc_embedder_t;

typedef struct sc_embedder_vtable {
    sc_error_t (*embed)(void *ctx, sc_allocator_t *alloc,
        const char *text, size_t text_len,
        sc_embedding_t *out);
    sc_error_t (*embed_batch)(void *ctx, sc_allocator_t *alloc,
        const char **texts, const size_t *text_lens, size_t count,
        sc_embedding_t *out);
    size_t (*dimensions)(void *ctx);
    void (*deinit)(void *ctx, sc_allocator_t *alloc);
} sc_embedder_vtable_t;

/* Vector store vtable */
struct sc_vector_store_vtable;

typedef struct sc_vector_store {
    void *ctx;
    const struct sc_vector_store_vtable *vtable;
} sc_vector_store_t;

typedef struct sc_vector_store_vtable {
    sc_error_t (*insert)(void *ctx, sc_allocator_t *alloc,
        const char *id, size_t id_len,
        const sc_embedding_t *embedding,
        const char *content, size_t content_len);
    sc_error_t (*search)(void *ctx, sc_allocator_t *alloc,
        const sc_embedding_t *query, size_t limit,
        sc_vector_entry_t **out, size_t *out_count);
    sc_error_t (*remove)(void *ctx, const char *id, size_t id_len);
    size_t (*count)(void *ctx);
    void (*deinit)(void *ctx, sc_allocator_t *alloc);
} sc_vector_store_vtable_t;

/* Chunker for splitting text before embedding */
typedef struct sc_chunker_options {
    size_t max_chunk_size;
    size_t overlap;
} sc_chunker_options_t;

typedef struct sc_text_chunk {
    const char *text;
    size_t text_len;
    size_t offset;
} sc_text_chunk_t;

sc_error_t sc_chunker_split(sc_allocator_t *alloc,
    const char *text, size_t text_len,
    const sc_chunker_options_t *opts,
    sc_text_chunk_t **out, size_t *out_count);
void sc_chunker_free(sc_allocator_t *alloc, sc_text_chunk_t *chunks, size_t count);

/* Utility */
float sc_cosine_similarity(const float *a, const float *b, size_t dim);
void sc_embedding_free(sc_allocator_t *alloc, sc_embedding_t *e);
void sc_vector_entries_free(sc_allocator_t *alloc, sc_vector_entry_t *entries, size_t count);

/* Factory: in-memory vector store */
sc_vector_store_t sc_vector_store_mem_create(sc_allocator_t *alloc);

/* Factory: local TF-IDF style embedder */
sc_embedder_t sc_embedder_local_create(sc_allocator_t *alloc);

#endif /* SC_MEMORY_VECTOR_H */
