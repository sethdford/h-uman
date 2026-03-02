#include "seaclaw/memory/retrieval.h"
#include "seaclaw/memory.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct sc_retrieval_engine_ctx {
    sc_allocator_t *alloc;
    sc_memory_t backend;
} sc_retrieval_engine_ctx_t;

static sc_error_t impl_retrieve(void *ctx, sc_allocator_t *alloc,
    const char *query, size_t query_len,
    const sc_retrieval_options_t *opts,
    sc_retrieval_result_t *out) {
    sc_retrieval_engine_ctx_t *e = (sc_retrieval_engine_ctx_t *)ctx;
    out->entries = NULL;
    out->count = 0;
    out->scores = NULL;

    if (!query || !opts) return SC_ERR_INVALID_ARGUMENT;

    sc_retrieval_options_t o = *opts;
    if (o.limit == 0) o.limit = 10;

    sc_error_t err;
    switch (o.mode) {
        case SC_RETRIEVAL_KEYWORD:
            err = sc_keyword_retrieve(alloc, &e->backend, query, query_len,
                &o, out);
            break;
        case SC_RETRIEVAL_SEMANTIC:
            /* Semantic not implemented - fall back to keyword */
            o.mode = SC_RETRIEVAL_KEYWORD;
            err = sc_keyword_retrieve(alloc, &e->backend, query, query_len,
                &o, out);
            break;
        case SC_RETRIEVAL_HYBRID:
            err = sc_hybrid_retrieve(alloc, &e->backend, query, query_len,
                &o, out);
            break;
        default:
            err = sc_keyword_retrieve(alloc, &e->backend, query, query_len,
                &o, out);
            break;
    }
    if (err != SC_OK || out->count == 0) return err;

    /* Apply temporal decay */
    if (o.temporal_decay_factor > 0.0) {
        for (size_t i = 0; i < out->count; i++) {
            sc_memory_entry_t *ent = &out->entries[i];
            out->scores[i] = sc_temporal_decay_score(out->scores[i],
                o.temporal_decay_factor,
                ent->timestamp, ent->timestamp_len);
        }
        /* Re-sort by score after decay */
        for (size_t i = 0; i < out->count; i++) {
            for (size_t j = i + 1; j < out->count; j++) {
                if (out->scores[j] > out->scores[i]) {
                    sc_memory_entry_t te = out->entries[i];
                    double ts = out->scores[i];
                    out->entries[i] = out->entries[j];
                    out->scores[i] = out->scores[j];
                    out->entries[j] = te;
                    out->scores[j] = ts;
                }
            }
        }
    }

    /* Apply MMR reranking if requested */
    if (o.use_reranking && out->count > 1) {
        err = sc_mmr_rerank(alloc, query, query_len,
            out->entries, out->scores, out->count, 0.7);
        if (err != SC_OK) {
            sc_retrieval_result_free(alloc, out);
            return err;
        }
    }

    return SC_OK;
}

static void impl_deinit(void *ctx, sc_allocator_t *alloc) {
    sc_retrieval_engine_ctx_t *e = (sc_retrieval_engine_ctx_t *)ctx;
    /* Backend is owned by caller, we do not deinit it */
    (void)e;
    alloc->free(alloc->ctx, e, sizeof(sc_retrieval_engine_ctx_t));
}

static const sc_retrieval_vtable_t engine_vtable = {
    .retrieve = impl_retrieve,
    .deinit = impl_deinit,
};

sc_retrieval_engine_t sc_retrieval_create(sc_allocator_t *alloc,
    sc_memory_t *backend) {
    if (!alloc || !backend || !backend->ctx || !backend->vtable)
        return (sc_retrieval_engine_t){ .ctx = NULL, .vtable = NULL };

    sc_retrieval_engine_ctx_t *ctx = (sc_retrieval_engine_ctx_t *)alloc->alloc(
        alloc->ctx, sizeof(sc_retrieval_engine_ctx_t));
    if (!ctx) return (sc_retrieval_engine_t){ .ctx = NULL, .vtable = NULL };

    ctx->alloc = alloc;
    ctx->backend = *backend;
    return (sc_retrieval_engine_t){
        .ctx = ctx,
        .vtable = &engine_vtable,
    };
}

void sc_retrieval_result_free(sc_allocator_t *alloc, sc_retrieval_result_t *r) {
    if (!alloc || !r) return;
    for (size_t i = 0; i < r->count; i++)
        sc_memory_entry_free_fields(alloc, &r->entries[i]);
    if (r->entries)
        alloc->free(alloc->ctx, r->entries,
            r->count * sizeof(sc_memory_entry_t));
    if (r->scores)
        alloc->free(alloc->ctx, r->scores, r->count * sizeof(double));
    r->entries = NULL;
    r->count = 0;
    r->scores = NULL;
}
