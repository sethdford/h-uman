/* LanceDB memory backend — SQLite + optional vector. Text-based search without embeddings.
 * SC_IS_TEST: in-memory mock.
 * When SC_ENABLE_SQLITE is not set (production build), all operations return
 * SC_ERR_NOT_SUPPORTED. This is intentional, documented stub behavior. */

#include "seaclaw/memory/engines.h"
#include "seaclaw/memory.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

#if defined(SC_IS_TEST) && SC_IS_TEST
/* ── Mock implementation (SC_IS_TEST) ───────────────────────────────────── */

#define MOCK_MAX_ENTRIES 32

typedef struct mock_entry {
    char *key;
    char *content;
    char *category;
    char *session_id;
    char *timestamp;
} mock_entry_t;

typedef struct sc_lancedb_memory {
    sc_allocator_t *alloc;
    mock_entry_t entries[MOCK_MAX_ENTRIES];
    size_t count;
} sc_lancedb_memory_t;

static void mock_free_entry(sc_lancedb_memory_t *self, mock_entry_t *e) {
    if (!self->alloc || !e) return;
    if (e->key) { self->alloc->free(self->alloc->ctx, e->key, strlen(e->key) + 1); e->key = NULL; }
    if (e->content) { self->alloc->free(self->alloc->ctx, e->content, strlen(e->content) + 1); e->content = NULL; }
    if (e->category) { self->alloc->free(self->alloc->ctx, e->category, strlen(e->category) + 1); e->category = NULL; }
    if (e->session_id) { self->alloc->free(self->alloc->ctx, e->session_id, strlen(e->session_id) + 1); e->session_id = NULL; }
    if (e->timestamp) { self->alloc->free(self->alloc->ctx, e->timestamp, strlen(e->timestamp) + 1); e->timestamp = NULL; }
}

static mock_entry_t *mock_find(sc_lancedb_memory_t *self, const char *key, size_t key_len) {
    for (size_t i = 0; i < self->count; i++) {
        mock_entry_t *e = &self->entries[i];
        if (e->key && strlen(e->key) == key_len && memcmp(e->key, key, key_len) == 0)
            return e;
    }
    return NULL;
}

static int mock_contains(const char *haystack, size_t hlen, const char *needle, size_t nlen) {
    if (nlen == 0) return 1;
    if (hlen < nlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (memcmp(haystack + i, needle, nlen) == 0) return 1;
    }
    return 0;
}

static const char *category_to_string(const sc_memory_category_t *cat) {
    if (!cat) return "core";
    switch (cat->tag) {
        case SC_MEMORY_CATEGORY_CORE: return "core";
        case SC_MEMORY_CATEGORY_DAILY: return "daily";
        case SC_MEMORY_CATEGORY_CONVERSATION: return "conversation";
        case SC_MEMORY_CATEGORY_CUSTOM:
            if (cat->data.custom.name && cat->data.custom.name_len > 0)
                return cat->data.custom.name;
            return "custom";
        default: return "core";
    }
}

static void fill_entry_from_mock(sc_allocator_t *alloc, const mock_entry_t *m,
    sc_memory_entry_t *out) {
    out->id = m->key ? sc_strndup(alloc, m->key, strlen(m->key)) : NULL;
    out->id_len = m->key ? strlen(m->key) : 0;
    out->key = m->key ? sc_strndup(alloc, m->key, strlen(m->key)) : NULL;
    out->key_len = m->key ? strlen(m->key) : 0;
    out->content = m->content ? sc_strndup(alloc, m->content, strlen(m->content)) : NULL;
    out->content_len = m->content ? strlen(m->content) : 0;
    out->category.tag = SC_MEMORY_CATEGORY_CUSTOM;
    out->category.data.custom.name = m->category ? sc_strndup(alloc, m->category, strlen(m->category)) : NULL;
    out->category.data.custom.name_len = m->category ? strlen(m->category) : 0;
    out->timestamp = m->timestamp ? sc_strndup(alloc, m->timestamp, strlen(m->timestamp)) : NULL;
    out->timestamp_len = m->timestamp ? strlen(m->timestamp) : 0;
    out->session_id = m->session_id ? sc_strndup(alloc, m->session_id, strlen(m->session_id)) : NULL;
    out->session_id_len = m->session_id ? strlen(m->session_id) : 0;
    out->score = NAN;
}

