/* semantic_recall.c — gate + wiring for the real semantic retriever. */
#include "human/memory/semantic_recall.h"

#include "human/memory/vector/embedder_http.h"
#include "human/memory/vector/store_sqlite_vec.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

size_t hu_semantic_recall_max_bytes(void) {
    const char *v = getenv("HU_SEMANTIC_RECALL_MAX_BYTES");
    if (!v || !v[0])
        return HU_SEMANTIC_RECALL_DEFAULT_MAX_BYTES;
    char *end = NULL;
    errno = 0;
    long n = strtol(v, &end, 10);
    if (errno != 0 || end == v || (end && *end != '\0') || n <= 0)
        return HU_SEMANTIC_RECALL_DEFAULT_MAX_BYTES; /* fail closed to the default */
    return (size_t)n;
}

size_t hu_semantic_recall_truncate_len(const char *s, size_t len, size_t max_bytes) {
    if (!s || max_bytes == 0)
        return 0;
    if (len <= max_bytes)
        return len;
    /* Prefer the last space in the upper half of the window: s[0, i) is then
     * whole words. A boundary further back would discard too much. */
    for (size_t i = max_bytes; i > max_bytes / 2; i--) {
        if (s[i] == ' ' || s[i] == '\n' || s[i] == '\t') {
            size_t cut = i;
            while (cut > 0 && (s[cut - 1] == ' ' || s[cut - 1] == '\n' || s[cut - 1] == '\t'))
                cut--;
            return cut;
        }
    }
    /* Hard cut: never split a multi-byte UTF-8 sequence. */
    size_t cut = max_bytes;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0u) == 0x80u)
        cut--;
    return cut;
}

size_t hu_semantic_recall_clamp_result(hu_allocator_t *alloc, hu_retrieval_result_t *res,
                                       size_t budget_bytes, size_t per_hit_bytes) {
    if (!alloc || !res || !res->entries || res->count == 0)
        return 0;
    size_t used = 0, keep = 0;
    for (size_t i = 0; i < res->count; i++) {
        hu_memory_entry_t *e = &res->entries[i];
        size_t clen = e->content ? e->content_len : 0;
        size_t cut =
            e->content ? hu_semantic_recall_truncate_len(e->content, clen, per_hit_bytes) : 0;
        if (used + cut > budget_bytes)
            break; /* this hit and every lower-ranked one are dropped */
        if (cut < clen) {
            /* Binary-safe copy: content may carry embedded NULs (the
             * 2026-07-13 memory-loader overflow), so hu_strndup — which stops
             * at the first NUL — would allocate fewer than cut+1 bytes while
             * content_len still claims cut. Copy exactly cut bytes. */
            char *nc = (char *)alloc->alloc(alloc->ctx, cut + 1);
            if (!nc)
                break;
            memcpy(nc, e->content, cut);
            nc[cut] = '\0';
            alloc->free(alloc->ctx, (void *)e->content, clen + 1);
            e->content = nc;
            e->content_len = cut;
        }
        used += cut;
        keep++;
    }
    if (keep == res->count)
        return used;
    for (size_t i = keep; i < res->count; i++) {
        hu_memory_entry_free_fields(alloc, &res->entries[i]);
        memset(&res->entries[i], 0, sizeof(res->entries[i])); /* no dangling pointers */
    }
    size_t old = res->count;
    if (keep == 0) {
        alloc->free(alloc->ctx, res->entries, old * sizeof(hu_memory_entry_t));
        if (res->scores)
            alloc->free(alloc->ctx, res->scores, old * sizeof(double));
        res->entries = NULL;
        res->scores = NULL;
        res->count = 0;
        return 0;
    }
    /* Shrink so hu_retrieval_result_free's sized frees stay exact. Both
     * shipped allocators ignore old_size on free, so a failed shrink (kept
     * larger buffer, smaller count) is tolerated rather than fatal. */
    hu_memory_entry_t *ne = (hu_memory_entry_t *)alloc->realloc(alloc->ctx, res->entries,
                                                                old * sizeof(hu_memory_entry_t),
                                                                keep * sizeof(hu_memory_entry_t));
    if (ne)
        res->entries = ne;
    if (res->scores) {
        double *ns = (double *)alloc->realloc(alloc->ctx, res->scores, old * sizeof(double),
                                              keep * sizeof(double));
        if (ns)
            res->scores = ns;
    }
    res->count = keep;
    return used;
}

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
