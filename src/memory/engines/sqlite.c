#ifdef SC_ENABLE_SQLITE

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory.h"
#include "seaclaw/platform.h"
#include <math.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "seaclaw/memory/sql_common.h"

#define SC_SQLITE_BUSY_TIMEOUT_MS 5000

typedef struct sc_sqlite_memory {
    sqlite3 *db;
    sc_allocator_t *alloc;
} sc_sqlite_memory_t;

static const char *const schema_parts[] = {
    "CREATE TABLE IF NOT EXISTS memories("
    "id TEXT PRIMARY KEY,key TEXT NOT NULL UNIQUE,"
    "content TEXT NOT NULL,category TEXT NOT NULL DEFAULT'core',"
    "session_id TEXT,created_at TEXT NOT NULL,updated_at TEXT NOT NULL)",
    "CREATE INDEX IF NOT EXISTS idx_memories_category ON memories(category)",
    "CREATE INDEX IF NOT EXISTS idx_memories_key ON memories(key)",
    "CREATE INDEX IF NOT EXISTS idx_memories_session ON memories(session_id)",
    "CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts USING fts5("
    "key,content,content=memories,content_rowid=rowid)",
    "CREATE TRIGGER IF NOT EXISTS memories_ai AFTER INSERT ON memories BEGIN "
    "INSERT INTO memories_fts(rowid,key,content)VALUES(new.rowid,new.key,new.content);END",
    "CREATE TRIGGER IF NOT EXISTS memories_ad AFTER DELETE ON memories BEGIN "
    "INSERT INTO memories_fts(memories_fts,rowid,key,content)"
    "VALUES('delete',old.rowid,old.key,old.content);END",
    "CREATE TRIGGER IF NOT EXISTS memories_au AFTER UPDATE ON memories BEGIN "
    "INSERT INTO memories_fts(memories_fts,rowid,key,content)"
    "VALUES('delete',old.rowid,old.key,old.content);"
    "INSERT INTO memories_fts(rowid,key,content)"
    "VALUES(new.rowid,new.key,new.content);END",
    "CREATE TABLE IF NOT EXISTS messages("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "session_id TEXT NOT NULL,role TEXT NOT NULL,"
    "content TEXT NOT NULL,created_at TEXT DEFAULT(datetime('now')))",
    "CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id)",
    "CREATE TABLE IF NOT EXISTS kv(key TEXT PRIMARY KEY,value TEXT NOT NULL)",
    NULL};

static void get_timestamp(char *buf, size_t buf_size) {
    time_t t = time(NULL);
    struct tm tm_buf;
    struct tm *tm = sc_platform_gmtime_r(&t, &tm_buf);
    if (tm) {
        strftime(buf, buf_size, "%Y-%m-%dT%H:%M:%SZ", tm);
    } else {
        snprintf(buf, buf_size, "%ld", (long)t);
    }
}

static char *generate_id(sc_allocator_t *alloc) {
    static unsigned long counter = 0;
    char ts[32];
    get_timestamp(ts, sizeof(ts));
    return sc_sprintf(alloc, "mem_%ld_%lu_%s", (long)time(NULL), ++counter, ts);
}

static const char *category_to_string(const sc_memory_category_t *cat) {
    if (!cat)
        return "core";
    switch (cat->tag) {
    case SC_MEMORY_CATEGORY_CORE:
        return "core";
    case SC_MEMORY_CATEGORY_DAILY:
        return "daily";
    case SC_MEMORY_CATEGORY_CONVERSATION:
        return "conversation";
    case SC_MEMORY_CATEGORY_CUSTOM:
        if (cat->data.custom.name && cat->data.custom.name_len > 0)
            return cat->data.custom.name;
        return "custom";
    default:
        return "core";
    }
}

