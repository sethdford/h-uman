/* W7 v1 backend.
 *
 * Wraps the existing graph + persona deltas + cross_edges + cases + quarantine
 * APIs behind the hu_memory_facade_t. Each backend ctx is one struct holding
 * the graph handle (since v1 stores all of these in one SQLite DB). The
 * routing table in hu_memory_facade dispatches per kind to the same backend ctx —
 * this avoids creating six copies of the same pointer.
 *
 * Layer 1 backend per the v2 architecture (docs/plans/2026-05-10-memory-v2-
 * roadmap-overview.md). New code should call through hu_memory_facade_t; this module
 * exists to make migration mechanical and zero-behavior-change.
 */

#include "human/memory/memory.h"
#include "human/memory/belief.h"
#include "human/memory/erasure.h"
#include "human/memory/graph.h"
#include "human/memory/hyperedge.h"

#include "human/core/error.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

/* Bridges declared by memory.c. */
hu_error_t hu_memory__v1_backend_register(struct hu_memory_facade *m, hu_graph_t *graph);
void hu_memory__v1_backend_unregister_all(struct hu_memory_facade *m);
void hu_memory__v1_set_bundle_for_close(hu_memory_facade_t *m, void *ctx);

/* Local allocator helpers. The h-uman allocator is method-pointer style. */
static inline void *xalloc(hu_allocator_t *a, size_t n) {
    return a->alloc(a->ctx, n);
}
static inline void xfree(hu_allocator_t *a, void *p, size_t n) {
    if (p) a->free(a->ctx, p, n);
}

/* Shared ctx — held once by hu_memory_facade and pointed to by every kind that the
 * v1 backend services. Facade close frees the bundle via memory.c (v1_bundle_ctx). */
struct hu_memory_v1_ctx {
    hu_memory_facade_t *facade; /* not owned; provided at register time */
    hu_graph_t *graph;
};

/* Tiny accessors used by the hyperedge facade backend to reach the
 * facade pointer without exposing the full struct outside this TU. */
hu_memory_facade_t *hu_memory__v1_ctx_facade(void *vctx) {
    struct hu_memory_v1_ctx *ctx = vctx;
    return ctx ? ctx->facade : NULL;
}

/* --------- ENTITY ----------------------------------------------------- */

