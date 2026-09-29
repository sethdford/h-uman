/* src/memory/repos/chatdb_cursor_repo_sqlite.c — see the header. */
#include "human/memory/chatdb_cursor_repo.h"

#include <stddef.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

hu_error_t hu_chatdb_max_rowid(const char *chatdb_path, int64_t *out_max_rowid) {
    if (!chatdb_path || !*chatdb_path || !out_max_rowid)
        return HU_ERR_INVALID_ARGUMENT;
#ifndef HU_ENABLE_SQLITE
    return HU_ERR_NOT_SUPPORTED;
#else
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(chatdb_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return HU_ERR_IO;
    }
    sqlite3_stmt *st = NULL;
    hu_error_t err = HU_ERR_IO;
    if (sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(ROWID),0) FROM message", -1, &st, NULL) ==
            SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        *out_max_rowid = (int64_t)sqlite3_column_int64(st, 0);
        err = HU_OK;
    }
    if (st)
        sqlite3_finalize(st);
    sqlite3_close(db);
    return err;
#endif
}