static sc_error_t read_entry_from_row(sqlite3_stmt *stmt, sc_allocator_t *alloc,
                                      sc_memory_entry_t *out) {
    const char *id_p = (const char *)sqlite3_column_text(stmt, 0);
    const char *key_p = (const char *)sqlite3_column_text(stmt, 1);
    const char *content_p = (const char *)sqlite3_column_text(stmt, 2);
    const char *category_p = (const char *)sqlite3_column_text(stmt, 3);
    const char *timestamp_p = (const char *)sqlite3_column_text(stmt, 4);
    const char *session_id_p = (const char *)sqlite3_column_text(stmt, 5);

    size_t id_len = id_p ? (size_t)sqlite3_column_bytes(stmt, 0) : 0;
    size_t key_len = key_p ? (size_t)sqlite3_column_bytes(stmt, 1) : 0;
    size_t content_len = content_p ? (size_t)sqlite3_column_bytes(stmt, 2) : 0;
    size_t timestamp_len = timestamp_p ? (size_t)sqlite3_column_bytes(stmt, 4) : 0;
    size_t session_id_len = session_id_p ? (size_t)sqlite3_column_bytes(stmt, 5) : 0;

    out->id = id_p ? sc_strndup(alloc, id_p, id_len) : NULL;
    out->id_len = id_len;
    out->key = key_p ? sc_strndup(alloc, key_p, key_len) : NULL;
    out->key_len = key_len;
    out->content = content_p ? sc_strndup(alloc, content_p, content_len) : NULL;
    out->content_len = content_len;
    out->category.tag = SC_MEMORY_CATEGORY_CUSTOM;
    out->category.data.custom.name =
        category_p
            ? sc_strndup(alloc, category_p, category_p ? (size_t)sqlite3_column_bytes(stmt, 3) : 0)
            : NULL;
    out->category.data.custom.name_len = category_p ? (size_t)sqlite3_column_bytes(stmt, 3) : 0;
    out->timestamp = timestamp_p ? sc_strndup(alloc, timestamp_p, timestamp_len) : NULL;
    out->timestamp_len = timestamp_len;
    out->session_id = session_id_p ? sc_strndup(alloc, session_id_p, session_id_len) : NULL;
    out->session_id_len = session_id_len;

    if (sqlite3_column_count(stmt) > 6) {
        out->score = sqlite3_column_double(stmt, 6);
    } else {
        out->score = NAN;
    }
    return SC_OK;
}

static void free_entry(sc_allocator_t *alloc, sc_memory_entry_t *e) {
    sc_memory_entry_free_fields(alloc, e);
}

static const char *impl_name(void *ctx) {
    (void)ctx;
    return "sqlite";
}

static sc_error_t impl_store(void *ctx, const char *key, size_t key_len, const char *content,
                             size_t content_len, const sc_memory_category_t *category,
                             const char *session_id, size_t session_id_len) {
    sc_sqlite_memory_t *self = (sc_sqlite_memory_t *)ctx;
    char ts[64];
    get_timestamp(ts, sizeof(ts));

    char *id = generate_id(self->alloc);
    if (!id)
        return SC_ERR_OUT_OF_MEMORY;

    const char *cat_str = category_to_string(category);
    const char *sql =
        "INSERT INTO memories (id, key, content, category, session_id, created_at, updated_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) "
        "ON CONFLICT(key) DO UPDATE SET "
        "content = excluded.content, category = excluded.category, "
        "session_id = excluded.session_id, updated_at = excluded.updated_at";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sc_str_free(self->alloc, id);
        return SC_ERR_MEMORY_STORE;
    }

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, key, (int)key_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, content, (int)content_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, cat_str, -1, SQLITE_STATIC);
    if (session_id && session_id_len > 0)
        sqlite3_bind_text(stmt, 5, session_id, (int)session_id_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 5);
    sqlite3_bind_text(stmt, 6, ts, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, ts, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sc_str_free(self->alloc, id);

    if (rc != SQLITE_DONE)
        return SC_ERR_MEMORY_STORE;
    return SC_OK;
}

