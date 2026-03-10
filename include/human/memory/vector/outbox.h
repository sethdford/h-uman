#ifndef HU_MEMORY_VECTOR_OUTBOX_H
#define HU_MEMORY_VECTOR_OUTBOX_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/vector/embeddings.h"
#include <stddef.h>

/* Simple in-memory queue for async embedding generation.
 * Enqueue text entries, flush processes them in batches with the given provider. */
typedef struct hu_embedding_outbox hu_embedding_outbox_t;

hu_embedding_outbox_t *hu_embedding_outbox_create(hu_allocator_t *alloc);
void hu_embedding_outbox_destroy(hu_allocator_t *alloc, hu_embedding_outbox_t *ob);

/* Enqueue text for embedding. id is an optional identifier (can be NULL). */
hu_error_t hu_embedding_outbox_enqueue(hu_embedding_outbox_t *ob, const char *id, size_t id_len,
                                       const char *text, size_t text_len);

/* Flush: process all pending entries with the provider.
 * Callback receives (id, id_len, embedding) for each. */
typedef void (*hu_embedding_outbox_flush_cb)(void *userdata, const char *id, size_t id_len,
                                             const float *values, size_t dims);

hu_error_t hu_embedding_outbox_flush(hu_embedding_outbox_t *ob, hu_allocator_t *alloc,
                                     hu_embedding_provider_t *provider,
                                     hu_embedding_outbox_flush_cb callback, void *userdata);

size_t hu_embedding_outbox_pending_count(const hu_embedding_outbox_t *ob);

#endif