static const char *impl_name(void *ctx) { (void)ctx; return "lancedb"; }

static sc_error_t impl_store(void *ctx,
    const char *key, size_t key_len,
    const char *content, size_t content_len,
    const sc_memory_category_t *category,
    const char *session_id, size_t session_id_len) {
    sc_lancedb_memory_t *self = (sc_lancedb_memory_t *)ctx;
    mock_entry_t *e = mock_find(self, key, key_len);
    if (e) mock_free_entry(self, e);
    else {
        if (self->count >= MOCK_MAX_ENTRIES) return SC_ERR_OUT_OF_MEMORY;
        e = &self->entries[self->count++];
    }
    e->key = sc_strndup(self->alloc, key, key_len);
    if (!e->key) return SC_ERR_OUT_OF_MEMORY;
    e->content = sc_strndup(self->alloc, content, content_len);
    if (!e->content) { self->alloc->free(self->alloc->ctx, e->key, key_len + 1); e->key = NULL; return SC_ERR_OUT_OF_MEMORY; }
    {
        const char *cat_str = category_to_string(category);
        e->category = sc_strndup(self->alloc, cat_str, strlen(cat_str));
        if (!e->category) {
            self->alloc->free(self->alloc->ctx, e->content, content_len + 1);
            self->alloc->free(self->alloc->ctx, e->key, key_len + 1);
            e->key = e->content = NULL;
            return SC_ERR_OUT_OF_MEMORY;
        }
    }
    e->session_id = (session_id && session_id_len > 0)
        ? sc_strndup(self->alloc, session_id, session_id_len) : NULL;
    {
        char ts[32];
        time_t t = time(NULL);
        struct tm *tm = gmtime(&t);
        if (tm)
            snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                tm->tm_hour, tm->tm_min, tm->tm_sec);
        else
            snprintf(ts, sizeof(ts), "%ld", (long)t);
        e->timestamp = sc_strndup(self->alloc, ts, strlen(ts));
    }
    return SC_OK;
}

static sc_error_t impl_recall(void *ctx, sc_allocator_t *alloc,
    const char *query, size_t query_len, size_t limit,
    const char *session_id, size_t session_id_len,
    sc_memory_entry_t **out, size_t *out_count) {
    sc_lancedb_memory_t *self = (sc_lancedb_memory_t *)ctx;
    *out = NULL;
    *out_count = 0;
    if (!query || query_len == 0) return SC_OK;
    sc_memory_entry_t *entries = (sc_memory_entry_t *)alloc->alloc(alloc->ctx,
        limit * sizeof(sc_memory_entry_t));
    if (!entries) return SC_ERR_OUT_OF_MEMORY;
    size_t count = 0;
    for (size_t i = 0; i < self->count && count < limit; i++) {
        mock_entry_t *m = &self->entries[i];
        if (!m->key || !m->content) continue;
        size_t clen = strlen(m->content), klen = strlen(m->key);
        if (!mock_contains(m->content, clen, query, query_len) &&
            !mock_contains(m->key, klen, query, query_len))
            continue;
        if (session_id && session_id_len > 0) {
            if (!m->session_id || strlen(m->session_id) != session_id_len ||
                memcmp(m->session_id, session_id, session_id_len) != 0)
                continue;
        }
        fill_entry_from_mock(alloc, m, &entries[count++]);
    }
    *out = entries;
    *out_count = count;
    return SC_OK;
}

