#ifndef HU_MEMORY_REPO_UTIL_H
#define HU_MEMORY_REPO_UTIL_H
/*
 * Shared helpers for the free-function repositories under src/memory/repos/.
 * Extracted 2026-09-20 when a second repo copied the first one's
 * ensure_schema tail verbatim (clone ratchet). Recall (memory) bounded
 * context; legal sqlite3 includer like the repos themselves.
 */
#include "human/core/error.h"

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

/* Run one or more DDL statements (CREATE TABLE IF NOT EXISTS ...). The
 * sqlite error text is released; the caller gets HU_ERR_MEMORY_STORE. */
hu_error_t hu_repo_exec_ddl(sqlite3 *db, const char *ddl);

#endif /* HU_ENABLE_SQLITE */
#endif /* HU_MEMORY_REPO_UTIL_H */
