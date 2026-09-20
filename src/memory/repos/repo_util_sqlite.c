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

#endif /* HU_ENABLE_SQLITE */
