typedef int hu_life_chapters_unused_;

#ifdef HU_ENABLE_SQLITE

#include "human/memory/life_chapters.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/life_chapter_repo.h"
#include "human/persona.h"
#include <stdio.h>
#include <string.h>

/* DDD Phase 3: SQL + the raw sqlite3 handle now live behind
 * hu_life_chapter_repo_t (src/memory/repos/life_chapter_repo_sqlite.c). This
 * file keeps the key_threads JSON (de)serialization and the directive builder,
 * and depends only on the backend-agnostic repo interface — no <sqlite3.h>. */

static char *key_threads_to_json(hu_allocator_t *alloc, const hu_life_chapter_t *chapter,
                                 size_t *out_len) {
    hu_json_value_t *arr = hu_json_array_new(alloc);
    if (!arr)
        return NULL;
    for (size_t i = 0; i < chapter->key_threads_count && i < 8; i++) {
        const char *t = chapter->key_threads[i];
        size_t tl = t ? strlen(t) : 0;
        if (tl == 0)
            continue;
        hu_json_value_t *s = hu_json_string_new(alloc, t, tl);
        if (!s) {
            hu_json_free(alloc, arr);
            return NULL;
        }
        hu_error_t err = hu_json_array_push(alloc, arr, s);
        if (err != HU_OK) {
            hu_json_free(alloc, s);
            hu_json_free(alloc, arr);
            return NULL;
        }
    }
    char *json = NULL;
    size_t json_len = 0;
    hu_error_t err = hu_json_stringify(alloc, arr, &json, &json_len);
    hu_json_free(alloc, arr);
    if (err != HU_OK || !json) {
        return NULL;
    }
    *out_len = json_len;
    return json;
}

static hu_error_t parse_key_threads(hu_allocator_t *alloc, const char *json, size_t json_len,
                                    hu_life_chapter_t *out) {
    if (!json || json_len == 0) {
        out->key_threads_count = 0;
        return HU_OK;
    }
    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, json, json_len, &root);
    if (err != HU_OK || !root || root->type != HU_JSON_ARRAY) {
        if (root)
            hu_json_free(alloc, root);
        out->key_threads_count = 0;
        return HU_OK;
    }
    size_t n = root->data.array.len;
    if (n > 8)
        n = 8;
    size_t filled = 0;
    for (size_t i = 0; i < n && root->data.array.items[i] && filled < 8; i++) {
        hu_json_value_t *item = root->data.array.items[i];
        if (item->type == HU_JSON_STRING && item->data.string.ptr) {
            size_t len = item->data.string.len;
            if (len > 127)
                len = 127;
            snprintf(out->key_threads[filled], sizeof(out->key_threads[filled]), "%.*s", (int)len,
                     item->data.string.ptr);
            filled++;
        }
    }
    out->key_threads_count = filled;
    hu_json_free(alloc, root);
    return HU_OK;
}

hu_error_t hu_life_chapter_get_active(hu_allocator_t *alloc, hu_memory_t *memory,
                                      hu_life_chapter_t *out) {
    if (!alloc || !memory || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    hu_life_chapter_repo_t repo;
    hu_error_t e = hu_life_chapter_repo_create(memory, alloc, &repo);
    if (e != HU_OK)
        return e; /* HU_ERR_NOT_SUPPORTED for non-sqlite, as before */

    bool found = false;
    hu_life_chapter_row_t row = {0};
    e = repo.vtable->get_active(repo.ctx, alloc, &found, &row);
    if (e == HU_OK && found) {
        if (row.theme)
            snprintf(out->theme, sizeof(out->theme), "%s", row.theme);
        if (row.mood)
            snprintf(out->mood, sizeof(out->mood), "%s", row.mood);
        out->started_at = row.started_at;
        if (row.key_threads_json)
            parse_key_threads(alloc, row.key_threads_json, strlen(row.key_threads_json), out);
    }
    if (row.theme)
        hu_str_free(alloc, row.theme);
    if (row.mood)
        hu_str_free(alloc, row.mood);
    if (row.key_threads_json)
        hu_str_free(alloc, row.key_threads_json);

    repo.vtable->deinit(repo.ctx);
    return e;
}

hu_error_t hu_life_chapter_store(hu_allocator_t *alloc, hu_memory_t *memory,
                                 const hu_life_chapter_t *chapter) {
    if (!alloc || !memory || !chapter)
        return HU_ERR_INVALID_ARGUMENT;

    hu_life_chapter_repo_t repo;
    hu_error_t e = hu_life_chapter_repo_create(memory, alloc, &repo);
    if (e != HU_OK)
        return e;

    size_t kt_len = 0;
    char *key_threads_json = key_threads_to_json(alloc, chapter, &kt_len);
    if (!key_threads_json && chapter->key_threads_count > 0) {
        repo.vtable->deinit(repo.ctx);
        return HU_ERR_OUT_OF_MEMORY;
    }
    const char *kt = key_threads_json ? key_threads_json : "[]";

    e = repo.vtable->store_active(repo.ctx, chapter->theme[0] ? chapter->theme : "",
                                  chapter->mood[0] ? chapter->mood : "", chapter->started_at, kt);

    if (key_threads_json)
        alloc->free(alloc->ctx, key_threads_json, kt_len + 1);
    repo.vtable->deinit(repo.ctx);
    return e;
}

char *hu_life_chapter_build_directive(hu_allocator_t *alloc, const hu_life_chapter_t *chapter,
                                      size_t *out_len) {
    if (!alloc || !chapter || !out_len)
        return NULL;
    *out_len = 0;

    if (!chapter->theme[0] && !chapter->mood[0])
        return NULL;

    const char *theme = chapter->theme[0] ? chapter->theme : "life";
    const char *mood = chapter->mood[0] ? chapter->mood : "";

    char threads_buf[1024] = {0};
    size_t pos = 0;
    for (size_t i = 0; i < chapter->key_threads_count && i < 8 && pos < sizeof(threads_buf) - 64;
         i++) {
        if (chapter->key_threads[i][0]) {
            if (pos > 0) {
                pos = hu_buf_appendf(threads_buf, sizeof(threads_buf), pos, ", ");
            }
            pos = hu_buf_appendf(threads_buf, sizeof(threads_buf), pos, "%.127s",
                                 chapter->key_threads[i]);
        }
    }
    const char *threads_str = threads_buf[0] ? threads_buf : "none";

    size_t cap = 512;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return NULL;
    int n = snprintf(buf, cap,
                     "[LIFE CHAPTER: You're in a phase of %s. %s. Key threads: %s. "
                     "Reference naturally when relevant.]",
                     theme, mood[0] ? mood : "(no mood)", threads_str);
    if (n < 0 || (size_t)n >= cap) {
        alloc->free(alloc->ctx, buf, cap);
        return NULL;
    }
    *out_len = (size_t)n;
    return buf;
}

#endif /* HU_ENABLE_SQLITE */