static sc_error_t impl_get(void *ctx, sc_allocator_t *alloc,
    const char *key, size_t key_len, sc_memory_entry_t *out, bool *found) {
    sc_lancedb_memory_t *self = (sc_lancedb_memory_t *)ctx;
    *found = false;
    mock_entry_t *m = mock_find(self, key, key_len);
    if (!m) return SC_OK;
    fill_entry_from_mock(alloc, m, out);
    *found = true;
    return SC_OK;
}

static sc_error_t impl_list(void *ctx, sc_allocator_t *alloc,
    const sc_memory_category_t *category,
    const char *session_id, size_t session_id_len,
    sc_memory_entry_t **out, size_t *out_count) {
    sc_lancedb_memory_t *self = (sc_lancedb_memory_t *)ctx;
    *out = NULL;
    *out_count = 0;
    const char *cat_str = category ? category_to_string(category) : NULL;
    size_t cap = 64;
    sc_memory_entry_t *entries = (sc_memory_entry_t *)alloc->alloc(alloc->ctx,
        cap * sizeof(sc_memory_entry_t));
    if (!entries) return SC_ERR_OUT_OF_MEMORY;
    size_t count = 0;
    for (size_t i = 0; i < self->count; i++) {
        mock_entry_t *m = &self->entries[i];
        if (!m->key || !m->content) continue;
        if (cat_str && (!m->category || strcmp(m->category, cat_str) != 0)) continue;
        if (session_id && session_id_len > 0) {
            if (!m->session_id || strlen(m->session_id) != session_id_len ||
                memcmp(m->session_id, session_id, session_id_len) != 0)
                continue;
        }
        if (count >= cap) {
            sc_memory_entry_t *n = (sc_memory_entry_t *)alloc->realloc(alloc->ctx, entries,
                cap * sizeof(sc_memory_entry_t), (cap * 2) * sizeof(sc_memory_entry_t));
            if (!n) break;
            entries = n;
            cap *= 2;
        }
        fill_entry_from_mock(alloc, m, &entries[count++]);
    }
    *out = entries;
    *out_count = count;
    return SC_OK;
}

static sc_error_t impl_forget(void *ctx, const char *key, size_t key_len, bool *deleted) {
    sc_lancedb_memory_t *self = (sc_lancedb_memory_t *)ctx;
    *deleted = false;
    for (size_t i = 0; i < self->count; i++) {
        mock_entry_t *e = &self->entries[i];
        if (e->key && strlen(e->key) == key_len && memcmp(e->key, key, key_len) == 0) {
            mock_free_entry(self, e);
            memmove(&self->entries[i], &self->entries[i + 1],
                (self->count - 1 - i) * sizeof(mock_entry_t));
            memset(&self->entries[self->count - 1], 0, sizeof(mock_entry_t));
            self->count--;
            *deleted = true;
            return SC_OK;
        }
    }
    return SC_OK;
}

static sc_error_t impl_count(void *ctx, size_t *out) {
    sc_lancedb_memory_t *self = (sc_lancedb_memory_t *)ctx;
    *out = self->count;
    return SC_OK;
}

static bool impl_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static void impl_deinit(void *ctx) {
    sc_lancedb_memory_t *self = (sc_lancedb_memory_t *)ctx;
    if (!self) return;
    for (size_t i = 0; i < self->count; i++)
        mock_free_entry(self, &self->entries[i]);
    self->count = 0;
    if (self->alloc)
        self->alloc->free(self->alloc->ctx, self, sizeof(sc_lancedb_memory_t));
}