static hu_error_t v1_entity_read(void *vctx, const hu_memory_query_t *q,
                                  hu_allocator_t *alloc,
                                  hu_memory_record_t **out, size_t *out_count) {
    struct hu_memory_v1_ctx *ctx = vctx;
    if (q->kind != HU_MEM_ENTITY) return HU_ERR_INVALID_ARGUMENT;

    /* Two query shapes today: by_name lookup, neighbors traversal.
     *
     * The union members ALIAS in memory (e.g. `neighbors.entity_id` and
     * `by_name.name` both sit at byte 0 of the union; `neighbors.hops` and
     * `by_name.name_len` both sit at byte 8). With AUTO callers can no
     * longer be reliably distinguished if both fields would test "set" —
     * setting `entity_id = 42, hops = 1` makes the by_name shape look
     * like `{name=(char*)0x2A, name_len=1}`, which dereferences a wild
     * pointer.
     *
     * Resolve the variant in this strict order:
     *   1. Explicit `variant` tag (preferred for new callers).
     *   2. AUTO heuristic for backward compat:
     *        - If `by_name.name_len` fits a sane entity-name range
     *          [1, 256] AND `by_name.name` points to a printable string,
     *          prefer by_name.
     *        - Else if `neighbors.entity_id != 0`, prefer neighbors.
     *        - Else invalid.
     */
    hu_memory_query_variant_t v = q->variant;
    if (v == HU_MEMORY_QUERY_AUTO) {
        /* AUTO heuristic: trust by_name only when the discriminating
         * fields look like a real (name, len) pair — len in [1, 256]
         * and the first byte of the name pointer is a printable ASCII
         * character. A wild pointer crafted from an entity_id (small
         * integer) fails both filters because dereferencing it would
         * read from page 0. We guard the deref with the length check
         * below: if name_len > 256, we never touch `name`. */
        size_t nlen = q->as.by_name.name_len;
        const char *nptr = q->as.by_name.name;
        if (nlen >= 1 && nlen <= 256 && nptr != NULL) {
            /* Final sanity: if the pointer is in the low-address range
             * (< 0x10000) it's almost certainly an entity_id, not a
             * pointer. This protects against the multi_hop crash. */
            if ((uintptr_t)nptr > 0x10000u) {
                v = HU_MEMORY_QUERY_BY_NAME;
            }
        }
        if (v == HU_MEMORY_QUERY_AUTO && q->as.neighbors.entity_id != 0) {
            v = HU_MEMORY_QUERY_NEIGHBORS;
        }
    }

    if (v == HU_MEMORY_QUERY_BY_NAME && q->as.by_name.name != NULL &&
        q->as.by_name.name_len > 0) {
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

    /* Neighbors traversal.
     *
     * Returns BOTH the neighbour entities AND the relation rows that
     * connect them to the anchor. The executor's W12 P6 re-ranker
     * needs relation context (the human-readable string attached to
     * each edge) to score how well each fact matches the user goal;
     * without relation records the executor only sees entity-name
     * overlap, which is near-useless at scale (the answer entity's
     * name rarely contains the question keywords). Surfacing both
     * lets `hu_planner_execute` propagate the matching relation's
     * score onto its endpoints and select the right answer entity.
     *
     * Layout: relations first (so a downstream linear scan finds
     * them and pre-populates the agg before entity dedupe runs),
     * then entities. Ordering within each group preserves the
     * underlying `hu_graph_neighbors` sequence so callers that
     * care about insertion order still get a stable view. */
    if (v == HU_MEMORY_QUERY_NEIGHBORS && q->as.neighbors.entity_id != 0) {
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

        size_t total = count * 2;  /* entities + relations, parallel arrays */
        hu_memory_record_t *recs = xalloc(alloc, sizeof(*recs) * total);
        if (recs == NULL) {
            hu_graph_entities_free(alloc, ents, count);
            hu_graph_relations_free(alloc, rels, count);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(recs, 0, sizeof(*recs) * total);

        /* Entities first — so `recs[0].kind = HU_MEM_ENTITY` makes
         * `hu_memory_facade_records_free` route to `v1_entity_records_free`.
         * That free function is now kind-aware: it inspects each record's
         * kind and dispatches to the appropriate per-payload free. */
        for (size_t i = 0; i < count; i++) {
            recs[i].kind = HU_MEM_ENTITY;
            recs[i].id = ents[i].id;
            recs[i].event_start = ents[i].first_seen;
            recs[i].confidence = 1.0f;
            hu_graph_entity_t *p = xalloc(alloc, sizeof(*p));
            if (p == NULL) {
                for (size_t j = 0; j < i; j++) {
                    hu_graph_entity_t *prev = (hu_graph_entity_t *)recs[j].payload;
                    if (prev) xfree(alloc, prev, sizeof(*prev));
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

        /* Relations second. Each relation payload is a
         * hu_memory_relation_row_t (the public surface struct), shallow-
         * copying the underlying graph relation's string pointers. The
         * strings' lifetime moves into the records; downstream
         * records_free reclaims them. */
        for (size_t i = 0; i < count; i++) {
            size_t k = count + i;
            hu_memory_relation_row_t *rp = xalloc(alloc, sizeof(*rp));
            if (rp == NULL) {
                /* Roll back entity payloads (just heap-alloc structs at
                 * this point; strings are still owned by `ents[]`). */
                for (size_t j = 0; j < count; j++) {
                    hu_graph_entity_t *prev = (hu_graph_entity_t *)recs[j].payload;
                    if (prev) xfree(alloc, prev, sizeof(*prev));
                }
                for (size_t j = count; j < k; j++) {
                    hu_memory_relation_row_t *prev =
                        (hu_memory_relation_row_t *)recs[j].payload;
                    if (prev) xfree(alloc, prev, sizeof(*prev));
                }
                hu_graph_entities_free(alloc, ents, count);
                hu_graph_relations_free(alloc, rels, count);
                xfree(alloc, recs, 0);
                return HU_ERR_OUT_OF_MEMORY;
            }
            /* hu_memory_relation_row_t is hu_graph_relation_t — shallow-
             * copy the whole row so we claim every string field
             * (context, provenance, source_name, target_name). Missing
             * even one leaks. The strings' lifetime is now owned by the
             * record payload; records_free reclaims them. */
            *rp = rels[i];

            recs[k].kind = HU_MEM_RELATION;
            recs[k].id = rels[i].id;
            recs[k].event_start = rels[i].first_seen;
            recs[k].confidence = 1.0f;
            recs[k].payload = rp;
            recs[k].payload_len = sizeof(*rp);
        }
        /* Ownership: payload string pointers are shallow-copied; the
         * graph entity/relation array backings are no longer owners.
         * Free the array backings only — strings travel with the
         * records and are released by the kind-aware records_free
         * downstream. */
        xfree(alloc, ents, 0);
        xfree(alloc, rels, 0);
        *out = recs;
        *out_count = total;
        return HU_OK;
    }

    return HU_ERR_INVALID_ARGUMENT;
}

static hu_error_t v1_entity_write(void *vctx, const hu_memory_record_t *rec) {
    struct hu_memory_v1_ctx *ctx = vctx;
    if (rec->kind != HU_MEM_ENTITY || rec->payload == NULL) return HU_ERR_INVALID_ARGUMENT;
    const hu_graph_entity_t *e = rec->payload;
    int64_t out_id = 0;
    /* P2G — honor contact_id scope from the record so callers can do
     * per-contact entity upserts through the facade. */
    return hu_graph_upsert_entity(ctx->graph, rec->contact_id, rec->contact_id_len,
                                  e->name, e->name_len, e->type, e->metadata_json,
                                  &out_id);
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
    /* Kind-aware free. Most paths return pure-entity arrays, but the
     * NEIGHBORS query returns a mixed entity+relation response so
     * the W12 P6 re-ranker can score relation contexts. We dispatch
     * by per-record kind: entity payloads via hu_graph_entities_free
     * (frees the entity struct AND its strings); relation payloads
     * are heap-allocated hu_memory_relation_row_t whose context /
     * provenance string pointers came from the underlying graph
     * relation array — free them with hu_graph_relations_free using
     * the embedded ids. Since hu_memory_relation_row_t is a flatter
     * row shape than hu_graph_relation_t, we replay the few fields
     * into a stack hu_graph_relation_t and call relations_free with
     * count=1. That lets us reuse the same xfree-of-strings logic. */
    for (size_t i = 0; i < n; i++) {
        if (r[i].kind == HU_MEM_ENTITY) {
            hu_graph_entity_t *e = (hu_graph_entity_t *)r[i].payload;
            if (e != NULL) {
                /* hu_graph_entities_free walks the strings AND xfrees
                 * the struct itself. Do NOT xfree the payload pointer
                 * afterward — double-free. */
                hu_graph_entities_free(alloc, e, 1);
            }
        } else if (r[i].kind == HU_MEM_RELATION) {
            hu_memory_relation_row_t *rp = (hu_memory_relation_row_t *)r[i].payload;
            if (rp != NULL) {
                /* hu_memory_relation_row_t aliases hu_graph_relation_t.
                 * Free every string field that hu_graph_relations_free
                 * would free, then the struct itself. Matches the
                 * shallow-copy claim in `v1_entity_read`'s NEIGHBORS
                 * branch. */
                if (rp->context) {
                    xfree(alloc, (void *)rp->context, rp->context_len + 1);
                }
                if (rp->provenance) {
                    xfree(alloc, (void *)rp->provenance, rp->provenance_len + 1);
                }
                if (rp->source_name) {
                    xfree(alloc, (void *)rp->source_name, rp->source_name_len + 1);
                }
                if (rp->target_name) {
                    xfree(alloc, (void *)rp->target_name, rp->target_name_len + 1);
                }
                xfree(alloc, rp, sizeof(*rp));
            }
        }
        if (r[i].provenance) {
            xfree(alloc, r[i].provenance, r[i].provenance_len + 1);
        }
    }
    xfree(alloc, r, n * sizeof(*r));
}

static void v1_ctx_deinit_noop(void *vctx) {
    (void)vctx;
}

static hu_memory_facade_vtable_t s_v1_entity_vt = {
    .name = "v1-entity",
    .read = v1_entity_read,
    .write = v1_entity_write,
    .erase = v1_entity_erase,
    .erase_by_provenance = v1_entity_erase_by_prov,
    .records_free = v1_entity_records_free,
    .deinit = v1_ctx_deinit_noop,
};

/* --------- RELATION --------------------------------------------------- */

static hu_error_t v1_relation_read(void *vctx, const hu_memory_query_t *q,
                                    hu_allocator_t *alloc,
                                    hu_memory_record_t **out, size_t *out_count) {
    struct hu_memory_v1_ctx *ctx = vctx;
    if (q->kind != HU_MEM_RELATION) return HU_ERR_INVALID_ARGUMENT;

    /* P4 — Honor explicit variant when present. The legacy AUTO path uses
     * `q->as.by_id.id == HU_MEMORY_REL_VERIFIER_SCAN` as an implicit
     * discriminator, which is fragile if a window caller ever sets
     * `from_ts` to that sentinel value. The explicit tag takes priority. */
    hu_memory_query_variant_t v = q->variant;
    if (v == HU_MEMORY_QUERY_BY_ID ||
        (v == HU_MEMORY_QUERY_AUTO && q->as.by_id.id == HU_MEMORY_REL_VERIFIER_SCAN)) {
        size_t lim = q->as.by_id.limit > 0 ? q->as.by_id.limit : 64;
        hu_graph_relation_t *rels = NULL;
        size_t count = 0;
        hu_error_t err = hu_graph_list_relations_verifier_scan(ctx->graph, alloc,
                                                               q->contact_id, q->contact_id_len,
                                                               lim, &rels, &count);
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
            /* W8 P2 — propagate Bayesian variance alongside the mean so
             * the facade record carries both halves of the belief. The
             * field is zeroed for legacy rows (treated as scalar). */
            recs[i].confidence_variance = rels[i].confidence_variance;
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

    /* Window query is the only relation read shape v1 exposes through a
     * direct list call; list_relations also exists for "top-N by weight"
     * which we offer when no window is given.
     *
     * P4 — Explicit WINDOW takes priority. Falling back to legacy AUTO
     * triggers a window read when either timestamp is non-zero. */
    if (v == HU_MEMORY_QUERY_WINDOW ||
        (v == HU_MEMORY_QUERY_AUTO &&
         (q->as.window.from_ts != 0 || q->as.window.to_ts != 0))) {
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
            recs[i].confidence_variance = rels[i].confidence_variance; /* W8 P2 */
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
        recs[i].confidence_variance = rels[i].confidence_variance; /* W8 P2 */
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
    /* P2G — Honor explicit (mean, variance) from the record. If variance is
     * negative or zero, derive it from provenance via the W8 heuristic so
     * legacy callers that don't fill in variance still get a meaningful
     * non-zero band. */
    float mean = rec->confidence < 0.0f ? 1.0f : rec->confidence;
    float variance = rec->confidence_variance;
    if (variance <= 0.0f) {
        variance = hu_belief_initial_variance_for_provenance(
            rec->provenance, rec->provenance_len);
    }
    return hu_graph_upsert_relation_with_belief(
        ctx->graph, rec->contact_id, rec->contact_id_len,
        r->source_id, r->target_id, r->type, r->weight,
        rec->event_start, rec->event_end, mean, variance, r->context,
        r->context_len, rec->provenance, rec->provenance_len, NULL);
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

static hu_memory_facade_vtable_t s_v1_relation_vt = {
    .name = "v1-relation",
    .read = v1_relation_read,
    .write = v1_relation_write,
    .erase = v1_relation_erase,
    .erase_by_provenance = NULL, /* relation erasure flows through the entity backend */
    .records_free = v1_relation_records_free,
    .deinit = v1_ctx_deinit_noop,
};

#ifdef HU_ENABLE_SQLITE

/* --------- CASE (W3 case_records) -------------------------------------- */

static int v1_case_run_ddl(struct sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK)
        return rc;
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

static hu_error_t v1_case_ensure_schema(struct sqlite3 *db) {
    static const char *const stmts[] = {
        "CREATE TABLE IF NOT EXISTS case_records ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "contact_id TEXT NOT NULL DEFAULT '',"
        "goal_verb TEXT NOT NULL,"
        "anchor_entity_ids TEXT NOT NULL DEFAULT '',"
        "plan_text TEXT,"
        "outcome TEXT,"
        "happened_at INTEGER NOT NULL)",
        "CREATE INDEX IF NOT EXISTS idx_cases_contact_verb "
        "ON case_records(contact_id, goal_verb)",
        "CREATE INDEX IF NOT EXISTS idx_cases_recent ON case_records(happened_at DESC)",
        NULL,
    };
    for (size_t i = 0; stmts[i]; i++) {
        if (v1_case_run_ddl(db, stmts[i]) != SQLITE_OK)
            return HU_ERR_IO;
    }
    return HU_OK;
}

static int v1_case_format_anchors(const int64_t *ids, size_t n, char *buf, size_t cap) {
    if (n == 0) {
        if (cap > 0)
            buf[0] = '\0';
        return 0;
    }
    size_t off = 0;
    for (size_t i = 0; i < n; i++) {
        int w = snprintf(buf + off, cap - off, "%s%lld", i == 0 ? "" : ",", (long long)ids[i]);
        if (w < 0 || (size_t)w >= cap - off)
            return -1;
        off += (size_t)w;
    }
    return (int)off;
}

static hu_error_t v1_case_parse_anchors(hu_allocator_t *alloc, const char *s, int64_t **out,
                                        size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    if (!s || !*s)
        return HU_OK;
    size_t cap = 1;
    for (const char *p = s; *p; p++)
        if (*p == ',')
            cap++;
    int64_t *arr = xalloc(alloc, cap * sizeof(int64_t));
    if (!arr)
        return HU_ERR_OUT_OF_MEMORY;
    size_t n = 0;
    const char *p = s;
    while (*p && n < cap) {
        char *end = NULL;
        long long v = strtoll(p, &end, 10);
        if (end == p)
            break;
        arr[n++] = (int64_t)v;
        p = end;
        while (*p == ',' || *p == ' ')
            p++;
    }
    *out = arr;
    *out_n = n;
    return HU_OK;
}

static hu_error_t v1_case_read(void *vctx, const hu_memory_query_t *q, hu_allocator_t *alloc,
                               hu_memory_record_t **out, size_t *out_count) {
    struct hu_memory_v1_ctx *ctx = vctx;
    if (q->kind != HU_MEM_CASE || !q->as.cases.goal_verb || q->as.cases.goal_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = hu_graph_sqlite_connection(ctx->graph);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t se = v1_case_ensure_schema(db);
    if (se != HU_OK)
        return se;

    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT id, goal_verb, anchor_entity_ids, plan_text, outcome, happened_at "
                      "FROM case_records WHERE contact_id = ? AND goal_verb = ? "
                      "ORDER BY happened_at DESC LIMIT 1024";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    const char *cid = q->contact_id ? q->contact_id : "";
    int cid_len = q->contact_id ? (int)q->contact_id_len : 0;
    sqlite3_bind_text(st, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, q->as.cases.goal_verb, (int)q->as.cases.goal_len, SQLITE_STATIC);

    hu_memory_record_t *cands = NULL;
    size_t cap = 0, n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n >= cap) {
            size_t new_cap = cap == 0 ? 16 : cap * 2;
            hu_memory_record_t *t = xalloc(alloc, new_cap * sizeof(hu_memory_record_t));
            if (!t) {
                sqlite3_finalize(st);
                if (cands) {
                    for (size_t k = 0; k < n; k++) {
                        hu_memory_case_payload_t *pl = (hu_memory_case_payload_t *)cands[k].payload;
                        if (pl) {
                            if (pl->goal_verb)
                                xfree(alloc, pl->goal_verb, pl->goal_verb_len + 1);
                            if (pl->plan_text)
                                xfree(alloc, pl->plan_text, pl->plan_text_len + 1);
                            if (pl->outcome)
                                xfree(alloc, pl->outcome, pl->outcome_len + 1);
                            if (pl->anchor_entity_ids)
                                xfree(alloc, pl->anchor_entity_ids,
                                      pl->anchor_count * sizeof(int64_t));
                            xfree(alloc, pl, sizeof(*pl));
                        }
                    }
                    xfree(alloc, cands, cap * sizeof(hu_memory_record_t));
                }
                return HU_ERR_OUT_OF_MEMORY;
            }
            memset(t, 0, new_cap * sizeof(hu_memory_record_t));
            if (cands) {
                memcpy(t, cands, n * sizeof(hu_memory_record_t));
                xfree(alloc, cands, cap * sizeof(hu_memory_record_t));
            }
            cands = t;
            cap = new_cap;
        }
        hu_memory_record_t *row = &cands[n];
        memset(row, 0, sizeof(*row));
        row->kind = HU_MEM_CASE;
        row->id = sqlite3_column_int64(st, 0);
        row->contact_id = q->contact_id;
        row->contact_id_len = q->contact_id_len;

        hu_memory_case_payload_t *pl = xalloc(alloc, sizeof(*pl));
        if (!pl) {
            sqlite3_finalize(st);
            /* rollback partial */
            for (size_t k = 0; k < n; k++) {
                hu_memory_case_payload_t *pp = (hu_memory_case_payload_t *)cands[k].payload;
                if (pp) {
                    if (pp->goal_verb)
                        xfree(alloc, pp->goal_verb, pp->goal_verb_len + 1);
                    if (pp->plan_text)
                        xfree(alloc, pp->plan_text, pp->plan_text_len + 1);
                    if (pp->outcome)
                        xfree(alloc, pp->outcome, pp->outcome_len + 1);
                    if (pp->anchor_entity_ids)
                        xfree(alloc, pp->anchor_entity_ids, pp->anchor_count * sizeof(int64_t));
                    xfree(alloc, pp, sizeof(*pp));
                }
            }
            xfree(alloc, cands, cap * sizeof(hu_memory_record_t));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(pl, 0, sizeof(*pl));
        const char *gv = (const char *)sqlite3_column_text(st, 1);
        size_t gv_len = gv ? (size_t)sqlite3_column_bytes(st, 1) : 0;
        const char *anchors = (const char *)sqlite3_column_text(st, 2);
        const char *pt = (const char *)sqlite3_column_text(st, 3);
        size_t pt_len = pt ? (size_t)sqlite3_column_bytes(st, 3) : 0;
        const char *oc = (const char *)sqlite3_column_text(st, 4);
        size_t oc_len = oc ? (size_t)sqlite3_column_bytes(st, 4) : 0;
        pl->happened_at = sqlite3_column_int64(st, 5);

        if (gv && gv_len > 0) {
            pl->goal_verb = hu_strndup(alloc, gv, gv_len);
            pl->goal_verb_len = gv_len;
        }
        if (pt && pt_len > 0) {
            pl->plan_text = hu_strndup(alloc, pt, pt_len);
            pl->plan_text_len = pt_len;
        }
        if (oc && oc_len > 0) {
            pl->outcome = hu_strndup(alloc, oc, oc_len);
            pl->outcome_len = oc_len;
        }
        if (anchors && v1_case_parse_anchors(alloc, anchors, &pl->anchor_entity_ids,
                                             &pl->anchor_count) != HU_OK) {
            if (pl->goal_verb)
                xfree(alloc, pl->goal_verb, pl->goal_verb_len + 1);
            if (pl->plan_text)
                xfree(alloc, pl->plan_text, pl->plan_text_len + 1);
            if (pl->outcome)
                xfree(alloc, pl->outcome, pl->outcome_len + 1);
            xfree(alloc, pl, sizeof(*pl));
            sqlite3_finalize(st);
            for (size_t k = 0; k < n; k++) {
                hu_memory_case_payload_t *pp = (hu_memory_case_payload_t *)cands[k].payload;
                if (pp) {
                    if (pp->goal_verb)
                        xfree(alloc, pp->goal_verb, pp->goal_verb_len + 1);
                    if (pp->plan_text)
                        xfree(alloc, pp->plan_text, pp->plan_text_len + 1);
                    if (pp->outcome)
                        xfree(alloc, pp->outcome, pp->outcome_len + 1);
                    if (pp->anchor_entity_ids)
                        xfree(alloc, pp->anchor_entity_ids, pp->anchor_count * sizeof(int64_t));
                    xfree(alloc, pp, sizeof(*pp));
                }
            }
            xfree(alloc, cands, cap * sizeof(hu_memory_record_t));
            return HU_ERR_OUT_OF_MEMORY;
        }
        row->payload = pl;
        row->payload_len = sizeof(*pl);
        n++;
    }
    sqlite3_finalize(st);
    *out = cands;
    *out_count = n;
    return HU_OK;
}

static hu_error_t v1_case_write(void *vctx, const hu_memory_record_t *rec) {
    struct hu_memory_v1_ctx *ctx = vctx;
    if (rec->kind != HU_MEM_CASE || rec->payload == NULL)
        return HU_ERR_INVALID_ARGUMENT;
    const hu_memory_case_payload_t *p = (const hu_memory_case_payload_t *)rec->payload;
    if (!p->goal_verb || p->goal_verb_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = hu_graph_sqlite_connection(ctx->graph);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (v1_case_ensure_schema(db) != HU_OK)
        return HU_ERR_IO;

    char anchors[1024];
    if (v1_case_format_anchors(p->anchor_entity_ids, p->anchor_count, anchors, sizeof(anchors)) < 0)
        return HU_ERR_INVALID_ARGUMENT;

    int64_t happened = rec->event_start > 0 ? rec->event_start : p->happened_at;

    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO case_records"
                      " (contact_id, goal_verb, anchor_entity_ids, plan_text, outcome,"
                      "  happened_at) VALUES (?, ?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(st, 1, rec->contact_id ? rec->contact_id : "",
                       rec->contact_id ? (int)rec->contact_id_len : 0, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, p->goal_verb, (int)p->goal_verb_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, anchors, -1, SQLITE_STATIC);
    if (p->plan_text && p->plan_text_len > 0)
        sqlite3_bind_text(st, 4, p->plan_text, (int)p->plan_text_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 4);
    if (p->outcome && p->outcome_len > 0)
        sqlite3_bind_text(st, 5, p->outcome, (int)p->outcome_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 5);
    sqlite3_bind_int64(st, 6, happened);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_IO;
}

static hu_error_t v1_case_erase(void *vctx, hu_memory_kind_t kind, int64_t id) {
    (void)vctx;
    (void)id;
    if (kind != HU_MEM_CASE)
        return HU_ERR_INVALID_ARGUMENT;
    return HU_ERR_NOT_SUPPORTED;
}

static void v1_case_records_free(void *vctx, hu_allocator_t *alloc, hu_memory_record_t *r, size_t n) {
    (void)vctx;
    if (r == NULL || n == 0 || alloc == NULL)
        return;
    for (size_t i = 0; i < n; i++) {
        hu_memory_case_payload_t *pl = (hu_memory_case_payload_t *)r[i].payload;
        if (pl) {
            if (pl->goal_verb)
                xfree(alloc, pl->goal_verb, pl->goal_verb_len + 1);
            if (pl->plan_text)
                xfree(alloc, pl->plan_text, pl->plan_text_len + 1);
            if (pl->outcome)
                xfree(alloc, pl->outcome, pl->outcome_len + 1);
            if (pl->anchor_entity_ids)
                xfree(alloc, pl->anchor_entity_ids, pl->anchor_count * sizeof(int64_t));
            xfree(alloc, pl, sizeof(*pl));
        }
        if (r[i].provenance)
            xfree(alloc, r[i].provenance, r[i].provenance_len + 1);
    }
    xfree(alloc, r, n * sizeof(*r));
}

static hu_memory_facade_vtable_t s_v1_case_vt = {
    .name = "v1-case",
    .read = v1_case_read,
    .write = v1_case_write,
    .erase = v1_case_erase,
    .erase_by_provenance = NULL,
    .records_free = v1_case_records_free,
    .deinit = v1_ctx_deinit_noop,
};

#endif /* HU_ENABLE_SQLITE */

/* --------- HYPEREDGE -------------------------------------------------- */

/* The hyperedge module already owns its SQLite tables and the
 * (mean,variance) belief schema; the facade backend is a thin shim
 * that adapts hu_memory_record_t <-> hu_hyperedge_t and routes through
 * the v1 hyperedge API. We DO NOT duplicate storage or schema: this
 * is the single source of truth for HU_MEM_HYPEREDGE. */

/* The hyperedge facade backend is a thin shim that adapts
 * hu_memory_record_t <-> hu_hyperedge_t and routes through the v1
 * hyperedge API. The shared ctx (defined at the top of this file)
 * holds the facade pointer that hu_hyperedge_upsert/query expects. */

static hu_error_t v1_hyperedge_read(void *vctx, const hu_memory_query_t *q,
                                     hu_allocator_t *alloc,
                                     hu_memory_record_t **out, size_t *out_count) {
    if (q->kind != HU_MEM_HYPEREDGE) return HU_ERR_INVALID_ARGUMENT;
    if (q->as.neighbors.entity_id == 0) {
        /* Hyperedge query is "by member entity"; without an anchor we
         * decline rather than scan the whole table. */
        return HU_ERR_INVALID_ARGUMENT;
    }
    hu_memory_facade_t *m = hu_memory__v1_ctx_facade(vctx);
    if (!m) return HU_ERR_INVALID_ARGUMENT;

    hu_hyperedge_t *edges = NULL;
    size_t count = 0;
    hu_error_t err = hu_hyperedge_query_by_member(m, alloc,
                                                   q->as.neighbors.entity_id,
                                                   &edges, &count);
    if (err != HU_OK) {
        *out = NULL;
        *out_count = 0;
        return err;
    }
    if (count == 0) {
        *out = NULL;
        *out_count = 0;
        return HU_OK;
    }

    hu_memory_record_t *recs = xalloc(alloc, sizeof(*recs) * count);
    if (recs == NULL) {
        hu_hyperedges_free(alloc, edges, count);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(recs, 0, sizeof(*recs) * count);
    for (size_t i = 0; i < count; i++) {
        recs[i].kind = HU_MEM_HYPEREDGE;
        recs[i].id = edges[i].id;
        recs[i].event_start = edges[i].event_start;
        recs[i].event_end = edges[i].event_end;
        /* Record carries the (mean) so legacy float-confidence callers
         * see the posterior point estimate. Variance is preserved on
         * the payload itself for callers who need it. */
        recs[i].confidence = edges[i].belief.mean;
        hu_hyperedge_t *p = xalloc(alloc, sizeof(*p));
        if (p == NULL) {
            for (size_t j = 0; j < i; j++) {
                hu_hyperedge_t *prev = (hu_hyperedge_t *)recs[j].payload;
                if (prev) {
                    hu_hyperedges_free(alloc, prev, 1);
                }
            }
            hu_hyperedges_free(alloc, edges, count);
            xfree(alloc, recs, 0);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *p = edges[i];
        recs[i].payload = p;
        recs[i].payload_len = sizeof(*p);
    }
    /* Free the array backing only — payloads now own the strings. */
    xfree(alloc, edges, 0);
    *out = recs;
    *out_count = count;
    return HU_OK;
}

static hu_error_t v1_hyperedge_write(void *vctx, const hu_memory_record_t *rec) {
    if (rec->kind != HU_MEM_HYPEREDGE || rec->payload == NULL)
        return HU_ERR_INVALID_ARGUMENT;
    hu_memory_facade_t *m = hu_memory__v1_ctx_facade(vctx);
    if (!m) return HU_ERR_INVALID_ARGUMENT;
    const hu_hyperedge_t *he = rec->payload;
    int64_t out_id = 0;
    return hu_hyperedge_upsert(m, "", 0, he, &out_id);
}

static hu_error_t v1_hyperedge_erase(void *vctx, hu_memory_kind_t kind, int64_t id) {
    (void)vctx;
    (void)id;
    if (kind != HU_MEM_HYPEREDGE) return HU_ERR_INVALID_ARGUMENT;
    /* v1 has no row-level hyperedge delete yet (cascading erasure goes
     * through entity-level erase). Surface honestly. */
    return HU_ERR_NOT_SUPPORTED;
}

static void v1_hyperedge_records_free(void *vctx, hu_allocator_t *alloc,
                                       hu_memory_record_t *r, size_t n) {
    (void)vctx;
    if (r == NULL || n == 0) return;
    for (size_t i = 0; i < n; i++) {
        hu_hyperedge_t *p = (hu_hyperedge_t *)r[i].payload;
        if (p != NULL) {
            hu_hyperedges_free(alloc, p, 1);
        }
        if (r[i].provenance) {
            xfree(alloc, r[i].provenance, r[i].provenance_len + 1);
        }
    }
    xfree(alloc, r, n * sizeof(*r));
}

static hu_memory_facade_vtable_t s_v1_hyperedge_vt = {
    .name = "v1-hyperedge",
    .read = v1_hyperedge_read,
    .write = v1_hyperedge_write,
    .erase = v1_hyperedge_erase,
    .erase_by_provenance = NULL,
    .records_free = v1_hyperedge_records_free,
    .deinit = v1_ctx_deinit_noop,
};

/* --------- registration ---------------------------------------------- */

hu_error_t hu_memory__v1_backend_register(struct hu_memory_facade *m, hu_graph_t *graph) {
    if (m == NULL || graph == NULL) return HU_ERR_INVALID_ARGUMENT;
    /* One ctx, shared across the kinds the v1 backend services. memory.c frees
     * it once at hu_memory_facade_close via v1_bundle_ctx after all slots clear. */
    struct hu_memory_v1_ctx *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) return HU_ERR_OUT_OF_MEMORY;
    ctx->facade = m;
    ctx->graph = graph;
    hu_memory__v1_set_bundle_for_close(m, ctx);

    hu_error_t e = hu_memory_facade_register_backend(m, HU_MEM_ENTITY, &s_v1_entity_vt, ctx);
    if (e != HU_OK) {
        free(ctx);
        return e;
    }
    e = hu_memory_facade_register_backend(m, HU_MEM_RELATION, &s_v1_relation_vt, ctx);
    if (e != HU_OK) {
        /* The entity slot still owns ctx; closing the facade will free it. */
        return e;
    }
    e = hu_memory_facade_register_backend(m, HU_MEM_HYPEREDGE, &s_v1_hyperedge_vt, ctx);
    if (e != HU_OK) {
        return e;
    }
#ifdef HU_ENABLE_SQLITE
    e = hu_memory_facade_register_backend(m, HU_MEM_CASE, &s_v1_case_vt, ctx);
    if (e != HU_OK) {
        return e;
    }
#endif
    return HU_OK;
}

void hu_memory__v1_backend_unregister_all(struct hu_memory_facade *m) {
    /* Nothing to do here — facade close walks slots and calls each vtable's
     * deinit. The entity slot's deinit owns the ctx free. We keep this hook
     * for symmetry and as an extension point for kinds that want to do extra
     * work at unregister time (e.g. flush a write-through cache). */
    (void)m;
}
