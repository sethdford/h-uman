#ifndef SC_MEMORY_VECTOR_OUTBOX_H
#define SC_MEMORY_VECTOR_OUTBOX_H

#include "seaclaw/memory/vector/embeddings.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

/* Simple in-memory queue for async embedding generation.
 * Enqueue text entries, flush processes them in batches with the given provider. */
typedef struct sc_embedding_outbox sc_embedding_outbox_t;

sc_embedding_outbox_t *sc_embedding_outbox_create(sc_allocator_t *alloc);
void sc_embedding_outbox_destroy(sc_allocator_t *alloc, sc_embedding_outbox_t *ob);

/* Enqueue text for embedding. id is an optional identifier (can be NULL). */
sc_error_t sc_embedding_outbox_enqueue(sc_embedding_outbox_t *ob,
    const char *id, size_t id_len,
    const char *text, size_t text_len);

/* Flush: process all pending entries with the provider.
 * Callback receives (id, id_len, embedding) for each. */
typedef void (*sc_embedding_outbox_flush_cb)(void *userdata,
    const char *id, size_t id_len,
    const float *values, size_t dims);

sc_error_t sc_embedding_outbox_flush(sc_embedding_outbox_t *ob,
    sc_allocator_t *alloc,
    sc_embedding_provider_t *provider,
    sc_embedding_outbox_flush_cb callback,
    void *userdata);

size_t sc_embedding_outbox_pending_count(const sc_embedding_outbox_t *ob);

#endif
