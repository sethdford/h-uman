/* include/human/memory/chatdb_cursor_repo.h
 *
 * Read-only repository over the Messages database for cursor bookkeeping.
 * Lives behind the memory-repository boundary so domain/doctor code never
 * includes <sqlite3.h> (see .claude/rules/sqlite-includer-ratchet.md). */
#ifndef HU_MEMORY_CHATDB_CURSOR_REPO_H
#define HU_MEMORY_CHATDB_CURSOR_REPO_H

#include "human/core/error.h"

#include <stdint.h>

/* MAX(ROWID) of chat.db's `message` table (0 when the table is empty).
 * HU_ERR_NOT_SUPPORTED without SQLite; HU_ERR_IO when the file cannot be
 * opened read-only or has no message table. Never creates the file. */
hu_error_t hu_chatdb_max_rowid(const char *chatdb_path, int64_t *out_max_rowid);

#endif /* HU_MEMORY_CHATDB_CURSOR_REPO_H */