static const sc_memory_vtable_t lancedb_vtable = {
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

sc_memory_t sc_lancedb_memory_create(sc_allocator_t *alloc, const char *db_path) {
    (void)db_path;
    if (!alloc) return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    sc_lancedb_memory_t *self = (sc_lancedb_memory_t *)alloc->alloc(
        alloc->ctx, sizeof(sc_lancedb_memory_t));
    if (!self) return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    memset(self, 0, sizeof(sc_lancedb_memory_t));
    self->alloc = alloc;
    return (sc_memory_t){ .ctx = self, .vtable = &lancedb_vtable };
}

#else /* !SC_IS_TEST — production: SQLite or stub */

#ifdef SC_ENABLE_SQLITE

#include <sqlite3.h>

typedef struct sc_lancedb_memory_prod {
    sc_allocator_t *alloc;
    sqlite3 *db;
} sc_lancedb_memory_t;

#define SC_SQLITE_BUSY_TIMEOUT_MS 5000

static const char *schema_sql =
    "CREATE TABLE IF NOT EXISTS lancedb_memories ("
    "  key        TEXT PRIMARY KEY,"
    "  text       TEXT NOT NULL,"
    "  category   TEXT NOT NULL DEFAULT 'core',"
    "  session_id TEXT,"
    "  created_at TEXT NOT NULL,"
    "  updated_at TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_lancedb_category ON lancedb_memories(category);"
    "CREATE INDEX IF NOT EXISTS idx_lancedb_session ON lancedb_memories(session_id);";

static void get_timestamp(char *buf, size_t buf_size) {
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    if (tm)
        strftime(buf, buf_size, "%Y-%m-%dT%H:%M:%SZ", tm);
    else
        snprintf(buf, buf_size, "%ld", (long)t);
}

static const char *category_to_string(const sc_memory_category_t *cat) {
    if (!cat) return "core";
    switch (cat->tag) {
        case SC_MEMORY_CATEGORY_CORE: return "core";
        case SC_MEMORY_CATEGORY_DAILY: return "daily";
        case SC_MEMORY_CATEGORY_CONVERSATION: return "conversation";
        case SC_MEMORY_CATEGORY_CUSTOM:
            if (cat->data.custom.name && cat->data.custom.name_len > 0)
                return cat->data.custom.name;
            return "custom";
        default: return "core";
    }
}

static sc_error_t read_entry_from_row(sqlite3_stmt *stmt, sc_allocator_t *alloc,
    sc_memory_entry_t *out) {
    const char *key_p = (const char *)sqlite3_column_text(stmt, 0);
    const char *text_p = (const char *)sqlite3_column_text(stmt, 1);
    const char *category_p = (const char *)sqlite3_column_text(stmt, 2);
    const char *session_id_p = (const char *)sqlite3_column_text(stmt, 3);
    const char *timestamp_p = (const char *)sqlite3_column_text(stmt, 4);
    size_t key_len = key_p ? (size_t)sqlite3_column_bytes(stmt, 0) : 0;
    size_t text_len = text_p ? (size_t)sqlite3_column_bytes(stmt, 1) : 0;
    size_t cat_len = category_p ? (size_t)sqlite3_column_bytes(stmt, 2) : 0;
    size_t session_id_len = session_id_p ? (size_t)sqlite3_column_bytes(stmt, 3) : 0;
    size_t timestamp_len = timestamp_p ? (size_t)sqlite3_column_bytes(stmt, 4) : 0;
    out->id = key_p ? sc_strndup(alloc, key_p, key_len) : NULL;
    out->id_len = key_len;
    out->key = key_p ? sc_strndup(alloc, key_p, key_len) : NULL;
    out->key_len = key_len;
    out->content = text_p ? sc_strndup(alloc, text_p, text_len) : NULL;
    out->content_len = text_len;
    out->category.tag = SC_MEMORY_CATEGORY_CUSTOM;
    out->category.data.custom.name = category_p ? sc_strndup(alloc, category_p, cat_len) : NULL;
    out->category.data.custom.name_len = cat_len;
    out->timestamp = timestamp_p ? sc_strndup(alloc, timestamp_p, timestamp_len) : NULL;
    out->timestamp_len = timestamp_len;
    out->session_id = session_id_p ? sc_strndup(alloc, session_id_p, session_id_len) : NULL;
    out->session_id_len = session_id_len;
    out->score = NAN;
    return SC_OK;
}

static const char *impl_name_prod(void *ctx) { (void)ctx; return "lancedb"; }

static sc_error_t impl_store_prod(void *ctx,
    const char *key, size_t key_len,
    const char *content, size_t content_len,
    const sc_memory_category_t *category,
    const char *session_id, size_t session_id_len) {
    sc_lancedb_memory_t *self = (sc_lancedb_memory_t *)ctx;
    char ts[64];
    get_timestamp(ts, sizeof(ts));
    const char *cat_str = category_to_string(category);
    const char *sql = "INSERT OR REPLACE INTO lancedb_memories "
        "(key, text, category, session_id, created_at, updated_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6)";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return SC_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, key, (int)key_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, content, (int)content_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, cat_str, -1, SQLITE_STATIC);
    if (session_id && session_id_len > 0)
        sqlite3_bind_text(stmt, 4, session_id, (int)session_id_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 4);
    sqlite3_bind_text(stmt, 5, ts, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, ts, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return SC_ERR_MEMORY_STORE;
    return SC_OK;
}

static sc_error_t impl_recall_prod(void *ctx, sc_allocator_t *alloc,
    const char *query, size_t query_len, size_t limit,
    const char *session_id, size_t session_id_len,
    sc_memory_entry_t **out, size_t *out_count) {
    sc_lancedb_memory_t *self = (sc_lancedb_memory_t *)ctx;
    (void)self;
    *out = NULL;
    *out_count = 0;
    if (!query || query_len == 0) return SC_OK;
    char *like_pattern = (char *)alloc->alloc(alloc->ctx, query_len + 3);
    if (!like_pattern) return SC_ERR_OUT_OF_MEMORY;
    like_pattern[0] = '%';
    memcpy(like_pattern + 1, query, query_len);
    like_pattern[query_len + 1] = '%';
    like_pattern[query_len + 2] = '\0';
    const char *sql = "SELECT key, text, category, session_id, updated_at "
        "FROM lancedb_memories WHERE text LIKE ?1 OR key LIKE ?1 ORDER BY updated_at DESC LIMIT ?2";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        alloc->free(alloc->ctx, like_pattern, query_len + 3);
        return SC_ERR_MEMORY_RECALL;
    }
    sqlite3_bind_text(stmt, 1, like_pattern, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)limit);
    sc_memory_entry_t *entries = (sc_memory_entry_t *)alloc->alloc(alloc->ctx,
        limit * sizeof(sc_memory_entry_t));
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
            sc_memory_entry_free_fields(alloc, &entries[count]);
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

