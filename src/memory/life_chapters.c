typedef int hu_life_chapters_unused_;

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/life_chapters.h"
#include "human/persona.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

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
    for (size_t i = 0; i < n && root->data.array.items[i]; i++) {
        hu_json_value_t *item = root->data.array.items[i];
        if (item->type == HU_JSON_STRING && item->data.string.ptr) {
            size_t len = item->data.string.len;
            if (len > 127)
                len = 127;
            snprintf(out->key_threads[i], sizeof(out->key_threads[i]), "%.*s", (int)len,
                     item->data.string.ptr);
        }
    }
    out->key_threads_count = n;
    hu_json_free(alloc, root);
    return HU_OK;
}

hu_error_t hu_life_chapter_get_active(hu_allocator_t *alloc, hu_memory_t *memory,
                                     hu_life_chapter_t *out) {
    if (!alloc || !memory || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT theme, mood, started_at, key_threads "
                                "FROM life_chapters WHERE active=1 "
                                "ORDER BY started_at DESC LIMIT 1",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return HU_OK;
    }

    const char *theme = (const char *)sqlite3_column_text(stmt, 0);
    if (theme) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 0);
        if (len > 255)
            len = 255;
        snprintf(out->theme, sizeof(out->theme), "%.*s", (int)len, theme);
    }
    const char *mood = (const char *)sqlite3_column_text(stmt, 1);
    if (mood) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 1);
        if (len > 63)
            len = 63;
        snprintf(out->mood, sizeof(out->mood), "%.*s", (int)len, mood);
    }
    out->started_at = sqlite3_column_int64(stmt, 2);
    const char *kt = (const char *)sqlite3_column_text(stmt, 3);
    if (kt) {
        size_t kt_len = (size_t)sqlite3_column_bytes(stmt, 3);
        parse_key_threads(alloc, kt, kt_len, out);
    }

    sqlite3_finalize(stmt);
    return HU_OK;
}

hu_error_t hu_life_chapter_store(hu_allocator_t *alloc, hu_memory_t *memory,
                                const hu_life_chapter_t *chapter) {
    if (!alloc || !memory || !chapter)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    /* Deactivate all previous chapters */
    sqlite3_stmt *upd = NULL;
    int rc = sqlite3_prepare_v2(db, "UPDATE life_chapters SET active=0", -1, &upd, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_step(upd);
    sqlite3_finalize(upd);

    size_t kt_len = 0;
    char *key_threads_json = key_threads_to_json(alloc, chapter, &kt_len);
    if (!key_threads_json && chapter->key_threads_count > 0) {
        return HU_ERR_OUT_OF_MEMORY;
    }
    const char *kt = key_threads_json ? key_threads_json : "[]";
    if (!key_threads_json)
        kt_len = 2;

    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(db,
                            "INSERT INTO life_chapters(theme,mood,started_at,key_threads,active) "
                            "VALUES(?,?,?,?,1)",
                            -1, &ins, NULL);
    if (rc != SQLITE_OK) {
        if (key_threads_json)
            alloc->free(alloc->ctx, key_threads_json, kt_len + 1);
        return HU_ERR_MEMORY_BACKEND;
    }
    sqlite3_bind_text(ins, 1, chapter->theme, -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, chapter->mood, -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 3, chapter->started_at);
    sqlite3_bind_text(ins, 4, kt, (int)kt_len, SQLITE_STATIC);
    rc = sqlite3_step(ins);
    sqlite3_finalize(ins);
    if (key_threads_json)
        alloc->free(alloc->ctx, key_threads_json, kt_len + 1);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

char *hu_life_chapter_build_directive(hu_allocator_t *alloc,
                                     const hu_life_chapter_t *chapter, size_t *out_len) {
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
                pos += (size_t)snprintf(threads_buf + pos, sizeof(threads_buf) - pos, ", ");
            }
            pos += (size_t)snprintf(threads_buf + pos, sizeof(threads_buf) - pos, "%.127s",
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
