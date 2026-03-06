#include "seaclaw/agent/memory_loader.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory/retrieval/adaptive.h"
#include <string.h>

static sc_retrieval_mode_t adaptive_to_retrieval_mode(sc_adaptive_strategy_t strategy) {
    switch (strategy) {
    case SC_ADAPTIVE_KEYWORD_ONLY:
        return SC_RETRIEVAL_KEYWORD;
    case SC_ADAPTIVE_VECTOR_ONLY:
        return SC_RETRIEVAL_SEMANTIC;
    case SC_ADAPTIVE_HYBRID:
    default:
        return SC_RETRIEVAL_HYBRID;
    }
}

static void free_recall_entries(sc_allocator_t *alloc, sc_memory_entry_t *entries, size_t count) {
    if (!alloc || !entries)
        return;
    for (size_t i = 0; i < count; i++)
        sc_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
}

sc_error_t sc_memory_loader_init(sc_memory_loader_t *loader, sc_allocator_t *alloc,
                                 sc_memory_t *memory, sc_retrieval_engine_t *retrieval_engine,
                                 size_t max_entries, size_t max_context_chars) {
    if (!loader || !alloc)
        return SC_ERR_INVALID_ARGUMENT;
    loader->alloc = alloc;
    loader->memory = memory;
    loader->retrieval_engine = retrieval_engine;
    loader->max_entries = max_entries ? max_entries : 10;
    loader->max_context_chars = max_context_chars ? max_context_chars : 4000;
    return SC_OK;
}

sc_error_t sc_memory_loader_load(sc_memory_loader_t *loader, const char *query, size_t query_len,
                                 const char *session_id, size_t session_id_len, char **out_context,
                                 size_t *out_context_len) {
    if (!loader || !out_context)
        return SC_ERR_INVALID_ARGUMENT;
    *out_context = NULL;
    if (out_context_len)
        *out_context_len = 0;

    sc_memory_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err;

    if (loader->retrieval_engine && loader->retrieval_engine->ctx &&
        loader->retrieval_engine->vtable) {
        sc_adaptive_config_t acfg = {.enabled = true, .keyword_max_tokens = 3, .vector_min_tokens = 5};
        sc_query_analysis_t qa = sc_adaptive_analyze_query(query ? query : "", query_len, &acfg);

        sc_retrieval_options_t opts = {
            .mode = adaptive_to_retrieval_mode(qa.recommended_strategy),
            .limit = loader->max_entries,
            .min_score = 0.0,
            .use_reranking = false,
            .temporal_decay_factor = 0.0,
        };
        sc_retrieval_result_t res = {0};
        err =
            loader->retrieval_engine->vtable->retrieve(loader->retrieval_engine->ctx, loader->alloc,
                                                       query ? query : "", query_len, &opts, &res);
        if (err == SC_OK && res.count > 0) {
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
        if (err != SC_OK)
            return err;
    } else {
        return SC_OK;
    }
    if (!entries || count == 0)
        return SC_OK;

    sc_json_buf_t buf;
    err = sc_json_buf_init(&buf, loader->alloc);
    if (err != SC_OK) {
        free_recall_entries(loader->alloc, entries, count);
        return err;
    }

    size_t total_len = 0;
    for (size_t i = 0; i < count && total_len < loader->max_context_chars; i++) {
        const sc_memory_entry_t *e = &entries[i];
        const char *key = e->key ? e->key : "unknown";
        size_t key_len = e->key_len ? e->key_len : strlen(key);
        const char *content = e->content ? e->content : "";
        size_t content_len = e->content_len;
        const char *timestamp = e->timestamp ? e->timestamp : "";
        size_t timestamp_len = e->timestamp_len ? e->timestamp_len : strlen(timestamp);

        /* Format: ### Memory: {key}\n{content}\n(stored: {timestamp})\n\n */
        size_t block_len = 12 + key_len + 1 + content_len + 12 + timestamp_len + 4;
        if (total_len + block_len > loader->max_context_chars) {
            size_t remain = loader->max_context_chars - total_len;
            if (remain < 30)
                break; /* too small for meaningful content */
            content_len = remain - 30;
        }

        err = sc_json_buf_append_raw(&buf, "### Memory: ", 12);
        if (err != SC_OK)
            goto cleanup;
        err = sc_json_buf_append_raw(&buf, key, key_len);
        if (err != SC_OK)
            goto cleanup;
        err = sc_json_buf_append_raw(&buf, "\n", 1);
        if (err != SC_OK)
            goto cleanup;

        if (content_len > 0) {
            err = sc_json_buf_append_raw(&buf, content, content_len);
            if (err != SC_OK)
                goto cleanup;
        }
        err = sc_json_buf_append_raw(&buf, "\n(stored: ", 10);
        if (err != SC_OK)
            goto cleanup;
        err = sc_json_buf_append_raw(&buf, timestamp, timestamp_len);
        if (err != SC_OK)
            goto cleanup;
        err = sc_json_buf_append_raw(&buf, ")\n\n", 3);
        if (err != SC_OK)
            goto cleanup;

        total_len += block_len;
    }

    if (buf.len > 0) {
        *out_context = sc_strndup(loader->alloc, buf.ptr, buf.len);
        if (!*out_context) {
            err = SC_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        if (out_context_len)
            *out_context_len = buf.len;
    }

cleanup:
    sc_json_buf_free(&buf);
    free_recall_entries(loader->alloc, entries, count);
    return err;
}
