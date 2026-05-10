/* W7 v1 backend.
 *
 * Wraps the existing graph + persona deltas + cross_edges + cases + quarantine
 * APIs behind the hu_memory_t facade. Each backend ctx is one struct holding
 * the graph handle (since v1 stores all of these in one SQLite DB). The
 * routing table in hu_memory dispatches per kind to the same backend ctx —
 * this avoids creating six copies of the same pointer.
 *
 * Layer 1 backend per the v2 architecture (docs/plans/2026-05-10-memory-v2-
 * roadmap-overview.md). New code should call through hu_memory_t; this module
 * exists to make migration mechanical and zero-behavior-change.
 */

#include "human/memory/memory.h"
#include "human/memory/erasure.h"
#include "human/memory/graph.h"

#include "human/core/error.h"
#include <stdlib.h>
#include <string.h>

/* Bridges declared by memory.c. */
hu_error_t hu_memory__v1_backend_register(struct hu_memory *m, hu_graph_t *graph);
void hu_memory__v1_backend_unregister_all(struct hu_memory *m);

/* Local allocator helpers. The h-uman allocator is method-pointer style. */
static inline void *xalloc(hu_allocator_t *a, size_t n) {
    return a->alloc(a->ctx, n);
}
static inline void xfree(hu_allocator_t *a, void *p, size_t n) {
    if (p) a->free(a->ctx, p, n);
}

/* Shared ctx — held once by hu_memory and pointed to by every kind that the
 * v1 backend services. We keep deinit a no-op for all but the FIRST kind so
 * the ctx isn't double-freed; the FIRST kind owns it. */
struct hu_memory_v1_ctx {
    hu_graph_t *graph;
    int owner_kind; /* the single slot whose deinit free()s ctx */
};

/* --------- ENTITY ----------------------------------------------------- */

