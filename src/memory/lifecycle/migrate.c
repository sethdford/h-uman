#include "seaclaw/memory/lifecycle/migrate.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SC_ENABLE_SQLITE
#include <sqlite3.h>

static int table_exists(sqlite3 *db, const char *table) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT name FROM sqlite_master WHERE type='table' AND name=?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
    int has = (sqlite3_step(stmt) == SQLITE_ROW) ? 1 : 0;
    sqlite3_finalize(stmt);
    return has;
}

static void detect_columns(sqlite3 *db, const char **key_expr, const char **content_col,
                           const char **category_expr) {
    static char key_buf[64], content_buf[64], category_buf[64];
    int has_key = 0, has_id = 0, has_name = 0;
    int has_content = 0, has_value = 0, has_text = 0, has_memory = 0;
    int has_category = 0, has_kind = 0, has_type = 0;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(memories)", -1, &stmt, NULL) != SQLITE_OK) {
        *key_expr = "CAST(rowid AS TEXT)";
        *content_col = "content";
        *category_expr = "'core'";
        return;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *col = (const char *)sqlite3_column_text(stmt, 1);
        if (!col)
            continue;
        if (strcmp(col, "key") == 0)
            has_key = 1;
        else if (strcmp(col, "id") == 0)
            has_id = 1;
        else if (strcmp(col, "name") == 0)
            has_name = 1;
        else if (strcmp(col, "content") == 0)
            has_content = 1;
        else if (strcmp(col, "value") == 0)
            has_value = 1;
        else if (strcmp(col, "text") == 0)
            has_text = 1;
        else if (strcmp(col, "memory") == 0)
            has_memory = 1;
        else if (strcmp(col, "category") == 0)
            has_category = 1;
        else if (strcmp(col, "kind") == 0)
            has_kind = 1;
        else if (strcmp(col, "type") == 0)
            has_type = 1;
    }
    sqlite3_finalize(stmt);

    if (has_content)
        strcpy(content_buf, "content");
    else if (has_value)
        strcpy(content_buf, "value");
    else if (has_text)
        strcpy(content_buf, "text");
    else if (has_memory)
        strcpy(content_buf, "memory");
    else {
        *content_col = NULL;
        return;
    }

    if (has_key)
        strcpy(key_buf, "key");
    else if (has_id)
        strcpy(key_buf, "id");
    else if (has_name)
        strcpy(key_buf, "name");
    else
        strcpy(key_buf, "CAST(rowid AS TEXT)");

    if (has_category)
        strcpy(category_buf, "category");
    else if (has_kind)
        strcpy(category_buf, "kind");
    else if (has_type)
        strcpy(category_buf, "type");
    else
        strcpy(category_buf, "'core'");

    *key_expr = key_buf;
    *content_col = content_buf;
    *category_expr = category_buf;
}
#endif /* SC_ENABLE_SQLITE */

sc_error_t sc_migrate_read_brain_db(sc_allocator_t *alloc, const char *db_path,
                                    sc_sqlite_source_entry_t **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;
    if (!alloc || !db_path)
        return SC_ERR_INVALID_ARGUMENT;

#ifndef SC_ENABLE_SQLITE
    (void)alloc;
    (void)db_path;
    return SC_ERR_NOT_SUPPORTED;
#else
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return SC_ERR_INVALID_ARGUMENT;
    }

    if (!table_exists(db, "memories")) {
        sqlite3_close(db);
        return SC_ERR_INVALID_ARGUMENT;
    }

    const char *key_expr = NULL, *content_col = NULL, *category_expr = NULL;
    detect_columns(db, &key_expr, &content_col, &category_expr);
    if (!content_col) {
        sqlite3_close(db);
        return SC_ERR_INVALID_ARGUMENT;
    }

    char sql[320];
    snprintf(sql, sizeof(sql), "SELECT %s, %s, %s FROM memories", key_expr, content_col,
             category_expr);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return SC_ERR_INTERNAL;
    }

    size_t cap = 64, count = 0;
    sc_sqlite_source_entry_t *entries = (sc_sqlite_source_entry_t *)alloc->alloc(
        alloc->ctx, cap * sizeof(sc_sqlite_source_entry_t));
    if (!entries) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return SC_ERR_OUT_OF_MEMORY;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *rk = (const char *)sqlite3_column_text(stmt, 0);
        const char *rc = (const char *)sqlite3_column_text(stmt, 1);
        const char *rcat = (const char *)sqlite3_column_text(stmt, 2);
        int rclen = sqlite3_column_bytes(stmt, 1);
        if (!rc || rclen <= 0)
            continue;

        size_t klen = rk ? (size_t)sqlite3_column_bytes(stmt, 0) : 0;
        size_t clen = (size_t)rclen;
        size_t catlen = rcat ? (size_t)sqlite3_column_bytes(stmt, 2) : 0;

        if (count >= cap) {
            cap *= 2;
            sc_sqlite_source_entry_t *n = (sc_sqlite_source_entry_t *)alloc->realloc(
                alloc->ctx, entries, count * sizeof(sc_sqlite_source_entry_t),
                cap * sizeof(sc_sqlite_source_entry_t));
            if (!n)
                break;
            entries = n;
        }

        char *key = (char *)alloc->alloc(alloc->ctx, klen + 1);
        char *content = (char *)alloc->alloc(alloc->ctx, clen + 1);
        char *category = (char *)alloc->alloc(alloc->ctx, catlen + 1);
        if (!key || !content || !category) {
            if (key)
                alloc->free(alloc->ctx, key, klen + 1);
            if (content)
                alloc->free(alloc->ctx, content, clen + 1);
            if (category)
                alloc->free(alloc->ctx, category, catlen + 1);
            break;
        }
        if (rk)
            memcpy(key, rk, klen);
        key[klen] = '\0';
        memcpy(content, rc, clen);
        content[clen] = '\0';
        if (rcat)
            memcpy(category, rcat, catlen);
        category[catlen] = '\0';

        entries[count].key = key;
        entries[count].content = content;
        entries[count].category = category;
        count++;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    *out = entries;
    *out_count = count;
    return SC_OK;
#endif
}

void sc_migrate_free_entries(sc_allocator_t *alloc, sc_sqlite_source_entry_t *entries,
                             size_t count) {
    if (!alloc || !entries)
        return;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].key)
            alloc->free(alloc->ctx, entries[i].key, strlen(entries[i].key) + 1);
        if (entries[i].content)
            alloc->free(alloc->ctx, entries[i].content, strlen(entries[i].content) + 1);
        if (entries[i].category)
            alloc->free(alloc->ctx, entries[i].category, strlen(entries[i].category) + 1);
    }
    alloc->free(alloc->ctx, entries, count * sizeof(sc_sqlite_source_entry_t));
}
