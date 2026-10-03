#ifndef HU_MEMORY_REPO_UTIL_H
#define HU_MEMORY_REPO_UTIL_H
/*
 * Shared helpers for the free-function repositories under src/memory/repos/.
 * Extracted 2026-09-20 when a second repo copied the first one's
 * ensure_schema tail verbatim (clone ratchet). Recall (memory) bounded
 * context; legal sqlite3 includer like the repos themselves.
 */
#include "human/core/error.h"
#include <stdbool.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

/* Run one or more DDL statements (CREATE TABLE IF NOT EXISTS ...). The
 * sqlite error text is released; the caller gets HU_ERR_MEMORY_STORE. */
hu_error_t hu_repo_exec_ddl(sqlite3 *db, const char *ddl);

/* Step a prepared INSERT once and finalize it. *inserted is false, and the
 * result still HU_OK, when a UNIQUE or PRIMARY KEY constraint refused the
 * row: "already there" is an answer, not a failure. */
hu_error_t hu_repo_step_insert(sqlite3_stmt *stmt, bool *inserted);

/* Step a prepared UPDATE or DELETE once and finalize it: HU_OK when exactly
 * one row changed, HU_ERR_NOT_FOUND when none did. */
hu_error_t hu_repo_step_update_one(sqlite3 *db, sqlite3_stmt *stmt);

#endif /* HU_ENABLE_SQLITE */
#endif /* HU_MEMORY_REPO_UTIL_H */
