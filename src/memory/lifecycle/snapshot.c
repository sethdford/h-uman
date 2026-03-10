#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/lifecycle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_SNAPSHOT_BUF_INIT 4096

static bool path_has_traversal(const char *path, size_t path_len) {
    for (size_t i = 0; i + 1 < path_len; i++)
        if (path[i] == '.' && path[i + 1] == '.')
            return true;
    return false;
}

hu_error_t hu_memory_snapshot_export(hu_allocator_t *alloc, hu_memory_t *memory, const char *path,
                                     size_t path_len) {
    if (!alloc || !memory || !memory->vtable || !path || path_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    /* Reject path traversal */
    if (path_has_traversal(path, path_len))
        return HU_ERR_INVALID_ARGUMENT;

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = memory->vtable->list(memory->ctx, alloc, NULL, NULL, 0, &entries, &count);
    if (err != HU_OK)
        return err;

    hu_json_value_t *arr = hu_json_array_new(alloc);
    if (!arr) {
        if (entries) {
            for (size_t i = 0; i < count; i++)
                hu_memory_entry_free_fields(alloc, &entries[i]);
            alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
        }
        return HU_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < count; i++) {
        hu_memory_entry_t *e = &entries[i];
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj) {
            hu_json_free(alloc, arr);
            for (size_t j = 0; j < count; j++)
                hu_memory_entry_free_fields(alloc, &entries[j]);
            alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
            return HU_ERR_OUT_OF_MEMORY;
        }
        if (e->key) {
            hu_json_value_t *v = hu_json_string_new(alloc, e->key, e->key_len);
            if (v)
                hu_json_object_set(alloc, obj, "key", v);
        }
        if (e->content) {
            hu_json_value_t *v = hu_json_string_new(alloc, e->content, e->content_len);
            if (v)
                hu_json_object_set(alloc, obj, "content", v);
        }
        if (e->timestamp) {
            hu_json_value_t *v = hu_json_string_new(alloc, e->timestamp, e->timestamp_len);
            if (v)
                hu_json_object_set(alloc, obj, "timestamp", v);
        }
        if (e->category.data.custom.name) {
            hu_json_value_t *v = hu_json_string_new(alloc, e->category.data.custom.name,
                                                    e->category.data.custom.name_len);
            if (v)
                hu_json_object_set(alloc, obj, "category", v);
        }
        if (e->session_id && e->session_id_len > 0) {
            hu_json_value_t *v = hu_json_string_new(alloc, e->session_id, e->session_id_len);
            if (v)
                hu_json_object_set(alloc, obj, "session_id", v);
        }
        err = hu_json_array_push(alloc, arr, obj);
        if (err != HU_OK) {
            hu_json_free(alloc, obj);
            hu_json_free(alloc, arr);
            for (size_t j = 0; j < count; j++)
                hu_memory_entry_free_fields(alloc, &entries[j]);
            alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
            return err;
        }
    }

    char *json = NULL;
    size_t json_len = 0;
    err = hu_json_stringify(alloc, arr, &json, &json_len);
    hu_json_free(alloc, arr);
    for (size_t i = 0; i < count; i++)
        hu_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
    if (err != HU_OK)
        return err;
    if (!json)
        return HU_ERR_OUT_OF_MEMORY;

    char *path0 = (char *)alloc->alloc(alloc->ctx, path_len + 1);
    if (!path0) {
        alloc->free(alloc->ctx, json, json_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(path0, path, path_len);
    path0[path_len] = '\0';

    FILE *f = fopen(path0, "w");
    alloc->free(alloc->ctx, path0, path_len + 1);
    if (!f) {
        alloc->free(alloc->ctx, json, json_len + 1);
        return HU_ERR_IO;
    }
    bool ok = (fwrite(json, 1, json_len, f) == json_len);
    fclose(f);
    alloc->free(alloc->ctx, json, json_len + 1);
    return ok ? HU_OK : HU_ERR_IO;
}

hu_error_t hu_memory_snapshot_import(hu_allocator_t *alloc, hu_memory_t *memory, const char *path,
                                     size_t path_len) {
    if (!alloc || !memory || !memory->vtable || !path || path_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    /* Reject path traversal */
    if (path_has_traversal(path, path_len))
        return HU_ERR_INVALID_ARGUMENT;

    char *path0 = (char *)alloc->alloc(alloc->ctx, path_len + 1);
    if (!path0)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(path0, path, path_len);
    path0[path_len] = '\0';

    FILE *f = fopen(path0, "rb");
    alloc->free(alloc->ctx, path0, path_len + 1);
    if (!f)
        return HU_ERR_IO;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    long sz = ftell(f);
    if (sz <= 0 || sz > 1024 * 1024 * 64) {
        fclose(f);
        return sz <= 0 ? HU_ERR_IO : HU_ERR_IO;
    }
    rewind(f);

    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (nr != (size_t)sz) {
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return HU_ERR_IO;
    }
    buf[nr] = '\0';

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, buf, nr, &root);
    alloc->free(alloc->ctx, buf, (size_t)sz + 1);
    if (err != HU_OK)
        return err;
    if (!root || root->type != HU_JSON_ARRAY) {
        if (root)
            hu_json_free(alloc, root);
        return HU_ERR_JSON_PARSE;
    }

    for (size_t i = 0; i < root->data.array.len; i++) {
        hu_json_value_t *item = root->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT)
            continue;

        const char *key = hu_json_get_string(item, "key");
        const char *content = hu_json_get_string(item, "content");
        const char *cat_str = hu_json_get_string(item, "category");
        const char *ts = hu_json_get_string(item, "timestamp");
        const char *sid = hu_json_get_string(item, "session_id");

        (void)ts;
        if (!key || !content)
            continue;

        size_t key_len = strlen(key);
        size_t content_len = strlen(content);

        hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
        if (cat_str && strlen(cat_str) > 0) {
            cat.tag = HU_MEMORY_CATEGORY_CUSTOM;
            cat.data.custom.name = cat_str;
            cat.data.custom.name_len = strlen(cat_str);
        }

        size_t sid_len = sid ? strlen(sid) : 0;
        err = memory->vtable->store(memory->ctx, key, key_len, content, content_len, &cat, sid,
                                    sid_len);
        if (err != HU_OK) {
            hu_json_free(alloc, root);
            return err;
        }
    }

    hu_json_free(alloc, root);
    return HU_OK;
}
