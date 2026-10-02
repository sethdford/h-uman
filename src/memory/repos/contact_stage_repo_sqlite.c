/*
 * src/memory/repos/contact_stage_repo_sqlite.c
 *
 * SQLite-backed contact-stage counts. Contract in
 * include/human/memory/contact_stage_repo.h (DEF-16).
 */
#include "human/memory/contact_stage_repo.h"

#ifdef HU_ENABLE_SQLITE

#include <string.h>

static int table_exists(sqlite3 *db, const char *name) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1", -1,
                           &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    int found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

hu_error_t hu_contact_stage_repo_each_contact(sqlite3 *db, hu_contact_stage_repo_row_fn fn,
                                              void *ctx) {
    if (!db || !fn)
        return HU_ERR_INVALID_ARGUMENT;
    if (!table_exists(db, "messages"))
        return HU_OK;
    static const char sql[] =
        "SELECT session_id, SUM(role = 'user'), SUM(role = 'assistant'), "
        "COUNT(DISTINCT date(created_at)) FROM messages GROUP BY session_id ORDER BY session_id";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        int id_len = sqlite3_column_bytes(stmt, 0);
        if (!id || id_len <= 0)
            continue;
        fn(ctx, id, (size_t)id_len, (uint32_t)sqlite3_column_int64(stmt, 1),
           (uint32_t)sqlite3_column_int64(stmt, 2), (uint32_t)sqlite3_column_int64(stmt, 3));
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_contact_stage_repo_update_persisted(sqlite3 *db, const char *contact,
                                                  size_t contact_len, int stage, int sessions,
                                                  int turns) {
    if (!db || !contact || contact_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!table_exists(db, "frontier_state"))
        return HU_OK;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "UPDATE frontier_state SET rel_stage = ?1, rel_session_count = ?2, "
                           "rel_total_turns = ?3 WHERE contact_id = ?4",
                           -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_int(stmt, 1, stage);
    sqlite3_bind_int(stmt, 2, sessions);
    sqlite3_bind_int(stmt, 3, turns);
    sqlite3_bind_text(stmt, 4, contact, (int)contact_len, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_STORE;
}

hu_error_t hu_contact_stage_repo_persisted_counts(sqlite3 *db, uint32_t counts[4]) {
    if (!db || !counts)
        return HU_ERR_INVALID_ARGUMENT;
    memset(counts, 0, 4 * sizeof(uint32_t));
    if (!table_exists(db, "frontier_state"))
        return HU_OK;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT rel_stage, COUNT(*) FROM frontier_state GROUP BY rel_stage",
                           -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int stage = sqlite3_column_int(stmt, 0);
        if (stage >= 0 && stage < 4)
            counts[stage] = (uint32_t)sqlite3_column_int64(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

#endif /* HU_ENABLE_SQLITE */
