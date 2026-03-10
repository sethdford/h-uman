#ifndef HU_MEMORY_SQL_COMMON_H
#define HU_MEMORY_SQL_COMMON_H

/*
 * Shared SQL fragments for memory engine backends.
 * Avoids duplicating DDL strings across sqlite.c, lucid.c, lancedb.c.
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

#endif /* HU_MEMORY_SQL_COMMON_H */
