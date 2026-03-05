#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory.h"
#include "seaclaw/memory/rerank.h"
#include "seaclaw/memory/retrieval.h"
#include "seaclaw/memory/vector.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SC_RRF_K 60.0f

/* Convert retrieval result to search results for RRF. Caller frees with sc_rerank_free_results. */
static sc_error_t entries_to_search_results(sc_allocator_t *alloc,
                                            const sc_memory_entry_t *entries, const double *scores,
                                            size_t count, sc_search_result_t *out) {
    for (size_t i = 0; i < count; i++) {
        const char *content = entries[i].content && entries[i].content_len > 0
                                  ? entries[i].content
                                  : (entries[i].key && entries[i].key_len > 0 ? entries[i].key
                                                                              : NULL);
        size_t len = content ? (entries[i].content && entries[i].content_len > 0
                                   ? entries[i].content_len
                                   : entries[i].key_len)
                            : 0;
        out[i].content = content && len > 0 ? sc_strndup(alloc, content, len) : NULL;
        out[i].score = (float)(scores && i < count ? scores[i] : 0.0);
        out[i].rerank_score = 0.0f;
        out[i].original_rank = i;
    }
    return SC_OK;
}

/* Convert search results back to retrieval result. Allocates entries/scores. */
static sc_error_t search_results_to_entries(sc_allocator_t *alloc, const char *query,
                                            sc_search_result_t *results, size_t count,
                                            size_t limit, sc_retrieval_result_t *out) {
    out->entries = NULL;
    out->count = 0;
    out->scores = NULL;
    if (count == 0)
        return SC_OK;
    size_t n = count > limit ? limit : count;
    sc_memory_entry_t *entries =
        (sc_memory_entry_t *)alloc->alloc(alloc->ctx, n * sizeof(sc_memory_entry_t));
    double *scores = (double *)alloc->alloc(alloc->ctx, n * sizeof(double));
    if (!entries || !scores) {
        if (entries)
            alloc->free(alloc->ctx, entries, n * sizeof(sc_memory_entry_t));
        if (scores)
            alloc->free(alloc->ctx, scores, n * sizeof(double));
        return SC_ERR_OUT_OF_MEMORY;
    }
    memset(entries, 0, n * sizeof(sc_memory_entry_t));
    for (size_t i = 0; i < n; i++) {
        if (results[i].content) {
            size_t len = strlen(results[i].content);
            entries[i].content = sc_strndup(alloc, results[i].content, len);
            entries[i].content_len = len;
            entries[i].key = sc_strndup(alloc, results[i].content, len);
            entries[i].key_len = len;
            entries[i].id = entries[i].key;
            entries[i].id_len = len;
        }
        entries[i].score = (double)results[i].rerank_score;
        scores[i] = (double)results[i].rerank_score;
    }
    (void)query;
    out->entries = entries;
    out->count = n;
    out->scores = scores;
    return SC_OK;
}

sc_error_t sc_hybrid_retrieve(sc_allocator_t *alloc, sc_memory_t *backend, sc_embedder_t *embedder,
                              sc_vector_store_t *vector_store, const char *query, size_t query_len,
                              const sc_retrieval_options_t *opts, sc_retrieval_result_t *out) {
    out->entries = NULL;
    out->count = 0;
    out->scores = NULL;

    if (!alloc || !query || query_len == 0)
        return SC_OK;

    size_t limit = opts && opts->limit > 0 ? opts->limit : 10;

    sc_retrieval_result_t keyword_result = {0};
    sc_error_t err = sc_keyword_retrieve(alloc, backend, query, query_len, opts, &keyword_result);
    if (err != SC_OK)
        return err;

    bool has_vector =
        embedder && embedder->vtable && vector_store && vector_store->vtable;

    if (!has_vector) {
        *out = keyword_result;
        return SC_OK;
    }

    sc_retrieval_result_t semantic_result = {0};
    err = sc_semantic_retrieve(alloc, embedder, vector_store, query, query_len, opts,
                               &semantic_result);
    if (err != SC_OK) {
        sc_retrieval_result_free(alloc, &keyword_result);
        return err;
    }

    /* Both keyword and semantic: merge with RRF, rerank with cross-encoder */
    size_t kw_count = keyword_result.count;
    size_t sem_count = semantic_result.count;

    if (kw_count == 0 && sem_count == 0) {
        return SC_OK;
    }

    size_t max_merged = (kw_count + sem_count) > limit ? (kw_count + sem_count) : limit;
    if (max_merged > 128)
        max_merged = 128;

    sc_search_result_t *kw_sr =
        (sc_search_result_t *)alloc->alloc(alloc->ctx, kw_count * sizeof(sc_search_result_t));
    sc_search_result_t *sem_sr =
        (sc_search_result_t *)alloc->alloc(alloc->ctx, sem_count * sizeof(sc_search_result_t));
    sc_search_result_t *merged =
        (sc_search_result_t *)alloc->alloc(alloc->ctx, max_merged * sizeof(sc_search_result_t));

    if (!kw_sr || !sem_sr || !merged) {
        if (kw_sr)
            alloc->free(alloc->ctx, kw_sr, kw_count * sizeof(sc_search_result_t));
        if (sem_sr)
            alloc->free(alloc->ctx, sem_sr, sem_count * sizeof(sc_search_result_t));
        if (merged)
            alloc->free(alloc->ctx, merged, max_merged * sizeof(sc_search_result_t));
        sc_retrieval_result_free(alloc, &keyword_result);
        sc_retrieval_result_free(alloc, &semantic_result);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memset(kw_sr, 0, kw_count * sizeof(sc_search_result_t));
    memset(sem_sr, 0, sem_count * sizeof(sc_search_result_t));
    memset(merged, 0, max_merged * sizeof(sc_search_result_t));

    sc_allocator_t sys = sc_system_allocator();
    if (kw_count > 0)
        entries_to_search_results(&sys, keyword_result.entries, keyword_result.scores, kw_count,
                                  kw_sr);
    if (sem_count > 0)
        entries_to_search_results(&sys, semantic_result.entries, semantic_result.scores, sem_count,
                                  sem_sr);

    sc_retrieval_result_free(alloc, &keyword_result);
    sc_retrieval_result_free(alloc, &semantic_result);

    size_t merged_count = 0;
    err = sc_rerank_rrf(kw_sr, kw_count, sem_sr, sem_count, merged, max_merged, &merged_count,
                        (float)SC_RRF_K);
    sc_rerank_free_results(kw_sr, kw_count);
    sc_rerank_free_results(sem_sr, sem_count);
    alloc->free(alloc->ctx, kw_sr, kw_count * sizeof(sc_search_result_t));
    alloc->free(alloc->ctx, sem_sr, sem_count * sizeof(sc_search_result_t));

    if (err != SC_OK) {
        sc_rerank_free_results(merged, merged_count);
        alloc->free(alloc->ctx, merged, max_merged * sizeof(sc_search_result_t));
        return err;
    }

    err = sc_rerank_cross_encoder(query, merged, merged_count);
    if (err != SC_OK) {
        sc_rerank_free_results(merged, merged_count);
        alloc->free(alloc->ctx, merged, max_merged * sizeof(sc_search_result_t));
        return err;
    }

    err = search_results_to_entries(alloc, query, merged, merged_count, limit, out);
    sc_rerank_free_results(merged, merged_count);
    alloc->free(alloc->ctx, merged, max_merged * sizeof(sc_search_result_t));
    return err;
}