static sc_error_t impl_recall(void *ctx, sc_allocator_t *alloc, const char *query, size_t query_len,
                              size_t limit, const char *session_id, size_t session_id_len,
                              sc_memory_entry_t **out, size_t *out_count) {
    sc_sqlite_memory_t *self = (sc_sqlite_memory_t *)ctx;
    *out = NULL;
    *out_count = 0;

    if (!query || query_len == 0) {
        *out = NULL;
        return SC_OK;
    }

    /* FTS5 BM25 search - build query from words */
    char fts_buf[512];
    size_t fts_len = 0;
    const char *p = query;
    const char *end = query + query_len;
    bool first = true;
    while (p < end && fts_len < sizeof(fts_buf) - 10) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
        if (p >= end)
            break;
        const char *word_start = p;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
            p++;
        if (p > word_start) {
            /* Escape double quotes in words for FTS5 MATCH query */
            char escaped_word[256];
            size_t ew_len = 0;
            for (const char *c = word_start; c < p && ew_len < sizeof(escaped_word) - 2; c++) {
                if (*c == '"') {
                    if (ew_len < sizeof(escaped_word) - 3) {
                        escaped_word[ew_len++] = '"';
                        escaped_word[ew_len++] = '"';
                    }
                } else {
                    escaped_word[ew_len++] = *c;
                }
            }
            escaped_word[ew_len] = '\0';
            if (!first) {
                fts_len += (size_t)snprintf(fts_buf + fts_len, sizeof(fts_buf) - fts_len, " OR ");
            }
            fts_len += (size_t)snprintf(fts_buf + fts_len, sizeof(fts_buf) - fts_len, "\"%s\"",
                                        escaped_word);
            first = false;
        }
    }

    /* Try FTS5 first; fall back to LIKE when FTS returns no rows */
    if (fts_len > 0) {
        const char *sql = "SELECT m.id, m.key, m.content, m.category, m.created_at, m.session_id, "
                          "bm25(memories_fts) as score FROM memories_fts f "
                          "JOIN memories m ON m.rowid = f.rowid "
                          "WHERE memories_fts MATCH ?1 "
                          "ORDER BY score LIMIT ?2";
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, fts_buf, (int)fts_len, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 2, (sqlite3_int64)limit);

            sc_memory_entry_t *entries =
                (sc_memory_entry_t *)alloc->alloc(alloc->ctx, limit * sizeof(sc_memory_entry_t));
            if (!entries) {
                sqlite3_finalize(stmt);
                return SC_ERR_OUT_OF_MEMORY;
            }
            size_t count = 0;

            while (sqlite3_step(stmt) == SQLITE_ROW && count < limit) {
                sc_memory_entry_t *e = &entries[count];
                read_entry_from_row(stmt, alloc, e);
                if (session_id && session_id_len > 0 && e->session_id &&
                    (e->session_id_len != session_id_len ||
                     memcmp(e->session_id, session_id, session_id_len) != 0)) {
                    free_entry(alloc, e);
                    continue;
                }
                count++;
            }
            sqlite3_finalize(stmt);
            if (count > 0) {
                *out = entries;
                *out_count = count;
                return SC_OK;
            }
            alloc->free(alloc->ctx, entries, limit * sizeof(sc_memory_entry_t));
        } else {
            if (stmt)
                sqlite3_finalize(stmt);
        }
    }

    /* Fallback: LIKE search */
    char *like_pattern = (char *)alloc->alloc(alloc->ctx, query_len + 3);
    if (!like_pattern)
        return SC_ERR_OUT_OF_MEMORY;
    like_pattern[0] = '%';
    memcpy(like_pattern + 1, query, query_len);
    like_pattern[query_len + 1] = '%';
    like_pattern[query_len + 2] = '\0';

    const char *sql =
        "SELECT id, key, content, category, created_at, session_id "
        "FROM memories WHERE content LIKE ?1 OR key LIKE ?1 ORDER BY updated_at DESC LIMIT ?2";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        alloc->free(alloc->ctx, like_pattern, query_len + 3);
        return SC_ERR_MEMORY_RECALL;
    }

    sqlite3_bind_text(stmt, 1, like_pattern, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)limit);

    sc_memory_entry_t *entries =
        (sc_memory_entry_t *)alloc->alloc(alloc->ctx, limit * sizeof(sc_memory_entry_t));
    if (!entries) {
        sqlite3_finalize(stmt);
        alloc->free(alloc->ctx, like_pattern, query_len + 3);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < limit) {
        read_entry_from_row(stmt, alloc, &entries[count]);
        if (session_id && session_id_len > 0 && entries[count].session_id &&
            (entries[count].session_id_len != session_id_len ||
             memcmp(entries[count].session_id, session_id, session_id_len) != 0)) {
            free_entry(alloc, &entries[count]);
            continue;
        }
        count++;
    }
    sqlite3_finalize(stmt);
    alloc->free(alloc->ctx, like_pattern, query_len + 3);
    *out = entries;
    *out_count = count;
    return SC_OK;
}

