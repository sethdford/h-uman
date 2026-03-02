#include "seaclaw/agent/memory_loader.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <string.h>

static void free_recall_entries(sc_allocator_t *alloc,
    sc_memory_entry_t *entries, size_t count)
{
    if (!alloc || !entries) return;
    for (size_t i = 0; i < count; i++) {
        sc_memory_entry_t *e = &entries[i];
        if (e->id) alloc->free(alloc->ctx, (void *)e->id, e->id_len + 1);
        if (e->key) alloc->free(alloc->ctx, (void *)e->key, e->key_len + 1);
        if (e->content) alloc->free(alloc->ctx, (void *)e->content, e->content_len + 1);
        if (e->category.tag == SC_MEMORY_CATEGORY_CUSTOM &&
            e->category.data.custom.name) {
            alloc->free(alloc->ctx, (void *)e->category.data.custom.name,
                e->category.data.custom.name_len + 1);
        }
        if (e->timestamp) alloc->free(alloc->ctx, (void *)e->timestamp, e->timestamp_len + 1);
        if (e->session_id) alloc->free(alloc->ctx, (void *)e->session_id, e->session_id_len + 1);
    }
    alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
}

sc_error_t sc_memory_loader_init(sc_memory_loader_t *loader,
    sc_allocator_t *alloc, sc_memory_t *memory,
    size_t max_entries, size_t max_context_chars)
{
    if (!loader || !alloc) return SC_ERR_INVALID_ARGUMENT;
    loader->alloc = alloc;
    loader->memory = memory;
    loader->max_entries = max_entries ? max_entries : 10;
    loader->max_context_chars = max_context_chars ? max_context_chars : 4000;
    return SC_OK;
}

sc_error_t sc_memory_loader_load(sc_memory_loader_t *loader,
    const char *query, size_t query_len,
    const char *session_id, size_t session_id_len,
    char **out_context, size_t *out_context_len)
{
    if (!loader || !out_context) return SC_ERR_INVALID_ARGUMENT;
    *out_context = NULL;
    if (out_context_len) *out_context_len = 0;

    if (!loader->memory || !loader->memory->vtable ||
        !loader->memory->vtable->recall) {
        return SC_OK; /* no memory backend, not an error */
    }

    sc_memory_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err = loader->memory->vtable->recall(loader->memory->ctx,
        loader->alloc,
        query ? query : "",
        query_len,
        loader->max_entries,
        session_id ? session_id : "",
        session_id_len,
        &entries, &count);

    if (err != SC_OK) return err;
    if (!entries || count == 0) return SC_OK;

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
            if (remain < 30) break; /* too small for meaningful content */
            content_len = remain - 30;
        }

        err = sc_json_buf_append_raw(&buf, "### Memory: ", 12);
        if (err != SC_OK) goto cleanup;
        err = sc_json_buf_append_raw(&buf, key, key_len);
        if (err != SC_OK) goto cleanup;
        err = sc_json_buf_append_raw(&buf, "\n", 1);
        if (err != SC_OK) goto cleanup;

        if (content_len > 0) {
            err = sc_json_buf_append_raw(&buf, content, content_len);
            if (err != SC_OK) goto cleanup;
        }
        err = sc_json_buf_append_raw(&buf, "\n(stored: ", 10);
        if (err != SC_OK) goto cleanup;
        err = sc_json_buf_append_raw(&buf, timestamp, timestamp_len);
        if (err != SC_OK) goto cleanup;
        err = sc_json_buf_append_raw(&buf, ")\n\n", 3);
        if (err != SC_OK) goto cleanup;

        total_len += block_len;
    }

    if (buf.len > 0) {
        *out_context = sc_strndup(loader->alloc, buf.ptr, buf.len);
        if (!*out_context) {
            err = SC_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        if (out_context_len) *out_context_len = buf.len;
    }

cleanup:
    sc_json_buf_free(&buf);
    free_recall_entries(loader->alloc, entries, count);
    return err;
}
