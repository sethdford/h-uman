/* src/memory/repos/repo_util_sqlite.c — see include/human/memory/repo_util.h */
#include "human/memory/repo_util.h"

#ifdef HU_ENABLE_SQLITE

#include <stddef.h>

hu_error_t hu_repo_exec_ddl(sqlite3 *db, const char *ddl) {
    if (!db || !ddl)
        return HU_ERR_INVALID_ARGUMENT;
    char *errmsg = NULL;
    if (sqlite3_exec(db, ddl, NULL, NULL, &errmsg) != SQLITE_OK) {
        if (errmsg)
            sqlite3_free(errmsg);
        return HU_ERR_MEMORY_STORE;
    }
    return HU_OK;
}

hu_error_t hu_repo_step_insert(sqlite3_stmt *stmt, bool *inserted) {
    if (inserted)
        *inserted = false;
    if (!stmt)
        return HU_ERR_INVALID_ARGUMENT;
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {
        if (inserted)
            *inserted = true;
        return HU_OK;
    }
    return (rc & 0xff) == SQLITE_CONSTRAINT ? HU_OK : HU_ERR_MEMORY_STORE;
}

hu_error_t hu_repo_step_update_one(sqlite3 *db, sqlite3_stmt *stmt) {
    if (!db || !stmt) {
        sqlite3_finalize(stmt);
        return HU_ERR_INVALID_ARGUMENT;
    }
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_STORE;
    return sqlite3_changes(db) == 1 ? HU_OK : HU_ERR_NOT_FOUND;
}

#endif /* HU_ENABLE_SQLITE */