static sc_error_t impl_get(void *ctx, sc_allocator_t *alloc, const char *key, size_t key_len,
                           sc_memory_entry_t *out, bool *found) {
    sc_sqlite_memory_t *self = (sc_sqlite_memory_t *)ctx;
    *found = false;

    const char *sql = "SELECT id, key, content, category, created_at, session_id "
                      "FROM memories WHERE key = ?1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return SC_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, key, (int)key_len, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        read_entry_from_row(stmt, alloc, out);
        *found = true;
    }
    sqlite3_finalize(stmt);
    return SC_OK;
}

static sc_error_t impl_list(void *ctx, sc_allocator_t *alloc, const sc_memory_category_t *category,
                            const char *session_id, size_t session_id_len, sc_memory_entry_t **out,
                            size_t *out_count) {
    sc_sqlite_memory_t *self = (sc_sqlite_memory_t *)ctx;
    const char *sql;
    if (category) {
        sql = "SELECT id, key, content, category, created_at, session_id "
              "FROM memories WHERE category = ?1 ORDER BY updated_at DESC";
    } else {
        sql = "SELECT id, key, content, category, created_at, session_id "
              "FROM memories ORDER BY updated_at DESC";
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return SC_ERR_MEMORY_BACKEND;

    if (category)
        sqlite3_bind_text(stmt, 1, category_to_string(category), -1, SQLITE_STATIC);

    size_t cap = 64;
    sc_memory_entry_t *entries =
        (sc_memory_entry_t *)alloc->alloc(alloc->ctx, cap * sizeof(sc_memory_entry_t));
    if (!entries) {
        sqlite3_finalize(stmt);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            sc_memory_entry_t *n = (sc_memory_entry_t *)alloc->realloc(
                alloc->ctx, entries, cap * sizeof(sc_memory_entry_t),
                (cap * 2) * sizeof(sc_memory_entry_t));
            if (!n)
                break;
            entries = n;
            cap *= 2;
        }
        read_entry_from_row(stmt, alloc, &entries[count]);
        if (session_id && session_id_len > 0 && entries[count].session_id &&
            (entries[count].session_id_len != session_id_len ||
             memcmp(entries[count].session_id, session_id, session_id_len) != 0)) {
            free_entry(alloc, &entries[count]);
            continue;
        }
        count++;
    }
    sqlite3_finalize(stmt);
    *out = entries;
    *out_count = count;
    return SC_OK;
}

