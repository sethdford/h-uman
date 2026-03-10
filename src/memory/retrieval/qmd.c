#include "human/memory/retrieval/qmd.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/process_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QMD_CMD         "qmd"
#define QMD_SEARCH_MODE "search"

#if defined(HU_GATEWAY_POSIX) && !HU_IS_TEST
static hu_error_t parse_qmd_json(hu_allocator_t *alloc, const char *json_buf, size_t json_len,
                                 hu_memory_entry_t **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;
    if (!json_buf || json_len == 0)
        return HU_OK;

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, json_buf, json_len, &root);
    if (err != HU_OK || !root)
        return err;

    if (root->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, root);
        return HU_OK;
    }

    size_t n = root->data.array.len;
    if (n == 0) {
        hu_json_free(alloc, root);
        return HU_OK;
    }

    hu_memory_entry_t *entries =
        (hu_memory_entry_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_memory_entry_t));
    if (!entries) {
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(entries, 0, n * sizeof(hu_memory_entry_t));

    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        hu_json_value_t *item = root->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT)
            continue;

        const char *path = hu_json_get_string(item, "path");
        const char *title = hu_json_get_string(item, "title");
        const char *content = hu_json_get_string(item, "content");
        const char *text = hu_json_get_string(item, "text");

        const char *key_str = (path && path[0]) ? path : (title && title[0]) ? title : "";
        const char *cont_str = (content && content[0]) ? content : (text && text[0]) ? text : "";
        if (!key_str[0] && !cont_str[0])
            continue;

        char id_buf[64];
        int id_n = snprintf(id_buf, sizeof(id_buf), "qmd:%zu", i);
        if (id_n <= 0 || (size_t)id_n >= sizeof(id_buf))
            continue;

        char *id = (char *)alloc->alloc(alloc->ctx, (size_t)id_n + 1);
        char *key = (char *)alloc->alloc(alloc->ctx, strlen(key_str) + 1);
        char *cont = (char *)alloc->alloc(alloc->ctx, strlen(cont_str) + 1);
        if (!id || !key || !cont) {
            if (id)
                alloc->free(alloc->ctx, id, (size_t)id_n + 1);
            if (key)
                alloc->free(alloc->ctx, key, strlen(key_str) + 1);
            if (cont)
                alloc->free(alloc->ctx, cont, strlen(cont_str) + 1);
            break;
        }
        memcpy(id, id_buf, (size_t)id_n + 1);
        memcpy(key, key_str, strlen(key_str) + 1);
        memcpy(cont, cont_str, strlen(cont_str) + 1);

        entries[count].id = id;
        entries[count].id_len = (size_t)id_n;
        entries[count].key = key;
        entries[count].key_len = strlen(key_str);
        entries[count].content = cont;
        entries[count].content_len = strlen(cont_str);
        entries[count].category.tag = HU_MEMORY_CATEGORY_CORE;
        entries[count].timestamp = NULL;
        entries[count].timestamp_len = 0;
        entries[count].session_id = NULL;
        entries[count].session_id_len = 0;
        entries[count].score = (double)((int)n - (int)i);
        count++;
    }

    hu_json_free(alloc, root);

    if (count == 0) {
        for (size_t j = 0; j < n; j++) {
            if (entries[j].id)
                alloc->free(alloc->ctx, (void *)entries[j].id, entries[j].id_len + 1);
            if (entries[j].key)
                alloc->free(alloc->ctx, (void *)entries[j].key, entries[j].key_len + 1);
            if (entries[j].content)
                alloc->free(alloc->ctx, (void *)entries[j].content, entries[j].content_len + 1);
        }
        alloc->free(alloc->ctx, entries, n * sizeof(hu_memory_entry_t));
        return HU_OK;
    }

    *out = entries;
    *out_count = count;
    return HU_OK;
}
#endif

hu_error_t hu_qmd_keyword_candidates(hu_allocator_t *alloc, const char *workspace_dir,
                                     size_t workspace_len, const char *query, size_t query_len,
                                     unsigned limit, hu_memory_entry_t **out, size_t *out_count) {
    if (!alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

#ifdef HU_IS_TEST
    (void)workspace_dir;
    (void)workspace_len;
    (void)query;
    (void)query_len;
    (void)limit;
    return HU_OK;
#elif defined(HU_GATEWAY_POSIX)
    char limit_str[32];
    int ln = snprintf(limit_str, sizeof(limit_str), "%u", limit > 0 ? limit : 10);
    if (ln <= 0 || (size_t)ln >= sizeof(limit_str))
        return HU_ERR_INVALID_ARGUMENT;

    char *query_dup = (char *)alloc->alloc(alloc->ctx, query_len + 1);
    if (!query_dup)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(query_dup, query, query_len);
    query_dup[query_len] = '\0';

    char *ws_dup = NULL;
    if (workspace_dir && workspace_len > 0) {
        ws_dup = (char *)alloc->alloc(alloc->ctx, workspace_len + 1);
        if (!ws_dup) {
            alloc->free(alloc->ctx, query_dup, query_len + 1);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(ws_dup, workspace_dir, workspace_len);
        ws_dup[workspace_len] = '\0';
    }

    const char *argv[] = {QMD_CMD, QMD_SEARCH_MODE, query_dup, "--json", "-n", limit_str, NULL};

    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(alloc, argv, ws_dup ? ws_dup : NULL, 1024 * 1024, &result);

    if (ws_dup)
        alloc->free(alloc->ctx, ws_dup, workspace_len + 1);
    alloc->free(alloc->ctx, query_dup, query_len + 1);

    if (err != HU_OK)
        return err;
    if (!result.success || !result.stdout_buf || result.stdout_len == 0) {
        hu_run_result_free(alloc, &result);
        return HU_OK; /* No results */
    }

    err = parse_qmd_json(alloc, result.stdout_buf, result.stdout_len, out, out_count);
    hu_run_result_free(alloc, &result);
    return err;
#else
    (void)workspace_dir;
    (void)workspace_len;
    (void)query;
    (void)query_len;
    (void)limit;
    return HU_ERR_NOT_SUPPORTED; /* Process spawn requires POSIX */
#endif
}