static sc_error_t impl_get_prod(void *ctx, sc_allocator_t *alloc,
    const char *key, size_t key_len, sc_memory_entry_t *out, bool *found) {
    sc_lancedb_memory_t *self = (sc_lancedb_memory_t *)ctx;
    *found = false;
    const char *sql = "SELECT key, text, category, session_id, updated_at "
        "FROM lancedb_memories WHERE key = ?1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return SC_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(stmt, 1, key, (int)key_len, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        read_entry_from_row(stmt, alloc, out);
        *found = true;
    }
    sqlite3_finalize(stmt);
    return SC_OK;
}

static sc_error_t impl_list_prod(void *ctx, sc_allocator_t *alloc,
    const sc_memory_category_t *category,
    const char *session_id, size_t session_id_len,
    sc_memory_entry_t **out, size_t *out_count) {
    sc_lancedb_memory_t *self = (sc_lancedb_memory_t *)ctx;
    const char *sql;
    if (category)
        sql = "SELECT key, text, category, session_id, updated_at "
            "FROM lancedb_memories WHERE category = ?1 ORDER BY updated_at DESC";
    else
        sql = "SELECT key, text, category, session_id, updated_at "
            "FROM lancedb_memories ORDER BY updated_at DESC";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return SC_ERR_MEMORY_BACKEND;
    if (category) sqlite3_bind_text(stmt, 1, category_to_string(category), -1, SQLITE_STATIC);
    size_t cap = 64;
    sc_memory_entry_t *entries = (sc_memory_entry_t *)alloc->alloc(alloc->ctx,
        cap * sizeof(sc_memory_entry_t));
    if (!entries) { sqlite3_finalize(stmt); return SC_ERR_OUT_OF_MEMORY; }
    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            sc_memory_entry_t *n = (sc_memory_entry_t *)alloc->realloc(alloc->ctx, entries,
                cap * sizeof(sc_memory_entry_t), (cap * 2) * sizeof(sc_memory_entry_t));
            if (!n) break;
            entries = n;
            cap *= 2;
        }
        read_entry_from_row(stmt, alloc, &entries[count]);
        if (session_id && session_id_len > 0 && entries[count].session_id &&
            (entries[count].session_id_len != session_id_len ||
             memcmp(entries[count].session_id, session_id, session_id_len) != 0)) {
            sc_memory_entry_free_fields(alloc, &entries[count]);
            continue;
        }
        count++;
    }
    sqlite3_finalize(stmt);
    *out = entries;
    *out_count = count;
    return SC_OK;
}