static sc_error_t impl_forget(void *ctx, const char *key, size_t key_len, bool *deleted) {
    sc_sqlite_memory_t *self = (sc_sqlite_memory_t *)ctx;
    const char *sql = "DELETE FROM memories WHERE key = ?1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return SC_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(stmt, 1, key, (int)key_len, SQLITE_STATIC);
    sqlite3_step(stmt);
    *deleted = sqlite3_changes(self->db) > 0;
    sqlite3_finalize(stmt);
    return SC_OK;
}

static sc_error_t impl_count(void *ctx, size_t *out) {
    sc_sqlite_memory_t *self = (sc_sqlite_memory_t *)ctx;
    const char *sql = "SELECT COUNT(*) FROM memories";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return SC_ERR_MEMORY_BACKEND;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        *out = (size_t)sqlite3_column_int64(stmt, 0);
    else
        *out = 0;
    sqlite3_finalize(stmt);
    return SC_OK;
}

static bool impl_health_check(void *ctx) {
    sc_sqlite_memory_t *self = (sc_sqlite_memory_t *)ctx;
    char *err = NULL;
    int rc = sqlite3_exec(self->db, "SELECT 1", NULL, NULL, &err);
    if (err)
        sqlite3_free(err);
    return rc == SQLITE_OK;
}

static void impl_deinit(void *ctx) {
    sc_sqlite_memory_t *self = (sc_sqlite_memory_t *)ctx;
    if (self->db)
        sqlite3_close(self->db);
    self->alloc->free(self->alloc->ctx, self, sizeof(sc_sqlite_memory_t));
}

static const sc_memory_vtable_t sqlite_vtable = {
    .name = impl_name,
    .store = impl_store,
    .recall = impl_recall,
    .get = impl_get,
    .list = impl_list,
    .forget = impl_forget,
    .count = impl_count,
    .health_check = impl_health_check,
    .deinit = impl_deinit,
};

/* Session store implementation */
static sc_error_t impl_session_save_message(void *ctx, const char *session_id,
                                            size_t session_id_len, const char *role,
                                            size_t role_len, const char *content,
                                            size_t content_len) {
    sc_sqlite_memory_t *self = (sc_sqlite_memory_t *)ctx;
    const char *sql = "INSERT INTO messages (session_id, role, content) VALUES (?1, ?2, ?3)";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return SC_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, session_id, (int)session_id_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, role, (int)role_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, content, (int)content_len, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? SC_OK : SC_ERR_MEMORY_STORE;
}

static sc_error_t impl_session_load_messages(void *ctx, sc_allocator_t *alloc,
                                             const char *session_id, size_t session_id_len,
                                             sc_message_entry_t **out, size_t *out_count) {
    sc_sqlite_memory_t *self = (sc_sqlite_memory_t *)ctx;
    const char *sql = "SELECT role, content FROM messages WHERE session_id = ?1 ORDER BY id ASC";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return SC_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(stmt, 1, session_id, (int)session_id_len, SQLITE_STATIC);

    size_t cap = 32;
    sc_message_entry_t *entries =
        (sc_message_entry_t *)alloc->alloc(alloc->ctx, cap * sizeof(sc_message_entry_t));
    if (!entries) {
        sqlite3_finalize(stmt);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            sc_message_entry_t *n = (sc_message_entry_t *)alloc->realloc(
                alloc->ctx, entries, cap * sizeof(sc_message_entry_t),
                (cap * 2) * sizeof(sc_message_entry_t));
            if (!n)
                break;
            entries = n;
            cap *= 2;
        }
        const char *role_p = (const char *)sqlite3_column_text(stmt, 0);
        const char *content_p = (const char *)sqlite3_column_text(stmt, 1);
        size_t rl = role_p ? (size_t)sqlite3_column_bytes(stmt, 0) : 0;
        size_t cl = content_p ? (size_t)sqlite3_column_bytes(stmt, 1) : 0;
        entries[count].role = role_p ? sc_strndup(alloc, role_p, rl) : NULL;
        entries[count].role_len = rl;
        entries[count].content = content_p ? sc_strndup(alloc, content_p, cl) : NULL;
        entries[count].content_len = cl;
        count++;
    }
    sqlite3_finalize(stmt);
    *out = entries;
    *out_count = count;
    return SC_OK;
}