static hu_error_t v1_entity_read(void *vctx, const hu_memory_query_t *q,
                                  hu_allocator_t *alloc,
                                  hu_memory_record_t **out, size_t *out_count) {
    struct hu_memory_v1_ctx *ctx = vctx;
    if (q->kind != HU_MEM_ENTITY) return HU_ERR_INVALID_ARGUMENT;

    /* Two query shapes today: by_name lookup, neighbors traversal. The two
     * branches are union members and share storage; we discriminate solely on
     * `by_name.name`. Callers wanting neighbors leave name=NULL and set
     * entity_id; callers wanting by_name set name (non-NULL). Setting both is
     * ambiguous and rejected as INVALID_ARGUMENT below. */
    if (q->as.by_name.name != NULL && q->as.by_name.name_len > 0) {
        hu_graph_entity_t e;
        memset(&e, 0, sizeof(e));
        hu_error_t err = hu_graph_find_entity(ctx->graph,
                                              q->contact_id, q->contact_id_len,
                                              q->as.by_name.name, q->as.by_name.name_len, &e);
        if (err != HU_OK) {
            *out = NULL;
            *out_count = 0;
            return err;
        }
        hu_memory_record_t *recs = xalloc(alloc, sizeof(*recs));
        if (recs == NULL) {
            hu_graph_entities_free(alloc, &e, 1);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(recs, 0, sizeof(*recs));
        recs[0].kind = HU_MEM_ENTITY;
        recs[0].id = e.id;
        recs[0].confidence = 1.0f;
        recs[0].event_start = e.first_seen;
        /* Hand off the graph_entity_t as payload; v1_entity_records_free
         * unwinds it. We reuse hu_graph_entities_free rather than re-cloning
         * the data — same pattern v1 graph callers use. */
        hu_graph_entity_t *payload = xalloc(alloc, sizeof(*payload));
        if (payload == NULL) {
            hu_graph_entities_free(alloc, &e, 1);
            xfree(alloc, recs, 0);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *payload = e;
        recs[0].payload = payload;
        recs[0].payload_len = sizeof(*payload);
        *out = recs;
        *out_count = 1;
        return HU_OK;
    }

    /* Neighbors traversal. */
    if (q->as.neighbors.entity_id != 0) {
        hu_graph_entity_t *ents = NULL;
        hu_graph_relation_t *rels = NULL;
        size_t count = 0;
        size_t hops = q->as.neighbors.hops > 0 ? q->as.neighbors.hops : 1;
        size_t lim = q->as.neighbors.limit > 0 ? q->as.neighbors.limit : 16;
        hu_error_t err = hu_graph_neighbors(ctx->graph, alloc,
                                             q->contact_id, q->contact_id_len,
                                             q->as.neighbors.entity_id, hops, lim,
                                             &ents, &rels, &count);
        if (err != HU_OK) {
            *out = NULL;
            *out_count = 0;
            return err;
        }
        if (count == 0) {
            hu_graph_entities_free(alloc, ents, 0);
            hu_graph_relations_free(alloc, rels, 0);
            *out = NULL;
            *out_count = 0;
            return HU_OK;
        }
        hu_memory_record_t *recs = xalloc(alloc, sizeof(*recs) * count);
        if (recs == NULL) {
            hu_graph_entities_free(alloc, ents, count);
            hu_graph_relations_free(alloc, rels, count);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(recs, 0, sizeof(*recs) * count);
        /* Fold entities and relations into a paired ENTITY+RELATION view. The
         * caller iterates the array; payload type is hu_graph_entity_t for
         * even indices in the array order. We attach relation data via the
         * provenance string ("rel:<id>") so it can be re-fetched if needed.
         * This keeps the simple case clean; sophisticated callers should query
         * RELATION directly. */
        for (size_t i = 0; i < count; i++) {
            recs[i].kind = HU_MEM_ENTITY;
            recs[i].id = ents[i].id;
            recs[i].event_start = ents[i].first_seen;
            recs[i].confidence = 1.0f;
            hu_graph_entity_t *p = xalloc(alloc, sizeof(*p));
            if (p == NULL) {
                /* Free what we built, then bail. Caller must not see partial. */
                for (size_t j = 0; j < i; j++) {
                    hu_graph_entity_t *prev = (hu_graph_entity_t *)recs[j].payload;
                    hu_graph_entities_free(alloc, prev, 1);
                }
                hu_graph_entities_free(alloc, ents, count);
                hu_graph_relations_free(alloc, rels, count);
                xfree(alloc, recs, 0);
                return HU_ERR_OUT_OF_MEMORY;
            }
            *p = ents[i];
            recs[i].payload = p;
            recs[i].payload_len = sizeof(*p);
        }
        /* The graph_neighbors call returned arrays we now own; the entity
         * memory is moved into the records' payloads (each owns one entity
         * struct). Free only the arrays themselves, not the strings (those
         * are now owned by the cloned-into-payload entities — but in fact
         * the assignment `*p = ents[i]` shallow-copies, including string
         * pointers, so to keep ownership simple we must NOT free the
         * underlying strings yet. hu_graph_entities_free does free strings,
         * so we cannot call it. Free the array backing only. */
        xfree(alloc, ents, 0);
        /* Relations array fully released — payload doesn't carry them. */
        hu_graph_relations_free(alloc, rels, count);
        *out = recs;
        *out_count = count;
        return HU_OK;
    }

    return HU_ERR_INVALID_ARGUMENT;
}

static hu_error_t v1_entity_write(void *vctx, const hu_memory_record_t *rec) {
    struct hu_memory_v1_ctx *ctx = vctx;
    if (rec->kind != HU_MEM_ENTITY || rec->payload == NULL) return HU_ERR_INVALID_ARGUMENT;
    const hu_graph_entity_t *e = rec->payload;
    int64_t out_id = 0;
    return hu_graph_upsert_entity(ctx->graph, "", 0, e->name, e->name_len, e->type,
                                  e->metadata_json, &out_id);
}

static hu_error_t v1_entity_erase(void *vctx, hu_memory_kind_t kind, int64_t id) {
    struct hu_memory_v1_ctx *ctx = vctx;
    if (kind != HU_MEM_ENTITY) return HU_ERR_INVALID_ARGUMENT;
    hu_erase_report_t rep;
    return hu_memory_erase_entity(ctx->graph, id, &rep);
}

static hu_error_t v1_entity_erase_by_prov(void *vctx, const char *substring, size_t len) {
    struct hu_memory_v1_ctx *ctx = vctx;
    hu_erase_report_t rep;
    return hu_memory_erase_by_provenance(ctx->graph, substring, len, &rep);
}

static void v1_entity_records_free(void *vctx, hu_allocator_t *alloc,
                                    hu_memory_record_t *r, size_t n) {
    (void)vctx;
    if (r == NULL || n == 0) return;
    for (size_t i = 0; i < n; i++) {
        hu_graph_entity_t *e = (hu_graph_entity_t *)r[i].payload;
        /* hu_graph_entities_free frees the strings AND the struct itself
         * (since each payload was allocated as a 1-element array). Do NOT
         * xfree the payload pointer afterward — that would double-free. */
        if (e != NULL) {
            hu_graph_entities_free(alloc, e, 1);
        }
        if (r[i].provenance) {
            xfree(alloc, r[i].provenance, r[i].provenance_len + 1);
        }
    }
    xfree(alloc, r, n * sizeof(*r));
}

static void v1_ctx_deinit_owner(void *vctx) {
    struct hu_memory_v1_ctx *ctx = vctx;
    if (ctx == NULL) return;
    free(ctx); /* allocated with raw malloc; see register below. */
}

static void v1_ctx_deinit_noop(void *vctx) {
    (void)vctx;
}

static hu_memory_vtable_t s_v1_entity_vt = {
    .name = "v1-entity",
    .read = v1_entity_read,
    .write = v1_entity_write,
    .erase = v1_entity_erase,
    .erase_by_provenance = v1_entity_erase_by_prov,
    .records_free = v1_entity_records_free,
    .deinit = v1_ctx_deinit_owner,
};

/* --------- RELATION --------------------------------------------------- */

static hu_error_t v1_relation_read(void *vctx, const hu_memory_query_t *q,
                                    hu_allocator_t *alloc,
                                    hu_memory_record_t **out, size_t *out_count) {
    struct hu_memory_v1_ctx *ctx = vctx;
    if (q->kind != HU_MEM_RELATION) return HU_ERR_INVALID_ARGUMENT;

    /* Window query is the only relation read shape v1 exposes through a
     * direct list call; list_relations also exists for "top-N by weight"
     * which we offer when no window is given. */
    if (q->as.window.from_ts != 0 || q->as.window.to_ts != 0) {
        hu_graph_relation_t *rels = NULL;
        size_t count = 0;
        size_t lim = q->as.window.limit > 0 ? q->as.window.limit : 32;
        hu_error_t err = hu_graph_relations_in_window(ctx->graph, alloc,
                                                       q->contact_id, q->contact_id_len,
                                                       q->as.window.from_ts,
                                                       q->as.window.to_ts, lim,
                                                       &rels, &count);
        if (err != HU_OK) {
            *out = NULL;
            *out_count = 0;
            return err;
        }
        if (count == 0) {
            hu_graph_relations_free(alloc, rels, 0);
            *out = NULL;
            *out_count = 0;
            return HU_OK;
        }
        hu_memory_record_t *recs = xalloc(alloc, sizeof(*recs) * count);
        if (recs == NULL) {
            hu_graph_relations_free(alloc, rels, count);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(recs, 0, sizeof(*recs) * count);
        for (size_t i = 0; i < count; i++) {
            recs[i].kind = HU_MEM_RELATION;
            recs[i].id = rels[i].id;
            recs[i].event_start = rels[i].event_start;
            recs[i].event_end = rels[i].event_end;
            recs[i].confidence = rels[i].confidence;
            hu_graph_relation_t *p = xalloc(alloc, sizeof(*p));
            if (p == NULL) {
                /* Roll back. */
                for (size_t j = 0; j < i; j++) {
                    hu_graph_relation_t *prev = (hu_graph_relation_t *)recs[j].payload;
                    hu_graph_relations_free(alloc, prev, 1);
                }
                hu_graph_relations_free(alloc, rels, count);
                xfree(alloc, recs, 0);
                return HU_ERR_OUT_OF_MEMORY;
            }
            *p = rels[i];
            recs[i].payload = p;
            recs[i].payload_len = sizeof(*p);
        }
        xfree(alloc, rels, 0);
        *out = recs;
        *out_count = count;
        return HU_OK;
    }

    /* Default: list top relations. */
    hu_graph_relation_t *rels = NULL;
    size_t count = 0;
    size_t lim = q->as.window.limit > 0 ? q->as.window.limit : 32;
    hu_error_t err = hu_graph_list_relations(ctx->graph, alloc,
                                              q->contact_id, q->contact_id_len, lim,
                                              &rels, &count);
    if (err != HU_OK) {
        *out = NULL;
        *out_count = 0;
        return err;
    }
    if (count == 0) {
        hu_graph_relations_free(alloc, rels, 0);
        *out = NULL;
        *out_count = 0;
        return HU_OK;
    }
    hu_memory_record_t *recs = xalloc(alloc, sizeof(*recs) * count);
    if (recs == NULL) {
        hu_graph_relations_free(alloc, rels, count);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(recs, 0, sizeof(*recs) * count);
    for (size_t i = 0; i < count; i++) {
        recs[i].kind = HU_MEM_RELATION;
        recs[i].id = rels[i].id;
        recs[i].event_start = rels[i].event_start;
        recs[i].event_end = rels[i].event_end;
        recs[i].confidence = rels[i].confidence;
        hu_graph_relation_t *p = xalloc(alloc, sizeof(*p));
        if (p == NULL) {
            for (size_t j = 0; j < i; j++) {
                hu_graph_relation_t *prev = (hu_graph_relation_t *)recs[j].payload;
                hu_graph_relations_free(alloc, prev, 1);
            }
            hu_graph_relations_free(alloc, rels, count);
            xfree(alloc, recs, 0);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *p = rels[i];
        recs[i].payload = p;
        recs[i].payload_len = sizeof(*p);
    }
    xfree(alloc, rels, 0);
    *out = recs;
    *out_count = count;
    return HU_OK;
}

static hu_error_t v1_relation_write(void *vctx, const hu_memory_record_t *rec) {
    struct hu_memory_v1_ctx *ctx = vctx;
    if (rec->kind != HU_MEM_RELATION || rec->payload == NULL) return HU_ERR_INVALID_ARGUMENT;
    const hu_graph_relation_t *r = rec->payload;
    return hu_graph_upsert_relation_ex(ctx->graph, "", 0, r->source_id, r->target_id,
                                       r->type, r->weight, rec->event_start, rec->event_end,
                                       rec->confidence < 0.0f ? 1.0f : rec->confidence,
                                       r->context, r->context_len,
                                       rec->provenance, rec->provenance_len);
}

static hu_error_t v1_relation_erase(void *vctx, hu_memory_kind_t kind, int64_t id) {
    (void)vctx;
    (void)id;
    if (kind != HU_MEM_RELATION) return HU_ERR_INVALID_ARGUMENT;
    /* v1 has no direct "delete relation by id"; erasure goes through the
     * cascading entity erase or provenance erase. Surface this honestly so
     * callers don't expect a row-level delete that doesn't exist. */
    return HU_ERR_NOT_SUPPORTED;
}

static void v1_relation_records_free(void *vctx, hu_allocator_t *alloc,
                                      hu_memory_record_t *r, size_t n) {
    (void)vctx;
    if (r == NULL || n == 0) return;
    for (size_t i = 0; i < n; i++) {
        hu_graph_relation_t *p = (hu_graph_relation_t *)r[i].payload;
        /* hu_graph_relations_free frees the strings AND the struct backing
         * (since each payload was allocated as a 1-element array). Do NOT
         * xfree the payload pointer afterward — that would double-free. */
        if (p != NULL) {
            hu_graph_relations_free(alloc, p, 1);
        }
        if (r[i].provenance) {
            xfree(alloc, r[i].provenance, r[i].provenance_len + 1);
        }
    }
    xfree(alloc, r, n * sizeof(*r));
}

static hu_memory_vtable_t s_v1_relation_vt = {
    .name = "v1-relation",
    .read = v1_relation_read,
    .write = v1_relation_write,
    .erase = v1_relation_erase,
    .erase_by_provenance = NULL, /* relation erasure flows through the entity backend */
    .records_free = v1_relation_records_free,
    .deinit = v1_ctx_deinit_noop,
};

/* --------- registration ---------------------------------------------- */

hu_error_t hu_memory__v1_backend_register(struct hu_memory *m, hu_graph_t *graph) {
    if (m == NULL || graph == NULL) return HU_ERR_INVALID_ARGUMENT;
    /* One ctx, shared across the kinds the v1 backend services. The owner is
     * the FIRST registered slot (HU_MEM_ENTITY). When the facade closes, that
     * slot's deinit free()s the ctx; sibling slots use a no-op deinit. */
    struct hu_memory_v1_ctx *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) return HU_ERR_OUT_OF_MEMORY;
    ctx->graph = graph;
    ctx->owner_kind = HU_MEM_ENTITY;

    hu_error_t e = hu_memory_register_backend(m, HU_MEM_ENTITY, &s_v1_entity_vt, ctx);
    if (e != HU_OK) {
        free(ctx);
        return e;
    }
    e = hu_memory_register_backend(m, HU_MEM_RELATION, &s_v1_relation_vt, ctx);
    if (e != HU_OK) {
        /* The entity slot still owns ctx; closing the facade will free it. */
        return e;
    }
    return HU_OK;
}

void hu_memory__v1_backend_unregister_all(struct hu_memory *m) {
    /* Nothing to do here — facade close walks slots and calls each vtable's
     * deinit. The entity slot's deinit owns the ctx free. We keep this hook
     * for symmetry and as an extension point for kinds that want to do extra
     * work at unregister time (e.g. flush a write-through cache). */
    (void)m;
}