static sc_error_t impl_forget_prod(void *ctx, const char *key, size_t key_len, bool *deleted) {
    sc_lancedb_memory_t *self = (sc_lancedb_memory_t *)ctx;
    const char *sql = "DELETE FROM lancedb_memories WHERE key = ?1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return SC_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(stmt, 1, key, (int)key_len, SQLITE_STATIC);
    sqlite3_step(stmt);
    *deleted = sqlite3_changes(self->db) > 0;
    sqlite3_finalize(stmt);
    return SC_OK;
}

static sc_error_t impl_count_prod(void *ctx, size_t *out) {
    sc_lancedb_memory_t *self = (sc_lancedb_memory_t *)ctx;
    const char *sql = "SELECT COUNT(*) FROM lancedb_memories";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return SC_ERR_MEMORY_BACKEND;
    *out = sqlite3_step(stmt) == SQLITE_ROW ? (size_t)sqlite3_column_int64(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return SC_OK;
}

static bool impl_health_check_prod(void *ctx) {
    sc_lancedb_memory_t *self = (sc_lancedb_memory_t *)ctx;
    char *err = NULL;
    int rc = sqlite3_exec(self->db, "SELECT 1", NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

static void impl_deinit_prod(void *ctx) {
    sc_lancedb_memory_t *self = (sc_lancedb_memory_t *)ctx;
    if (self->db) sqlite3_close(self->db);
    self->alloc->free(self->alloc->ctx, self, sizeof(sc_lancedb_memory_t));
}

static const sc_memory_vtable_t lancedb_vtable_prod = {
    .name = impl_name_prod,
    .store = impl_store_prod,
    .recall = impl_recall_prod,
    .get = impl_get_prod,
    .list = impl_list_prod,
    .forget = impl_forget_prod,
    .count = impl_count_prod,
    .health_check = impl_health_check_prod,
    .deinit = impl_deinit_prod,
};

sc_memory_t sc_lancedb_memory_create(sc_allocator_t *alloc, const char *db_path) {
    if (!alloc) return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    sqlite3 *db = NULL;
    int rc = sqlite3_open(db_path ? db_path : ":memory:", &db);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    }
    sqlite3_busy_timeout(db, SC_SQLITE_BUSY_TIMEOUT_MS);
    sqlite3_exec(db, "PRAGMA secure_delete=ON;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    char *err = NULL;
    rc = sqlite3_exec(db, schema_sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err) sqlite3_free(err);
        sqlite3_close(db);
        return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    }
    sc_lancedb_memory_t *self = (sc_lancedb_memory_t *)alloc->alloc(alloc->ctx,
        sizeof(sc_lancedb_memory_t));
    if (!self) {
        sqlite3_close(db);
        return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    }
    memset(self, 0, sizeof(sc_lancedb_memory_t));
    self->alloc = alloc;
    self->db = db;
    return (sc_memory_t){ .ctx = self, .vtable = &lancedb_vtable_prod };
}

#else /* !SC_ENABLE_SQLITE — stub */

static const char *impl_name_stub(void *ctx) { (void)ctx; return "lancedb"; }
static sc_error_t impl_store_stub(void *ctx, const char *k, size_t kl, const char *c, size_t cl,
    const sc_memory_category_t *cat, const char *sid, size_t sidl) {
    (void)ctx;(void)k;(void)kl;(void)c;(void)cl;(void)cat;(void)sid;(void)sidl;
    return SC_ERR_NOT_SUPPORTED;
}
static sc_error_t impl_recall_stub(void *ctx, sc_allocator_t *a, const char *q, size_t ql,
    size_t lim, const char *sid, size_t sidl, sc_memory_entry_t **o, size_t *oc) {
    (void)ctx;(void)a;(void)q;(void)ql;(void)lim;(void)sid;(void)sidl;
    *o = NULL; *oc = 0; return SC_ERR_NOT_SUPPORTED;
}
static sc_error_t impl_get_stub(void *ctx, sc_allocator_t *a, const char *k, size_t kl,
    sc_memory_entry_t *o, bool *f) { (void)ctx;(void)a;(void)k;(void)kl;(void)o; *f = false; return SC_ERR_NOT_SUPPORTED; }
static sc_error_t impl_list_stub(void *ctx, sc_allocator_t *a, const sc_memory_category_t *c,
    const char *sid, size_t sidl, sc_memory_entry_t **o, size_t *oc) {
    (void)ctx;(void)a;(void)c;(void)sid;(void)sidl; *o = NULL; *oc = 0; return SC_ERR_NOT_SUPPORTED;
}
static sc_error_t impl_forget_stub(void *ctx, const char *k, size_t kl, bool *d) {
    (void)ctx;(void)k;(void)kl; *d = false; return SC_ERR_NOT_SUPPORTED;
}
static sc_error_t impl_count_stub(void *ctx, size_t *o) { (void)ctx; *o = 0; return SC_ERR_NOT_SUPPORTED; }
static bool impl_health_check_stub(void *ctx) { (void)ctx; return false; }

typedef struct sc_lancedb_memory_stub {
    sc_allocator_t *alloc;
} sc_lancedb_memory_t;

static void impl_deinit_stub(void *ctx) {
    sc_lancedb_memory_t *self = (sc_lancedb_memory_t *)ctx;
    if (self && self->alloc)
        self->alloc->free(self->alloc->ctx, self, sizeof(sc_lancedb_memory_t));
}

static const sc_memory_vtable_t lancedb_vtable_stub = {
    .name = impl_name_stub,
    .store = impl_store_stub,
    .recall = impl_recall_stub,
    .get = impl_get_stub,
    .list = impl_list_stub,
    .forget = impl_forget_stub,
    .count = impl_count_stub,
    .health_check = impl_health_check_stub,
    .deinit = impl_deinit_stub,
};

sc_memory_t sc_lancedb_memory_create(sc_allocator_t *alloc, const char *db_path) {
    (void)db_path;
    if (!alloc) return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    sc_lancedb_memory_t *self = (sc_lancedb_memory_t *)alloc->alloc(alloc->ctx, sizeof(sc_lancedb_memory_t));
    if (!self) return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    memset(self, 0, sizeof(sc_lancedb_memory_t));
    self->alloc = alloc;
    return (sc_memory_t){ .ctx = self, .vtable = &lancedb_vtable_stub };
}

#endif /* SC_ENABLE_SQLITE */
#endif /* SC_IS_TEST */
