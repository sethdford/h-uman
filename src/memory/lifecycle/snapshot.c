#include "seaclaw/memory/lifecycle.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_SNAPSHOT_BUF_INIT 4096

static bool path_has_traversal(const char *path, size_t path_len) {
    for (size_t i = 0; i + 1 < path_len; i++)
        if (path[i] == '.' && path[i + 1] == '.') return true;
    return false;
}

sc_error_t sc_memory_snapshot_export(sc_allocator_t *alloc, sc_memory_t *memory,
    const char *path, size_t path_len) {
    if (!alloc || !memory || !memory->vtable || !path || path_len == 0)
        return SC_ERR_INVALID_ARGUMENT;
    /* Reject path traversal */
    if (path_has_traversal(path, path_len)) return SC_ERR_INVALID_ARGUMENT;

    sc_memory_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err = memory->vtable->list(memory->ctx, alloc, NULL, NULL, 0, &entries, &count);
    if (err != SC_OK) return err;

    sc_json_value_t *arr = sc_json_array_new(alloc);
    if (!arr) {
        if (entries) {
            for (size_t i = 0; i < count; i++) sc_memory_entry_free_fields(alloc, &entries[i]);
            alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
        }
        return SC_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < count; i++) {
        sc_memory_entry_t *e = &entries[i];
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) {
            sc_json_free(alloc, arr);
            for (size_t j = 0; j < count; j++) sc_memory_entry_free_fields(alloc, &entries[j]);
            alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
            return SC_ERR_OUT_OF_MEMORY;
        }
        if (e->key) {
            sc_json_value_t *v = sc_json_string_new(alloc, e->key, e->key_len);
            if (v) sc_json_object_set(alloc, obj, "key", v);
        }
        if (e->content) {
            sc_json_value_t *v = sc_json_string_new(alloc, e->content, e->content_len);
            if (v) sc_json_object_set(alloc, obj, "content", v);
        }
        if (e->timestamp) {
            sc_json_value_t *v = sc_json_string_new(alloc, e->timestamp, e->timestamp_len);
            if (v) sc_json_object_set(alloc, obj, "timestamp", v);
        }
        if (e->category.data.custom.name) {
            sc_json_value_t *v = sc_json_string_new(alloc, e->category.data.custom.name,
                e->category.data.custom.name_len);
            if (v) sc_json_object_set(alloc, obj, "category", v);
        }
        if (e->session_id && e->session_id_len > 0) {
            sc_json_value_t *v = sc_json_string_new(alloc, e->session_id, e->session_id_len);
            if (v) sc_json_object_set(alloc, obj, "session_id", v);
        }
        err = sc_json_array_push(alloc, arr, obj);
        if (err != SC_OK) {
            sc_json_free(alloc, obj);
            sc_json_free(alloc, arr);
            for (size_t j = 0; j < count; j++) sc_memory_entry_free_fields(alloc, &entries[j]);
            alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
            return err;
        }
    }

    char *json = NULL;
    size_t json_len = 0;
    err = sc_json_stringify(alloc, arr, &json, &json_len);
    sc_json_free(alloc, arr);
    for (size_t i = 0; i < count; i++) sc_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
    if (err != SC_OK) return err;
    if (!json) return SC_ERR_OUT_OF_MEMORY;

    char *path0 = (char *)alloc->alloc(alloc->ctx, path_len + 1);
    if (!path0) {
        alloc->free(alloc->ctx, json, json_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memcpy(path0, path, path_len);
    path0[path_len] = '\0';

    FILE *f = fopen(path0, "w");
    alloc->free(alloc->ctx, path0, path_len + 1);
    if (!f) {
        alloc->free(alloc->ctx, json, json_len + 1);
        return SC_ERR_IO;
    }
    bool ok = (fwrite(json, 1, json_len, f) == json_len);
    fclose(f);
    alloc->free(alloc->ctx, json, json_len + 1);
    return ok ? SC_OK : SC_ERR_IO;
}

sc_error_t sc_memory_snapshot_import(sc_allocator_t *alloc, sc_memory_t *memory,
    const char *path, size_t path_len) {
    if (!alloc || !memory || !memory->vtable || !path || path_len == 0)
        return SC_ERR_INVALID_ARGUMENT;
    /* Reject path traversal */
    if (path_has_traversal(path, path_len)) return SC_ERR_INVALID_ARGUMENT;

    char *path0 = (char *)alloc->alloc(alloc->ctx, path_len + 1);
    if (!path0) return SC_ERR_OUT_OF_MEMORY;
    memcpy(path0, path, path_len);
    path0[path_len] = '\0';

    FILE *f = fopen(path0, "rb");
    alloc->free(alloc->ctx, path0, path_len + 1);
    if (!f) return SC_ERR_IO;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return SC_ERR_IO;
    }
    long sz = ftell(f);
    if (sz <= 0 || sz > 1024 * 1024 * 64) {
        fclose(f);
        return sz <= 0 ? SC_ERR_IO : SC_ERR_IO;
    }
    rewind(f);

    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (nr != (size_t)sz) {
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return SC_ERR_IO;
    }
    buf[nr] = '\0';

    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(alloc, buf, nr, &root);
    alloc->free(alloc->ctx, buf, (size_t)sz + 1);
    if (err != SC_OK) return err;
    if (!root || root->type != SC_JSON_ARRAY) {
        if (root) sc_json_free(alloc, root);
        return SC_ERR_JSON_PARSE;
    }

    for (size_t i = 0; i < root->data.array.len; i++) {
        sc_json_value_t *item = root->data.array.items[i];
        if (!item || item->type != SC_JSON_OBJECT) continue;

        const char *key = sc_json_get_string(item, "key");
        const char *content = sc_json_get_string(item, "content");
        const char *cat_str = sc_json_get_string(item, "category");
        const char *ts = sc_json_get_string(item, "timestamp");
        const char *sid = sc_json_get_string(item, "session_id");

        (void)ts;
        if (!key || !content) continue;

        size_t key_len = strlen(key);
        size_t content_len = strlen(content);

        sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
        if (cat_str && strlen(cat_str) > 0) {
            cat.tag = SC_MEMORY_CATEGORY_CUSTOM;
            cat.data.custom.name = cat_str;
            cat.data.custom.name_len = strlen(cat_str);
        }

        size_t sid_len = sid ? strlen(sid) : 0;
        err = memory->vtable->store(memory->ctx, key, key_len, content, content_len,
            &cat, sid, sid_len);
        if (err != SC_OK) {
            sc_json_free(alloc, root);
            return err;
        }
    }

    sc_json_free(alloc, root);
    return SC_OK;
}
