#include "human/agent/memory_loader.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/memory/retrieval/adaptive.h"
#include <string.h>

static hu_retrieval_mode_t adaptive_to_retrieval_mode(hu_adaptive_strategy_t strategy) {
    switch (strategy) {
    case HU_ADAPTIVE_KEYWORD_ONLY:
        return HU_RETRIEVAL_KEYWORD;
    case HU_ADAPTIVE_VECTOR_ONLY:
        return HU_RETRIEVAL_SEMANTIC;
    case HU_ADAPTIVE_HYBRID:
    default:
        return HU_RETRIEVAL_HYBRID;
    }
}

static void free_recall_entries(hu_allocator_t *alloc, hu_memory_entry_t *entries, size_t count) {
    if (!alloc || !entries)
        return;
    for (size_t i = 0; i < count; i++)
        hu_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
}

hu_error_t hu_memory_loader_init(hu_memory_loader_t *loader, hu_allocator_t *alloc,
                                 hu_memory_t *memory, hu_retrieval_engine_t *retrieval_engine,
                                 size_t max_entries, size_t max_context_chars) {
    if (!loader || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    loader->alloc = alloc;
    loader->memory = memory;
    loader->retrieval_engine = retrieval_engine;
    loader->max_entries = max_entries ? max_entries : 10;
    loader->max_context_chars = max_context_chars ? max_context_chars : 4000;
    return HU_OK;
}

hu_error_t hu_memory_loader_load(hu_memory_loader_t *loader, const char *query, size_t query_len,
                                 const char *session_id, size_t session_id_len, char **out_context,
                                 size_t *out_context_len) {
    if (!loader || !out_context)
        return HU_ERR_INVALID_ARGUMENT;
    *out_context = NULL;
    if (out_context_len)
        *out_context_len = 0;

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err;

    if (loader->retrieval_engine && loader->retrieval_engine->ctx &&
        loader->retrieval_engine->vtable) {
        hu_adaptive_config_t acfg = {
            .enabled = true, .keyword_max_tokens = 3, .vector_min_tokens = 5};
        hu_query_analysis_t qa = hu_adaptive_analyze_query(query ? query : "", query_len, &acfg);

        hu_retrieval_options_t opts = {
            .mode = adaptive_to_retrieval_mode(qa.recommended_strategy),
            .limit = loader->max_entries,
            .min_score = 0.0,
            .use_reranking = false,
            .temporal_decay_factor = 0.0,
        };
        hu_retrieval_result_t res = {0};
        err =
            loader->retrieval_engine->vtable->retrieve(loader->retrieval_engine->ctx, loader->alloc,
                                                       query ? query : "", query_len, &opts, &res);
        if (err == HU_OK && res.count > 0) {
            entries = res.entries;
            count = res.count;
            if (res.scores)
                loader->alloc->free(loader->alloc->ctx, res.scores, count * sizeof(double));
            res.entries = NULL;
            res.count = 0;
            res.scores = NULL;
        }
    } else if (loader->memory && loader->memory->vtable && loader->memory->vtable->recall) {
        err = loader->memory->vtable->recall(
            loader->memory->ctx, loader->alloc, query ? query : "", query_len, loader->max_entries,
            session_id ? session_id : "", session_id_len, &entries, &count);
        if (err != HU_OK)
            return err;
    } else {
        return HU_OK;
    }
    if (!entries || count == 0)
        return HU_OK;

    hu_json_buf_t buf;
    err = hu_json_buf_init(&buf, loader->alloc);
    if (err != HU_OK) {
        free_recall_entries(loader->alloc, entries, count);
        return err;
    }

    size_t total_len = 0;
    for (size_t i = 0; i < count && total_len < loader->max_context_chars; i++) {
        const hu_memory_entry_t *e = &entries[i];
        const char *key = e->key ? e->key : "unknown";
        size_t key_len = e->key_len ? e->key_len : strlen(key);
        const char *content = e->content ? e->content : "";
        size_t content_len = e->content_len;
        const char *timestamp = e->timestamp ? e->timestamp : "";
        size_t timestamp_len = e->timestamp_len ? e->timestamp_len : strlen(timestamp);

        /* Format: ### Memory: {key}\n{content}\n(stored: {timestamp})\n\n */
        size_t overhead = 26 + key_len + timestamp_len;
        size_t block_len = overhead + content_len;
        if (total_len + block_len > loader->max_context_chars) {
            size_t remain = loader->max_context_chars - total_len;
            if (remain <= overhead)
                break;
            content_len = remain - overhead;
            block_len = remain;
        }

        err = hu_json_buf_append_raw(&buf, "### Memory: ", 12);
        if (err != HU_OK)
            goto cleanup;
        err = hu_json_buf_append_raw(&buf, key, key_len);
        if (err != HU_OK)
            goto cleanup;
        err = hu_json_buf_append_raw(&buf, "\n", 1);
        if (err != HU_OK)
            goto cleanup;

        if (content_len > 0) {
            err = hu_json_buf_append_raw(&buf, content, content_len);
            if (err != HU_OK)
                goto cleanup;
        }
        err = hu_json_buf_append_raw(&buf, "\n(stored: ", 10);
        if (err != HU_OK)
            goto cleanup;
        err = hu_json_buf_append_raw(&buf, timestamp, timestamp_len);
        if (err != HU_OK)
            goto cleanup;
        err = hu_json_buf_append_raw(&buf, ")\n\n", 3);
        if (err != HU_OK)
            goto cleanup;

        total_len += block_len;
    }

    if (buf.len > 0) {
        *out_context = hu_strndup(loader->alloc, buf.ptr, buf.len);
        if (!*out_context) {
            err = HU_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        if (out_context_len)
            *out_context_len = buf.len;
    }

cleanup:
    hu_json_buf_free(&buf);
    free_recall_entries(loader->alloc, entries, count);
    return err;
}
