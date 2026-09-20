#ifdef HU_ENABLE_SQLITE

#include "human/memory/multimodal_index.h"
#include "human/core/time.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DESC_COPY_LEN 511

static int64_t now_ms(void) {
    return (int64_t)hu_time_wall_ms();
}

hu_error_t hu_multimodal_memory_init_tables(sqlite3 *db) {
    sqlite3 *sqlite_db = db;
    if (!sqlite_db)
        return HU_ERR_INVALID_ARGUMENT;
    const char *sql = "CREATE TABLE IF NOT EXISTS multimodal_memories ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                      "modality INTEGER NOT NULL,"
                      "description TEXT NOT NULL,"
                      "created_at INTEGER NOT NULL)";
    int rc = sqlite3_exec(sqlite_db, sql, NULL, NULL, NULL);
    return (rc == SQLITE_OK) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_multimodal_memory_store(hu_allocator_t *alloc, sqlite3 *db, hu_modality_t type,
                                      const char *description, size_t desc_len) {
    (void)alloc;
    sqlite3 *sqlite_db = db;
    if (!alloc || !sqlite_db || !description)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *stmt = NULL;
    int rc =
        sqlite3_prepare_v2(sqlite_db,
                           "INSERT INTO multimodal_memories(modality, description, created_at) "
                           "VALUES(?, ?, ?)",
                           -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_int(stmt, 1, (int)type);
    sqlite3_bind_text(stmt, 2, description, (int)desc_len, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, now_ms());
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_multimodal_memory_search(hu_allocator_t *alloc, sqlite3 *db, const char *query,
                                       size_t query_len, hu_multimodal_memory_entry_t *results,
                                       size_t max_results, size_t *out_count) {
    (void)alloc;
    sqlite3 *sqlite_db = db;
    if (!sqlite_db || !results || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    if (max_results == 0)
        return HU_OK;

    char pattern[1024];
    if (query_len >= sizeof(pattern) - 4)
        return HU_ERR_INVALID_ARGUMENT;
    int n = snprintf(pattern, sizeof(pattern), "%%%.*s%%", (int)query_len, query);
    if (n <= 0 || (size_t)n >= sizeof(pattern))
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(sqlite_db,
                                "SELECT id, modality, description, created_at "
                                "FROM multimodal_memories WHERE description LIKE ? "
                                "ORDER BY created_at DESC LIMIT ?",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)max_results);

    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_results) {
        hu_multimodal_memory_entry_t *e = &results[count];
        e->id = sqlite3_column_int64(stmt, 0);
        e->modality = (hu_modality_t)sqlite3_column_int(stmt, 1);
        const char *desc = (const char *)sqlite3_column_text(stmt, 2);
        size_t desc_len = desc ? (size_t)sqlite3_column_bytes(stmt, 2) : 0;
        if (desc_len > DESC_COPY_LEN)
            desc_len = DESC_COPY_LEN;
        if (desc && desc_len > 0)
            memcpy(e->description, desc, desc_len);
        e->description[desc_len] = '\0';
        e->description_len = desc_len;
        e->created_at = sqlite3_column_int64(stmt, 3);
        count++;
    }
    sqlite3_finalize(stmt);
    *out_count = count;
    return HU_OK;
}

hu_error_t hu_multimodal_memory_search_cross_modal(hu_allocator_t *alloc, sqlite3 *db,
                                                   const char *query, size_t query_len,
                                                   hu_modality_t target_modality,
                                                   hu_multimodal_memory_entry_t *results,
                                                   size_t max_results, size_t *out_count) {
    (void)alloc;
    sqlite3 *sqlite_db = db;
    if (!sqlite_db || !results || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    if (max_results == 0)
        return HU_OK;

    char pattern[1024];
    if (query_len >= sizeof(pattern) - 4)
        return HU_ERR_INVALID_ARGUMENT;
    int n = snprintf(pattern, sizeof(pattern), "%%%.*s%%", (int)query_len, query);
    if (n <= 0 || (size_t)n >= sizeof(pattern))
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(sqlite_db,
                                "SELECT id, modality, description, created_at "
                                "FROM multimodal_memories "
                                "WHERE description LIKE ? AND modality = ? "
                                "ORDER BY created_at DESC LIMIT ?",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, (int)target_modality);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)max_results);

    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_results) {
        hu_multimodal_memory_entry_t *e = &results[count];
        e->id = sqlite3_column_int64(stmt, 0);
        e->modality = (hu_modality_t)sqlite3_column_int(stmt, 1);
        const char *desc = (const char *)sqlite3_column_text(stmt, 2);
        size_t desc_len = desc ? (size_t)sqlite3_column_bytes(stmt, 2) : 0;
        if (desc_len > DESC_COPY_LEN)
            desc_len = DESC_COPY_LEN;
        if (desc && desc_len > 0)
            memcpy(e->description, desc, desc_len);
        e->description[desc_len] = '\0';
        e->description_len = desc_len;
        e->created_at = sqlite3_column_int64(stmt, 3);
        count++;
    }
    sqlite3_finalize(stmt);
    *out_count = count;
    return HU_OK;
}

#endif /* HU_ENABLE_SQLITE */

typedef int hu_multimodal_index_unused_t;
