/*
 * src/memory/repos/contact_optout_repo_sqlite.c
 *
 * SQLite-backed contact opt-out repository. Contract in
 * include/human/memory/contact_optout_repo.h (October roadmap O5).
 */
#include "human/memory/contact_optout_repo.h"

#ifdef HU_ENABLE_SQLITE

#include "human/memory/repo_util.h"
#include <string.h>

hu_error_t hu_contact_optout_repo_ensure_schema(sqlite3 *db) {
    static const char *kSchema = "CREATE TABLE IF NOT EXISTS contact_suppressions ("
                                 "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                 "  contact TEXT NOT NULL UNIQUE,"
                                 "  ts INTEGER NOT NULL,"
                                 "  reason TEXT,"
                                 "  excerpt TEXT"
                                 ");";
    return hu_repo_exec_ddl(db, kSchema);
}

hu_error_t hu_contact_optout_repo_record(sqlite3 *db, int64_t ts, const char *contact,
                                         const char *reason, const char *excerpt) {
    if (!db || !contact || !contact[0])
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t e = hu_contact_optout_repo_ensure_schema(db);
    if (e != HU_OK)
        return e;
    static const char *kUpsert =
        "INSERT INTO contact_suppressions (contact, ts, reason, excerpt) VALUES (?1, ?2, ?3, ?4) "
        "ON CONFLICT(contact) DO UPDATE SET ts=excluded.ts, reason=excluded.reason, "
        "excerpt=excluded.excerpt;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, kUpsert, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, contact, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, ts);
    if (reason)
        sqlite3_bind_text(stmt, 3, reason, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 3);
    if (excerpt)
        sqlite3_bind_text(stmt, 4, excerpt, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 4);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_STORE;
}

hu_error_t hu_contact_optout_repo_is_suppressed(sqlite3 *db, const char *contact, bool *out) {
    if (!db || !contact || !contact[0] || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = false;
    hu_error_t e = hu_contact_optout_repo_ensure_schema(db);
    if (e != HU_OK)
        return e;
    static const char *kSel = "SELECT 1 FROM contact_suppressions WHERE contact = ?1 LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, kSel, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, contact, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_ROW) {
        *out = true;
        return HU_OK;
    }
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_STORE;
}

hu_error_t hu_contact_optout_repo_clear(sqlite3 *db, const char *contact) {
    if (!db || !contact || !contact[0])
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t e = hu_contact_optout_repo_ensure_schema(db);
    if (e != HU_OK)
        return e;
    static const char *kDel = "DELETE FROM contact_suppressions WHERE contact = ?1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, kDel, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, contact, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_STORE;
}

hu_error_t hu_contact_optout_repo_count(sqlite3 *db, int64_t *out) {
    if (!db || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = 0;
    hu_error_t e = hu_contact_optout_repo_ensure_schema(db);
    if (e != HU_OK)
        return e;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM contact_suppressions;", -1, &stmt, NULL) !=
        SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        *out = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW ? HU_OK : HU_ERR_MEMORY_STORE;
}

#endif /* HU_ENABLE_SQLITE */
