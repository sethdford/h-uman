#ifndef HU_MEMORY_SQL_COMMON_H
#define HU_MEMORY_SQL_COMMON_H

/*
 * Shared SQL fragments for memory engine backends.
 * Avoids duplicating DDL strings across sqlite.c, sqlite_lucid.c, sqlite_fts.c.
 */

#define HU_SQL_PRAGMA_INIT     \
    "PRAGMA secure_delete=ON;" \
    "PRAGMA journal_mode=WAL;" \
    "PRAGMA foreign_keys=ON;"

#define HU_SQL_MEMORIES_TABLE                                     \
    "CREATE TABLE IF NOT EXISTS memories("                        \
    "id TEXT PRIMARY KEY,key TEXT NOT NULL UNIQUE,"               \
    "content TEXT NOT NULL,category TEXT NOT NULL DEFAULT'core'," \
    "session_id TEXT,created_at TEXT NOT NULL,updated_at TEXT NOT NULL)"

#define HU_SQL_MEMORIES_INDEXES                                               \
    "CREATE INDEX IF NOT EXISTS idx_memories_category ON memories(category);" \
    "CREATE INDEX IF NOT EXISTS idx_memories_key ON memories(key);"           \
    "CREATE INDEX IF NOT EXISTS idx_memories_session ON memories(session_id);"

#define HU_SQL_MEMORIES_UPSERT                                                               \
    "INSERT INTO memories (id, key, content, category, session_id, created_at, updated_at) " \
    "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) "                                                   \
    "ON CONFLICT(key) DO UPDATE SET content = excluded.content, "                            \
    "category = excluded.category, session_id = excluded.session_id, "                       \
    "updated_at = excluded.updated_at"

#define HU_SQL_MESSAGES_TABLE                      \
    "CREATE TABLE IF NOT EXISTS messages("         \
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"        \
    "session_id TEXT NOT NULL,role TEXT NOT NULL," \
    "content TEXT NOT NULL,created_at TEXT DEFAULT(datetime('now')))"

#define HU_SQL_KV_TABLE "CREATE TABLE IF NOT EXISTS kv(key TEXT PRIMARY KEY,value TEXT NOT NULL)"

/* ===== Corruption self-heal (resilience) =====
 * A corrupted on-disk memory DB must never be read as silent garbage. These
 * helpers let an engine open path detect corruption and recover loudly:
 * quick_check the freshly-opened handle; if it fails, quarantine the file
 * (preserved for manual recovery) and reopen fresh. Implemented in
 * src/memory/engines/sqlite.c (same TU + HU_ENABLE_SQLITE gate as the engine).
 *
 * Cost gating: PRAGMA quick_check walks every btree page — minutes of pread at
 * 100% CPU on a multi-GB DB — so the engine only runs the full scan when the
 * previous process did NOT close the DB cleanly. That is tracked by an
 * unclean-shutdown sentinel (<db_path>.open-sentinel): written right after a
 * successful open, removed on clean close (deinit). Presence at the next open
 * ⇒ prior unclean shutdown ⇒ full quick_check (+ quarantine on failure). On
 * the clean-shutdown fast path, gross corruption (bad header / broken master
 * btree) is still healed cheaply when schema init fails. */
#include <stdbool.h>
struct sqlite3;

/* Pure predicate: runs `PRAGMA quick_check` and returns true IFF the first
 * result row is exactly "ok". Cheaper than integrity_check; catches the
 * page/btree corruption and not-a-database classes that matter. NULL handle, a
 * prepare failure (e.g. SQLITE_NOTADB), or any non-"ok" row → false. */
bool hu_sqlite_quick_check_ok(struct sqlite3 *db);

/* Move a corrupt DB file aside: rename <path> (and its -wal/-shm siblings, best
 * effort) to <path>.corrupt-<unix_ts> so the bytes are preserved for manual
 * recovery but out of the way of a fresh DB at the original path. Returns true
 * if the main file was renamed. No-op returning false for NULL / empty /
 * ":memory:" (nothing on disk to quarantine). */
bool hu_sqlite_quarantine_corrupt_file(const char *db_path);

/* Unclean-shutdown sentinel (<db_path>.open-sentinel). present: does the
 * sentinel exist? write: create it (returns false when there is nothing on
 * disk to guard — NULL / empty / ":memory:" — or on I/O failure). remove:
 * best-effort delete on clean close. */
bool hu_sqlite_open_sentinel_present(const char *db_path);
bool hu_sqlite_open_sentinel_write(const char *db_path);
void hu_sqlite_open_sentinel_remove(const char *db_path);

#endif /* HU_MEMORY_SQL_COMMON_H */
