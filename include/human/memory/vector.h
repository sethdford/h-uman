#ifndef HU_MEMORY_VECTOR_H
#define HU_MEMORY_VECTOR_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

#define HU_EMBEDDING_DIM 384 /* small model default */

typedef struct hu_embedding {
    float *values;
    size_t dim;
} hu_embedding_t;

typedef struct hu_vector_entry {
    const char *id;
    size_t id_len;
    hu_embedding_t embedding;
    const char *content;
    size_t content_len;
    float score;
} hu_vector_entry_t;

/* Embedding provider vtable */
struct hu_embedder_vtable;

typedef struct hu_embedder {
    void *ctx;
    const struct hu_embedder_vtable *vtable;
} hu_embedder_t;

typedef struct hu_embedder_vtable {
    hu_error_t (*embed)(void *ctx, hu_allocator_t *alloc, const char *text, size_t text_len,
                        hu_embedding_t *out);
    hu_error_t (*embed_batch)(void *ctx, hu_allocator_t *alloc, const char **texts,
                              const size_t *text_lens, size_t count, hu_embedding_t *out);
    size_t (*dimensions)(void *ctx);
    void (*deinit)(void *ctx, hu_allocator_t *alloc);
} hu_embedder_vtable_t;

/* Vector store vtable */
struct hu_vector_store_vtable;

typedef struct hu_vector_store {
    void *ctx;
    const struct hu_vector_store_vtable *vtable;
} hu_vector_store_t;

typedef struct hu_vector_store_vtable {
    hu_error_t (*insert)(void *ctx, hu_allocator_t *alloc, const char *id, size_t id_len,
                         const hu_embedding_t *embedding, const char *content, size_t content_len);
    hu_error_t (*search)(void *ctx, hu_allocator_t *alloc, const hu_embedding_t *query,
                         size_t limit, hu_vector_entry_t **out, size_t *out_count);
    hu_error_t (*remove)(void *ctx, const char *id, size_t id_len);
    size_t (*count)(void *ctx);
    void (*deinit)(void *ctx, hu_allocator_t *alloc);
} hu_vector_store_vtable_t;

/* Chunker for splitting text before embedding */
typedef struct hu_chunker_options {
    size_t max_chunk_size;
    size_t overlap;
} hu_chunker_options_t;

typedef struct hu_text_chunk {
    const char *text;
    size_t text_len;
    size_t offset;
} hu_text_chunk_t;

hu_error_t hu_chunker_split(hu_allocator_t *alloc, const char *text, size_t text_len,
                            const hu_chunker_options_t *opts, hu_text_chunk_t **out,
                            size_t *out_count);
void hu_chunker_free(hu_allocator_t *alloc, hu_text_chunk_t *chunks, size_t count);

/* Utility */
float hu_cosine_similarity(const float *a, const float *b, size_t dim);
void hu_embedding_free(hu_allocator_t *alloc, hu_embedding_t *e);
void hu_vector_entries_free(hu_allocator_t *alloc, hu_vector_entry_t *entries, size_t count);

/* Factory: in-memory vector store */
hu_vector_store_t hu_vector_store_mem_create(hu_allocator_t *alloc);

/* Factory: local TF-IDF style embedder */
hu_embedder_t hu_embedder_local_create(hu_allocator_t *alloc);

#endif /* HU_MEMORY_VECTOR_H */
