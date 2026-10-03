/*
 * src/memory/repos/briefing_repo_sqlite.c
 *
 * One morning briefing per local day. Contract: include/human/memory/briefing_repo.h.
 */
#include "human/memory/briefing_repo.h"

#ifdef HU_ENABLE_SQLITE

#include "human/memory/repo_util.h"
#include <stddef.h>

hu_error_t hu_briefing_repo_ensure_schema(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    return hu_repo_exec_ddl(db, "CREATE TABLE IF NOT EXISTS briefing_days ("
                                "  day TEXT PRIMARY KEY,"
                                "  claimed_at INTEGER NOT NULL,"
                                "  mode TEXT NOT NULL"
                                ");");
}

hu_error_t hu_briefing_repo_claim(sqlite3 *db, const char *day, int64_t now, const char *mode,
                                  bool *claimed) {
    if (!db || !day || !day[0] || !mode || !claimed)
        return HU_ERR_INVALID_ARGUMENT;
    *claimed = false;
    hu_error_t err = hu_briefing_repo_ensure_schema(db);
    if (err != HU_OK)
        return err;
    sqlite3_stmt *stmt = NULL;
    /* A plain INSERT: the primary key refuses a second claim for the day. */
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO briefing_days (day, claimed_at, mode) VALUES (?1, ?2, ?3);",
                           -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, day, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, now);
    sqlite3_bind_text(stmt, 3, mode, -1, SQLITE_STATIC);
    return hu_repo_step_insert(stmt, claimed); /* the day's key refuses a second claim */
}

hu_error_t hu_briefing_repo_release(sqlite3 *db, const char *day) {
    if (!db || !day || !day[0])
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM briefing_days WHERE day=?1;", -1, &stmt, NULL) !=
        SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, day, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_STORE;
}

#endif /* HU_ENABLE_SQLITE */
