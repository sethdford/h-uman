/* W7 Memory Facade — dispatcher.
 *
 * Owns nothing of substance: a vtable per kind, a pointer to the graph for the
 * v1 backend, and the routing-metadata SQLite table. All real work happens in
 * registered backends.
 */

#include "human/memory/memory.h"

#include "human/core/error.h"
#include <string.h>

/* Forward declarations from memory_v1_backend.c. */
hu_error_t hu_memory__v1_backend_register(struct hu_memory *m, hu_graph_t *graph);
void hu_memory__v1_backend_unregister_all(struct hu_memory *m);

struct hu_memory_slot {
    hu_memory_vtable_t *vt;
    void *ctx;
};

struct hu_memory {
    hu_allocator_t *alloc;
    hu_graph_t *graph; /* not owned; provided at open() */
    struct hu_memory_slot slots[HU_MEM_KIND_MAX];
};

hu_error_t hu_memory_open(hu_allocator_t *alloc, hu_graph_t *graph, hu_memory_t **out) {
    if (alloc == NULL || graph == NULL || out == NULL) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    hu_memory_t *m = alloc->alloc(alloc->ctx, sizeof(hu_memory_t));
    if (m == NULL) {
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(m, 0, sizeof(*m));
    m->alloc = alloc;
    m->graph = graph;

    hu_error_t e = hu_memory__v1_backend_register(m, graph);
    if (e != HU_OK) {
        alloc->free(alloc->ctx, m, sizeof(*m));
        return e;
    }
    *out = m;
    return HU_OK;
}

void hu_memory_close(hu_memory_t *m, hu_allocator_t *alloc) {
    if (m == NULL) return;
    hu_memory__v1_backend_unregister_all(m);
    for (int i = 0; i < HU_MEM_KIND_MAX; i++) {
        struct hu_memory_slot *s = &m->slots[i];
        if (s->vt && s->vt->deinit && s->ctx) {
            s->vt->deinit(s->ctx);
        }
        s->vt = NULL;
        s->ctx = NULL;
    }
    alloc->free(alloc->ctx, m, sizeof(*m));
}

hu_error_t hu_memory_register_backend(hu_memory_t *m, hu_memory_kind_t kind,
                                      hu_memory_vtable_t *vt, void *ctx) {
    if (m == NULL || vt == NULL || (int)kind < 0 || kind >= HU_MEM_KIND_MAX) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    struct hu_memory_slot *s = &m->slots[kind];
    if (s->vt && s->vt->deinit && s->ctx) {
        s->vt->deinit(s->ctx);
    }
    s->vt = vt;
    s->ctx = ctx;
    return HU_OK;
}

static inline struct hu_memory_slot *slot_for(hu_memory_t *m, hu_memory_kind_t kind) {
    if (m == NULL || (int)kind < 0 || kind >= HU_MEM_KIND_MAX) return NULL;
    struct hu_memory_slot *s = &m->slots[kind];
    if (s->vt == NULL) return NULL;
    return s;
}

hu_error_t hu_memory_read(hu_memory_t *m, const hu_memory_query_t *q, hu_allocator_t *alloc,
                          hu_memory_record_t **out, size_t *out_count) {
    if (m == NULL || q == NULL || alloc == NULL || out == NULL || out_count == NULL) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    struct hu_memory_slot *s = slot_for(m, q->kind);
    if (s == NULL || s->vt->read == NULL) return HU_ERR_NOT_SUPPORTED;
    return s->vt->read(s->ctx, q, alloc, out, out_count);
}

hu_error_t hu_memory_write(hu_memory_t *m, const hu_memory_record_t *rec) {
    if (m == NULL || rec == NULL) return HU_ERR_INVALID_ARGUMENT;
    struct hu_memory_slot *s = slot_for(m, rec->kind);
    if (s == NULL || s->vt->write == NULL) return HU_ERR_NOT_SUPPORTED;
    return s->vt->write(s->ctx, rec);
}

hu_error_t hu_memory_erase(hu_memory_t *m, hu_memory_kind_t kind, int64_t id) {
    if (m == NULL) return HU_ERR_INVALID_ARGUMENT;
    struct hu_memory_slot *s = slot_for(m, kind);
    if (s == NULL || s->vt->erase == NULL) return HU_ERR_NOT_SUPPORTED;
    return s->vt->erase(s->ctx, kind, id);
}

hu_error_t hu_memory_purge_by_provenance(hu_memory_t *m, const char *substring, size_t len) {
    if (m == NULL || substring == NULL || len == 0) return HU_ERR_INVALID_ARGUMENT;
    /* Fan out to every registered backend; first error wins, but we still
     * call the rest so that erasure is best-effort across backends. The W4
     * v1 helper hu_memory_erase_by_provenance does this for the graph; here
     * we extend it to every backend that implements the hook. */
    hu_error_t first_err = HU_OK;
    bool any_attempted = false;
    for (int i = 0; i < HU_MEM_KIND_MAX; i++) {
        struct hu_memory_slot *s = &m->slots[i];
        if (s->vt == NULL || s->vt->erase_by_provenance == NULL) continue;
        any_attempted = true;
        hu_error_t e = s->vt->erase_by_provenance(s->ctx, substring, len);
        if (e != HU_OK && first_err == HU_OK) first_err = e;
    }
    if (!any_attempted) return HU_ERR_NOT_SUPPORTED;
    return first_err;
}

void hu_memory_records_free(hu_memory_t *m, hu_allocator_t *alloc,
                            hu_memory_record_t *r, size_t n) {
    if (m == NULL || r == NULL || n == 0) return;
    /* All records in a single response come from the same backend, identified
     * by the kind of the first record. Mixing kinds in one read is not
     * permitted (the API only routes a single kind per call). */
    struct hu_memory_slot *s = slot_for(m, r[0].kind);
    if (s == NULL || s->vt->records_free == NULL) return;
    s->vt->records_free(s->ctx, alloc, r, n);
}

const char *hu_memory_backend_name(hu_memory_t *m, hu_memory_kind_t kind) {
    if (m == NULL || (int)kind < 0 || kind >= HU_MEM_KIND_MAX) return NULL;
    struct hu_memory_slot *s = &m->slots[kind];
    return (s->vt != NULL) ? s->vt->name : NULL;
}

hu_graph_t *hu_memory_graph_handle(hu_memory_t *m) {
    return (m != NULL) ? m->graph : NULL;
}
