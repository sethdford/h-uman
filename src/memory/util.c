#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

void hu_memory_entry_free_fields(hu_allocator_t *alloc, hu_memory_entry_t *e) {
    if (!alloc || !e)
        return;
    if (e->id)
        alloc->free(alloc->ctx, (void *)e->id, e->id_len + 1);
    if (e->key && e->key != e->id)
        alloc->free(alloc->ctx, (void *)e->key, e->key_len + 1);
    if (e->content)
        alloc->free(alloc->ctx, (void *)e->content, e->content_len + 1);
    if (e->category.data.custom.name)
        alloc->free(alloc->ctx, (void *)e->category.data.custom.name,
                    e->category.data.custom.name_len + 1);
    if (e->timestamp)
        alloc->free(alloc->ctx, (void *)e->timestamp, e->timestamp_len + 1);
    if (e->session_id)
        alloc->free(alloc->ctx, (void *)e->session_id, e->session_id_len + 1);
    if (e->source)
        alloc->free(alloc->ctx, (void *)e->source, e->source_len + 1);
    if (e->provenance)
        alloc->free(alloc->ctx, (void *)e->provenance, e->provenance_len + 1);
}

/* JSON-escape into a fixed buffer; truncates silently on overflow. */
static size_t export_escape_json(const char *in, size_t in_len,
                                 char *out, size_t out_cap) {
    if (out_cap == 0) return 0;
    size_t w = 0;
    for (size_t i = 0; i < in_len && w + 6 < out_cap; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
        case '"':  out[w++] = '\\'; out[w++] = '"';  break;
        case '\\': out[w++] = '\\'; out[w++] = '\\'; break;
        case '\n': out[w++] = '\\'; out[w++] = 'n';  break;
        case '\r': out[w++] = '\\'; out[w++] = 'r';  break;
        case '\t': out[w++] = '\\'; out[w++] = 't';  break;
        default:
            if (c < 0x20) {
                w += (size_t)snprintf(out + w, out_cap - w, "\\u%04x", c);
            } else {
                out[w++] = (char)c;
            }
            break;
        }
    }
    out[w] = '\0';
    return w;
}

hu_error_t hu_memory_export_json(hu_memory_t *mem, hu_allocator_t *alloc,
                                 const char *output_path) {
    if (!mem || !mem->vtable || !alloc || !output_path)
        return HU_ERR_INVALID_ARGUMENT;

    FILE *fp = fopen(output_path, "w");
    if (!fp)
        return HU_ERR_IO;

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = mem->vtable->list(mem->ctx, alloc, NULL, NULL, 0,
                                       &entries, &count);
    if (err != HU_OK) {
        fclose(fp);
        return err;
    }

    const char *name = mem->vtable->name
        ? mem->vtable->name(mem->ctx) : "unknown";

    fprintf(fp, "{\n  \"version\": \"hu-export-v1\",\n  \"backend\": \"%s\",\n"
                "  \"count\": %zu,\n  \"entries\": [\n", name, count);

    char ek[8192];
    char ec[65536];
    for (size_t i = 0; i < count; i++) {
        const hu_memory_entry_t *e = &entries[i];
        ek[0] = ec[0] = '\0';
        export_escape_json(e->key ? e->key : "", e->key_len, ek, sizeof(ek));
        export_escape_json(e->content ? e->content : "", e->content_len,
                           ec, sizeof(ec));
        fprintf(fp, "    {\"key\": \"%s\", \"content\": \"%s\"}%s\n",
                ek, ec, (i + 1 < count) ? "," : "");
        hu_memory_entry_free_fields(alloc, &entries[i]);
    }
    fprintf(fp, "  ]\n}\n");
    alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
    fclose(fp);
    return HU_OK;
}

hu_error_t hu_memory_store_with_source(hu_memory_t *mem, const char *key, size_t key_len,
                                       const char *content, size_t content_len,
                                       const hu_memory_category_t *category, const char *session_id,
                                       size_t session_id_len, const char *source,
                                       size_t source_len) {
    if (!mem || !mem->vtable)
        return HU_ERR_INVALID_ARGUMENT;

    if (mem->vtable->store_ex && source && source_len > 0) {
        hu_memory_store_opts_t opts = {
            .source = source, .source_len = source_len, .importance = -1.0};
        return mem->vtable->store_ex(mem->ctx, key, key_len, content, content_len, category,
                                     session_id, session_id_len, &opts);
    }
    return mem->vtable->store(mem->ctx, key, key_len, content, content_len, category, session_id,
                              session_id_len);
}
