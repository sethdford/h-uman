/* semantic_recall.c — gate + wiring for the real semantic retriever. */
#include "human/memory/semantic_recall.h"

#include "human/memory/vector/embedder_http.h"
#include "human/memory/vector/store_sqlite_vec.h"

#include <stdlib.h>
#include <string.h>

hu_gate_mode_t hu_semantic_recall_mode(void) {
    return hu_gate_mode_from_env("HU_SEMANTIC_RECALL", HU_GATE_OFF);
}

const char *hu_semantic_recall_embed_url(void) {
    const char *u = getenv("HU_SEMANTIC_EMBED_URL");
    return (u && u[0]) ? u : "http://127.0.0.1:8741";
}

hu_error_t hu_semantic_recall_attach(hu_allocator_t *alloc, hu_memory_t *mem,
                                     hu_embedder_t *out_embedder, hu_vector_store_t *out_store) {
    if (!alloc || !mem || !mem->vtable || !out_embedder || !out_store)
        return HU_ERR_INVALID_ARGUMENT;
    out_embedder->ctx = NULL;
    out_store->ctx = NULL;
#ifdef HU_ENABLE_SQLITE
    struct sqlite3 *db = hu_sqlite_memory_get_db(mem);
    if (!db)
        return HU_ERR_NOT_SUPPORTED; /* not the sqlite engine */
    *out_embedder = hu_embedder_http_create(alloc, hu_semantic_recall_embed_url());
    if (!out_embedder->ctx)
        return HU_ERR_INTERNAL;
    *out_store = hu_vector_store_sqlite_vec_create(alloc, db, HU_SEMANTIC_EMBED_DIM);
    if (!out_store->ctx) {
        out_embedder->vtable->deinit(out_embedder->ctx, alloc);
        out_embedder->ctx = NULL;
        return HU_ERR_INTERNAL;
    }
    hu_sqlite_memory_set_semantic_index(mem, out_embedder, out_store);
    return HU_OK;
#else
    return HU_ERR_NOT_SUPPORTED;
#endif
}