static sc_error_t impl_session_clear_messages(void *ctx, const char *session_id,
                                              size_t session_id_len) {
    sc_sqlite_memory_t *self = (sc_sqlite_memory_t *)ctx;
    const char *sql = "DELETE FROM messages WHERE session_id = ?1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return SC_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(stmt, 1, session_id, (int)session_id_len, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return SC_OK;
}

static sc_error_t impl_session_clear_auto_saved(void *ctx, const char *session_id,
                                                size_t session_id_len) {
    sc_sqlite_memory_t *self = (sc_sqlite_memory_t *)ctx;
    const char *sql;
    if (session_id && session_id_len > 0)
        sql = "DELETE FROM memories WHERE key LIKE 'autosave_%' AND session_id = ?1";
    else
        sql = "DELETE FROM memories WHERE key LIKE 'autosave_%'";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return SC_ERR_MEMORY_BACKEND;
    if (session_id && session_id_len > 0)
        sqlite3_bind_text(stmt, 1, session_id, (int)session_id_len, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return SC_OK;
}

static const sc_session_store_vtable_t sqlite_session_vtable = {
    .save_message = impl_session_save_message,
    .load_messages = impl_session_load_messages,
    .clear_messages = impl_session_clear_messages,
    .clear_auto_saved = impl_session_clear_auto_saved,
};

sc_memory_t sc_sqlite_memory_create(sc_allocator_t *alloc, const char *db_path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(db_path ? db_path : ":memory:", &db);
    if (rc != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return (sc_memory_t){.ctx = NULL, .vtable = NULL};
    }
    sqlite3_busy_timeout(db, SC_SQLITE_BUSY_TIMEOUT_MS);
    sqlite3_exec(db, SC_SQL_PRAGMA_INIT, NULL, NULL, NULL);

    for (const char *const *part = schema_parts; *part; part++) {
        char *err = NULL;
        rc = sqlite3_exec(db, *part, NULL, NULL, &err);
        if (rc != SQLITE_OK) {
            if (err)
                sqlite3_free(err);
            sqlite3_close(db);
            return (sc_memory_t){.ctx = NULL, .vtable = NULL};
        }
    }

    sc_sqlite_memory_t *self =
        (sc_sqlite_memory_t *)alloc->alloc(alloc->ctx, sizeof(sc_sqlite_memory_t));
    if (!self) {
        sqlite3_close(db);
        return (sc_memory_t){.ctx = NULL, .vtable = NULL};
    }
    self->db = db;
    self->alloc = alloc;
    return (sc_memory_t){
        .ctx = self,
        .vtable = &sqlite_vtable,
    };
}

sc_session_store_t sc_sqlite_memory_get_session_store(sc_memory_t *mem) {
    if (!mem || !mem->ctx || !mem->vtable)
        return (sc_session_store_t){.ctx = NULL, .vtable = NULL};
    const char *n = mem->vtable->name(mem->ctx);
    if (!n || strcmp(n, "sqlite") != 0)
        return (sc_session_store_t){.ctx = NULL, .vtable = NULL};
    return (sc_session_store_t){
        .ctx = mem->ctx,
        .vtable = &sqlite_session_vtable,
    };
}

#else /* !SC_ENABLE_SQLITE */

#include "seaclaw/core/allocator.h"
#include "seaclaw/memory.h"

sc_memory_t sc_sqlite_memory_create(sc_allocator_t *alloc, const char *db_path) {
    (void)db_path;
    return sc_none_memory_create(alloc);
}

sc_session_store_t sc_sqlite_memory_get_session_store(sc_memory_t *mem) {
    (void)mem;
    return (sc_session_store_t){.ctx = NULL, .vtable = NULL};
}

#endif
