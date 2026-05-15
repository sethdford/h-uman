/* Contact-scoped memory: wrapper around memory vtable for per-contact store/recall. */

#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/retrieval.h"
#include "human/memory/vector.h"
#include <stddef.h>
#include <string.h>

#define HU_CONTACT_PREFIX     "contact:"
#define HU_CONTACT_PREFIX_LEN 8

hu_error_t hu_memory_store_for_contact(hu_memory_t *mem, const char *contact_id,
                                       size_t contact_id_len, const char *key, size_t key_len,
                                       const char *content, size_t content_len,
                                       const hu_memory_category_t *category, const char *session_id,
                                       size_t session_id_len) {
    if (!mem || !mem->vtable || !mem->vtable->store || !contact_id || !key)
        return HU_ERR_INVALID_ARGUMENT;

    size_t prefixed_len = HU_CONTACT_PREFIX_LEN + contact_id_len + 1 + key_len;
    if (prefixed_len > 4096)
        return HU_ERR_INVALID_ARGUMENT;

    char prefixed_key[4096];
    size_t pos = 0;
    memcpy(prefixed_key, HU_CONTACT_PREFIX, HU_CONTACT_PREFIX_LEN);
    pos += HU_CONTACT_PREFIX_LEN;
    memcpy(prefixed_key + pos, contact_id, contact_id_len);
    pos += contact_id_len;
    prefixed_key[pos++] = ':';
    memcpy(prefixed_key + pos, key, key_len);
    pos += key_len;
    prefixed_key[pos] = '\0';

    return mem->vtable->store(mem->ctx, prefixed_key, pos, content, content_len, category,
                              session_id ? session_id : "", session_id_len);
}

hu_error_t hu_memory_recall_for_contact(hu_memory_t *mem, hu_allocator_t *alloc,
                                        hu_embedder_t *embedder, hu_vector_store_t *vector_store,
                                        const char *contact_id, size_t contact_id_len,
                                        const char *query, size_t query_len, size_t limit,
                                        const char *session_id, size_t session_id_len,
                                        hu_memory_entry_t **out, size_t *out_count) {
    if (!mem || !mem->vtable || !alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    if (!contact_id || contact_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    /* Use only the user's query for FTS/semantic, then filter results by contact key prefix.
     * Including the contact prefix in the FTS query causes false matches (FTS tokenizes
     * on colons and treats words as implicit OR). */
    const char *search_query = (query && query_len > 0) ? query : contact_id;
    size_t search_len = (query && query_len > 0) ? query_len : contact_id_len;

    hu_memory_entry_t *raw = NULL;
    size_t raw_count = 0;
    /* Pre-filter pool: ask the retrieval layer for `limit * N` so the contact-prefix
     * filter has room to discard non-matching namespaces. */
    size_t fetch_limit = limit > 0 ? limit * 4 : 20;
    hu_error_t err = HU_OK;

    /* Hybrid path: route through `hu_hybrid_retrieve` only when BOTH the embedder
     * and the vector store are provided. Partial-vector mode (one NULL) is rejected
     * by design — it would produce asymmetric RRF scores (US-3.1 design note). */
    if (embedder && embedder->vtable && vector_store && vector_store->vtable) {
        hu_retrieval_options_t opts = {
            .mode = HU_RETRIEVAL_HYBRID,
            .limit = fetch_limit,
            .min_score = 0.0,
            .use_reranking = true,
            .temporal_decay_factor = 0.0,
        };
        hu_retrieval_result_t res = {0};
        err = hu_hybrid_retrieve(alloc, mem, embedder, vector_store, /*graph=*/NULL, search_query,
                                 search_len, &opts, &res);
        if (err != HU_OK)
            return err;
        raw = res.entries;
        raw_count = res.count;
        /* hu_hybrid_retrieve allocates `scores` separately from `entries`; free the
         * scores array here so we can hand `entries` ownership to the filter below. */
        if (res.scores)
            alloc->free(alloc->ctx, res.scores, res.count * sizeof(double));
    } else {
        if (!mem->vtable->recall)
            return HU_OK;
        err = mem->vtable->recall(mem->ctx, alloc, search_query, search_len, fetch_limit,
                                  session_id ? session_id : "", session_id_len, &raw, &raw_count);
        if (err != HU_OK)
            return err;
    }
    if (!raw || raw_count == 0)
        return HU_OK;

    /* Filter to only entries with key prefix contact:<contact_id>: */
    char key_prefix[256];
    size_t kp_len = HU_CONTACT_PREFIX_LEN + contact_id_len + 1;
    if (kp_len >= sizeof(key_prefix))
        kp_len = sizeof(key_prefix) - 1;
    memcpy(key_prefix, HU_CONTACT_PREFIX, HU_CONTACT_PREFIX_LEN);
    memcpy(key_prefix + HU_CONTACT_PREFIX_LEN, contact_id, kp_len - HU_CONTACT_PREFIX_LEN - 1);
    key_prefix[kp_len - 1] = ':';
    key_prefix[kp_len] = '\0';

    size_t filtered = 0;
    for (size_t i = 0; i < raw_count && filtered < limit; i++) {
        if (raw[i].key && raw[i].key_len >= kp_len && memcmp(raw[i].key, key_prefix, kp_len) == 0)
            filtered++;
    }

    if (filtered == 0) {
        for (size_t i = 0; i < raw_count; i++)
            hu_memory_entry_free_fields(alloc, &raw[i]);
        alloc->free(alloc->ctx, raw, raw_count * sizeof(hu_memory_entry_t));
        return HU_OK;
    }

    hu_memory_entry_t *result =
        (hu_memory_entry_t *)alloc->alloc(alloc->ctx, filtered * sizeof(hu_memory_entry_t));
    if (!result) {
        for (size_t i = 0; i < raw_count; i++)
            hu_memory_entry_free_fields(alloc, &raw[i]);
        alloc->free(alloc->ctx, raw, raw_count * sizeof(hu_memory_entry_t));
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t dst = 0;
    for (size_t i = 0; i < raw_count && dst < filtered; i++) {
        if (raw[i].key && raw[i].key_len >= kp_len && memcmp(raw[i].key, key_prefix, kp_len) == 0) {
            result[dst++] = raw[i];
            memset(&raw[i], 0, sizeof(raw[i]));
        } else {
            hu_memory_entry_free_fields(alloc, &raw[i]);
        }
    }
    alloc->free(alloc->ctx, raw, raw_count * sizeof(hu_memory_entry_t));

    *out = result;
    *out_count = filtered;
    return HU_OK;
}
