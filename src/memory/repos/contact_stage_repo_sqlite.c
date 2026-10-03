/*
 * src/memory/repos/contact_stage_repo_sqlite.c
 *
 * SQLite-backed contact-stage counts and the derived-stage table. Contract in
 * include/human/memory/contact_stage_repo.h (DEF-16).
 */
#include "human/memory/contact_stage_repo.h"

#ifdef HU_ENABLE_SQLITE

#include "human/memory/repo_util.h"

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
    static const char sql[] = "SELECT session_id, COUNT(*), COUNT(DISTINCT date(created_at)) "
                              "FROM messages WHERE role = 'user' GROUP BY session_id";
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
           (uint32_t)sqlite3_column_int64(stmt, 2));
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_contact_stage_repo_contact_counts(sqlite3 *db, const char *contact,
                                                size_t contact_len, uint32_t *inbound,
                                                uint32_t *active_days) {
    if (!db || !contact || contact_len == 0 || !inbound || !active_days)
        return HU_ERR_INVALID_ARGUMENT;
    *inbound = 0;
    *active_days = 0;
    if (!table_exists(db, "messages"))
        return HU_OK;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT COUNT(*), COUNT(DISTINCT date(created_at)) FROM messages "
                           "WHERE session_id = ?1 AND role = 'user'",
                           -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(stmt, 1, contact, (int)contact_len, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *inbound = (uint32_t)sqlite3_column_int64(stmt, 0);
        *active_days = (uint32_t)sqlite3_column_int64(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

static hu_error_t stage_counts(sqlite3 *db, const char *sql, const char *table,
                               uint32_t counts[4]) {
    if (!db || !counts)
        return HU_ERR_INVALID_ARGUMENT;
    memset(counts, 0, 4 * sizeof(uint32_t));
    if (!table_exists(db, table))
        return HU_OK;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
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

hu_error_t hu_contact_stage_repo_persisted_counts(sqlite3 *db, uint32_t counts[4]) {
    return stage_counts(db, "SELECT rel_stage, COUNT(*) FROM frontier_state GROUP BY rel_stage",
                        "frontier_state", counts);
}

hu_error_t hu_contact_stage_repo_derived_counts(sqlite3 *db, uint32_t counts[4]) {
    return stage_counts(db, "SELECT stage, COUNT(*) FROM contact_rel_stage GROUP BY stage",
                        "contact_rel_stage", counts);
}

hu_error_t hu_contact_stage_repo_save_derived(sqlite3 *db, const char *contact, size_t contact_len,
                                              int stage, double quality, uint32_t inbound,
                                              uint32_t active_days, int64_t seth_replies,
                                              int64_t now_unix) {
    if (!db || !contact || contact_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t e = hu_repo_exec_ddl(db, "CREATE TABLE IF NOT EXISTS contact_rel_stage ("
                                        "  contact_id TEXT PRIMARY KEY,"
                                        "  stage INTEGER NOT NULL,"
                                        "  quality REAL NOT NULL,"
                                        "  inbound INTEGER NOT NULL,"
                                        "  active_days INTEGER NOT NULL,"
                                        "  seth_replies INTEGER NOT NULL,"
                                        "  updated_at INTEGER NOT NULL);");
    if (e != HU_OK)
        return e;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO contact_rel_stage (contact_id, stage, quality, inbound, "
                           "active_days, seth_replies, updated_at) VALUES (?1,?2,?3,?4,?5,?6,?7) "
                           "ON CONFLICT(contact_id) DO UPDATE SET stage=excluded.stage, "
                           "quality=excluded.quality, inbound=excluded.inbound, "
                           "active_days=excluded.active_days, seth_replies=excluded.seth_replies, "
                           "updated_at=excluded.updated_at;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, contact, (int)contact_len, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, stage);
    sqlite3_bind_double(stmt, 3, quality);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)inbound);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)active_days);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)seth_replies);
    sqlite3_bind_int64(stmt, 7, (sqlite3_int64)now_unix);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_STORE;
}

hu_error_t hu_contact_stage_repo_get_derived(sqlite3 *db, const char *contact, size_t contact_len,
                                             int *stage, double *quality, int64_t *seth_replies) {
    if (!db || !contact || contact_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!table_exists(db, "contact_rel_stage"))
        return HU_ERR_NOT_FOUND;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT stage, quality, seth_replies FROM contact_rel_stage "
                           "WHERE contact_id = ?1",
                           -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(stmt, 1, contact, (int)contact_len, SQLITE_STATIC);
    hu_error_t err = HU_ERR_NOT_FOUND;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (stage)
            *stage = sqlite3_column_int(stmt, 0);
        if (quality)
            *quality = sqlite3_column_double(stmt, 1);
        if (seth_replies)
            *seth_replies = sqlite3_column_int64(stmt, 2);
        err = HU_OK;
    }
    sqlite3_finalize(stmt);
    return err;
}

#endif /* HU_ENABLE_SQLITE */
