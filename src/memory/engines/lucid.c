/* Lucid memory backend — SQLite + lucid CLI for cross-project sync.
 * HU_IS_TEST: in-memory mock.
 * When HU_ENABLE_SQLITE is not set (production build), all operations return
 * HU_ERR_NOT_SUPPORTED. This is intentional, documented stub behavior. */

/*
 * Name suggests: native Lucid database backend.
 * Actually: SQLite-based storage with optional lucid CLI for sync. No Lucid library.
 * A native implementation would require the actual Lucid libraries.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/engines.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(HU_IS_TEST) && HU_IS_TEST
/* ── Mock implementation (HU_IS_TEST) ───────────────────────────────────── */

#define MOCK_MAX_ENTRIES 32

typedef struct mock_entry {
    char *key;
    char *content;
    char *category;
    char *session_id;
    char *timestamp;
} mock_entry_t;

typedef struct hu_lucid_memory {
    hu_allocator_t *alloc;
    mock_entry_t entries[MOCK_MAX_ENTRIES];
    size_t count;
    char *workspace_dir;
    char *lucid_cmd;
} hu_lucid_memory_t;

static void mock_free_entry(hu_lucid_memory_t *self, mock_entry_t *e) {
    if (!self->alloc || !e)
        return;
    if (e->key) {
        self->alloc->free(self->alloc->ctx, e->key, strlen(e->key) + 1);
        e->key = NULL;
    }
    if (e->content) {
        self->alloc->free(self->alloc->ctx, e->content, strlen(e->content) + 1);
        e->content = NULL;
    }
    if (e->category) {
        self->alloc->free(self->alloc->ctx, e->category, strlen(e->category) + 1);
        e->category = NULL;
    }
    if (e->session_id) {
        self->alloc->free(self->alloc->ctx, e->session_id, strlen(e->session_id) + 1);
        e->session_id = NULL;
    }
    if (e->timestamp) {
        self->alloc->free(self->alloc->ctx, e->timestamp, strlen(e->timestamp) + 1);
        e->timestamp = NULL;
    }
}

static mock_entry_t *mock_find(hu_lucid_memory_t *self, const char *key, size_t key_len) {
    for (size_t i = 0; i < self->count; i++) {
        mock_entry_t *e = &self->entries[i];
        if (e->key && strlen(e->key) == key_len && memcmp(e->key, key, key_len) == 0)
            return e;
    }
    return NULL;
}

static int mock_contains(const char *haystack, size_t hlen, const char *needle, size_t nlen) {
    if (nlen == 0)
        return 1;
    if (hlen < nlen)
        return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (memcmp(haystack + i, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

static const char *category_to_string(const hu_memory_category_t *cat) {
    if (!cat)
        return "core";
    switch (cat->tag) {
    case HU_MEMORY_CATEGORY_CORE:
        return "core";
    case HU_MEMORY_CATEGORY_DAILY:
        return "daily";
    case HU_MEMORY_CATEGORY_CONVERSATION:
        return "conversation";
    case HU_MEMORY_CATEGORY_INSIGHT:
        return "insight";
    case HU_MEMORY_CATEGORY_CUSTOM:
        if (cat->data.custom.name && cat->data.custom.name_len > 0)
            return cat->data.custom.name;
        return "custom";
    default:
        return "core";
    }
}

static void fill_entry_from_mock(hu_allocator_t *alloc, const mock_entry_t *m,
                                 hu_memory_entry_t *out) {
    out->id = m->key ? hu_strndup(alloc, m->key, strlen(m->key)) : NULL;
    out->id_len = m->key ? strlen(m->key) : 0;
    out->key = m->key ? hu_strndup(alloc, m->key, strlen(m->key)) : NULL;
    out->key_len = m->key ? strlen(m->key) : 0;
    out->content = m->content ? hu_strndup(alloc, m->content, strlen(m->content)) : NULL;
    out->content_len = m->content ? strlen(m->content) : 0;
    out->category.tag = HU_MEMORY_CATEGORY_CUSTOM;
    out->category.data.custom.name =
        m->category ? hu_strndup(alloc, m->category, strlen(m->category)) : NULL;
    out->category.data.custom.name_len = m->category ? strlen(m->category) : 0;
    out->timestamp = m->timestamp ? hu_strndup(alloc, m->timestamp, strlen(m->timestamp)) : NULL;
    out->timestamp_len = m->timestamp ? strlen(m->timestamp) : 0;
    out->session_id =
        m->session_id ? hu_strndup(alloc, m->session_id, strlen(m->session_id)) : NULL;
    out->session_id_len = m->session_id ? strlen(m->session_id) : 0;
    out->score = NAN;
}

static const char *impl_name(void *ctx) {
    (void)ctx;
    return "lucid";
}

static hu_error_t impl_store(void *ctx, const char *key, size_t key_len, const char *content,
                             size_t content_len, const hu_memory_category_t *category,
                             const char *session_id, size_t session_id_len) {
    hu_lucid_memory_t *self = (hu_lucid_memory_t *)ctx;
    mock_entry_t *e = mock_find(self, key, key_len);
    if (e) {
        mock_free_entry(self, e);
    } else {
        if (self->count >= MOCK_MAX_ENTRIES)
            return HU_ERR_OUT_OF_MEMORY;
        e = &self->entries[self->count++];
    }
    e->key = hu_strndup(self->alloc, key, key_len);
    if (!e->key)
        return HU_ERR_OUT_OF_MEMORY;
    e->content = hu_strndup(self->alloc, content, content_len);
    if (!e->content) {
        self->alloc->free(self->alloc->ctx, e->key, key_len + 1);
        e->key = NULL;
        return HU_ERR_OUT_OF_MEMORY;
    }
    {
        const char *cat_str = category_to_string(category);
        e->category = hu_strndup(self->alloc, cat_str, strlen(cat_str));
        if (!e->category) {
            self->alloc->free(self->alloc->ctx, e->content, content_len + 1);
            self->alloc->free(self->alloc->ctx, e->key, key_len + 1);
            e->key = e->content = NULL;
            return HU_ERR_OUT_OF_MEMORY;
        }
    }
    e->session_id = (session_id && session_id_len > 0)
                        ? hu_strndup(self->alloc, session_id, session_id_len)
                        : NULL;
    {
        char ts[32];
        time_t t = time(NULL);
        struct tm *tm = gmtime(&t);
        if (tm)
            snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm->tm_year + 1900,
                     tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
        else
            snprintf(ts, sizeof(ts), "%ld", (long)t);
        e->timestamp = hu_strndup(self->alloc, ts, strlen(ts));
    }
    return HU_OK;
}

static hu_error_t impl_recall(void *ctx, hu_allocator_t *alloc, const char *query, size_t query_len,
                              size_t limit, const char *session_id, size_t session_id_len,
                              hu_memory_entry_t **out, size_t *out_count) {
    hu_lucid_memory_t *self = (hu_lucid_memory_t *)ctx;
    *out = NULL;
    *out_count = 0;
    if (!query || query_len == 0)
        return HU_OK;
    hu_memory_entry_t *entries =
        (hu_memory_entry_t *)alloc->alloc(alloc->ctx, limit * sizeof(hu_memory_entry_t));
    if (!entries)
        return HU_ERR_OUT_OF_MEMORY;
    size_t count = 0;
    for (size_t i = 0; i < self->count && count < limit; i++) {
        mock_entry_t *m = &self->entries[i];
        if (!m->key || !m->content)
            continue;
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
    return HU_OK;
}

static hu_error_t impl_get(void *ctx, hu_allocator_t *alloc, const char *key, size_t key_len,
                           hu_memory_entry_t *out, bool *found) {
    hu_lucid_memory_t *self = (hu_lucid_memory_t *)ctx;
    *found = false;
    mock_entry_t *m = mock_find(self, key, key_len);
    if (!m)
        return HU_OK;
    fill_entry_from_mock(alloc, m, out);
    *found = true;
    return HU_OK;
}

static hu_error_t impl_list(void *ctx, hu_allocator_t *alloc, const hu_memory_category_t *category,
                            const char *session_id, size_t session_id_len, hu_memory_entry_t **out,
                            size_t *out_count) {
    hu_lucid_memory_t *self = (hu_lucid_memory_t *)ctx;
    *out = NULL;
    *out_count = 0;
    const char *cat_str = category ? category_to_string(category) : NULL;
    size_t cap = 64;
    hu_memory_entry_t *entries =
        (hu_memory_entry_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_memory_entry_t));
    if (!entries)
        return HU_ERR_OUT_OF_MEMORY;
    size_t count = 0;
    for (size_t i = 0; i < self->count; i++) {
        mock_entry_t *m = &self->entries[i];
        if (!m->key || !m->content)
            continue;
        if (cat_str && (!m->category || strcmp(m->category, cat_str) != 0))
            continue;
        if (session_id && session_id_len > 0) {
            if (!m->session_id || strlen(m->session_id) != session_id_len ||
                memcmp(m->session_id, session_id, session_id_len) != 0)
                continue;
        }
        if (count >= cap) {
            hu_memory_entry_t *n = (hu_memory_entry_t *)alloc->realloc(
                alloc->ctx, entries, cap * sizeof(hu_memory_entry_t),
                (cap * 2) * sizeof(hu_memory_entry_t));
            if (!n)
                break;
            entries = n;
            cap *= 2;
        }
        fill_entry_from_mock(alloc, m, &entries[count++]);
    }
    *out = entries;
    *out_count = count;
    return HU_OK;
}

static hu_error_t impl_forget(void *ctx, const char *key, size_t key_len, bool *deleted) {
    hu_lucid_memory_t *self = (hu_lucid_memory_t *)ctx;
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
            return HU_OK;
        }
    }
    return HU_OK;
}

static hu_error_t impl_count(void *ctx, size_t *out) {
    hu_lucid_memory_t *self = (hu_lucid_memory_t *)ctx;
    *out = self->count;
    return HU_OK;
}

static bool impl_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static void impl_deinit(void *ctx) {
    hu_lucid_memory_t *self = (hu_lucid_memory_t *)ctx;
    if (!self)
        return;
    for (size_t i = 0; i < self->count; i++)
        mock_free_entry(self, &self->entries[i]);
    self->count = 0;
    if (self->workspace_dir && self->alloc)
        self->alloc->free(self->alloc->ctx, self->workspace_dir, strlen(self->workspace_dir) + 1);
    if (self->lucid_cmd && self->alloc)
        self->alloc->free(self->alloc->ctx, self->lucid_cmd, strlen(self->lucid_cmd) + 1);
    if (self->alloc)
        self->alloc->free(self->alloc->ctx, self, sizeof(hu_lucid_memory_t));
}

static const hu_memory_vtable_t lucid_vtable = {
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

hu_memory_t hu_lucid_memory_create(hu_allocator_t *alloc, const char *db_path,
                                   const char *workspace_dir) {
    (void)db_path;
    if (!alloc || !workspace_dir)
        return (hu_memory_t){.ctx = NULL, .vtable = NULL};
    hu_lucid_memory_t *self =
        (hu_lucid_memory_t *)alloc->alloc(alloc->ctx, sizeof(hu_lucid_memory_t));
    if (!self)
        return (hu_memory_t){.ctx = NULL, .vtable = NULL};
    memset(self, 0, sizeof(hu_lucid_memory_t));
    self->alloc = alloc;
    self->workspace_dir = hu_strndup(alloc, workspace_dir, strlen(workspace_dir));
    if (!self->workspace_dir) {
        alloc->free(alloc->ctx, self, sizeof(hu_lucid_memory_t));
        return (hu_memory_t){.ctx = NULL, .vtable = NULL};
    }
    self->lucid_cmd = hu_strndup(alloc, "lucid", 5);
    if (!self->lucid_cmd) {
        alloc->free(alloc->ctx, self->workspace_dir, strlen(workspace_dir) + 1);
        alloc->free(alloc->ctx, self, sizeof(hu_lucid_memory_t));
        return (hu_memory_t){.ctx = NULL, .vtable = NULL};
    }
    return (hu_memory_t){.ctx = self, .vtable = &lucid_vtable};
}

#else /* !HU_IS_TEST — production: SQLite */

#ifdef HU_ENABLE_SQLITE

#include "human/memory/sql_common.h"
#include <sqlite3.h>
#include <stdbool.h>

static bool fts5_available(sqlite3 *db) {
    int rc = sqlite3_exec(db, "CREATE VIRTUAL TABLE IF NOT EXISTS temp._fts5_check USING fts5(x)",
                          NULL, NULL, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_exec(db, "DROP TABLE IF EXISTS temp._fts5_check", NULL, NULL, NULL);
        return true;
    }
    return false;
}

typedef struct hu_lucid_memory_prod {
    hu_allocator_t *alloc;
    sqlite3 *db;
    char *workspace_dir;
    char *lucid_cmd;
    bool has_fts5;
} hu_lucid_memory_t;

#define HU_SQLITE_BUSY_TIMEOUT_MS 5000

static const char *schema_sql =
    "CREATE TABLE IF NOT EXISTS memories("
    "id TEXT PRIMARY KEY,key TEXT NOT NULL UNIQUE,"
    "content TEXT NOT NULL,category TEXT NOT NULL DEFAULT'core',"
    "session_id TEXT,created_at TEXT NOT NULL,updated_at TEXT NOT NULL);"
    "CREATE INDEX IF NOT EXISTS idx_memories_category ON memories(category);"
    "CREATE INDEX IF NOT EXISTS idx_memories_key ON memories(key);"
    "CREATE INDEX IF NOT EXISTS idx_memories_session ON memories(session_id);";

static void get_timestamp(char *buf, size_t buf_size) {
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    if (tm)
        strftime(buf, buf_size, "%Y-%m-%dT%H:%M:%SZ", tm);
    else
        snprintf(buf, buf_size, "%ld", (long)t);
}

static char *generate_id(hu_allocator_t *alloc) {
    char ts[32];
    get_timestamp(ts, sizeof(ts));
    return hu_sprintf(alloc, "mem_%ld_%s", (long)time(NULL), ts);
}

static const char *category_to_string(const hu_memory_category_t *cat) {
    if (!cat)
        return "core";
    switch (cat->tag) {
    case HU_MEMORY_CATEGORY_CORE:
        return "core";
    case HU_MEMORY_CATEGORY_DAILY:
        return "daily";
    case HU_MEMORY_CATEGORY_CONVERSATION:
        return "conversation";
    case HU_MEMORY_CATEGORY_INSIGHT:
        return "insight";
    case HU_MEMORY_CATEGORY_CUSTOM:
        if (cat->data.custom.name && cat->data.custom.name_len > 0)
            return cat->data.custom.name;
        return "custom";
    default:
        return "core";
    }
}

static hu_error_t read_entry_from_row(sqlite3_stmt *stmt, hu_allocator_t *alloc,
                                      hu_memory_entry_t *out) {
    const char *id_p = (const char *)sqlite3_column_text(stmt, 0);
    const char *key_p = (const char *)sqlite3_column_text(stmt, 1);
    const char *content_p = (const char *)sqlite3_column_text(stmt, 2);
    const char *category_p = (const char *)sqlite3_column_text(stmt, 3);
    const char *timestamp_p = (const char *)sqlite3_column_text(stmt, 4);
    const char *session_id_p = (const char *)sqlite3_column_text(stmt, 5);
    const char *source_p = (const char *)sqlite3_column_text(stmt, 6);
    size_t id_len = id_p ? (size_t)sqlite3_column_bytes(stmt, 0) : 0;
    size_t key_len = key_p ? (size_t)sqlite3_column_bytes(stmt, 1) : 0;
    size_t content_len = content_p ? (size_t)sqlite3_column_bytes(stmt, 2) : 0;
    size_t timestamp_len = timestamp_p ? (size_t)sqlite3_column_bytes(stmt, 4) : 0;
    size_t session_id_len = session_id_p ? (size_t)sqlite3_column_bytes(stmt, 5) : 0;
    size_t source_len = source_p ? (size_t)sqlite3_column_bytes(stmt, 6) : 0;
    out->id = id_p ? hu_strndup(alloc, id_p, id_len) : NULL;
    out->id_len = id_len;
    out->key = key_p ? hu_strndup(alloc, key_p, key_len) : NULL;
    out->key_len = key_len;
    out->content = content_p ? hu_strndup(alloc, content_p, content_len) : NULL;
    out->content_len = content_len;
    out->category.tag = HU_MEMORY_CATEGORY_CUSTOM;
    out->category.data.custom.name =
        category_p
            ? hu_strndup(alloc, category_p, category_p ? (size_t)sqlite3_column_bytes(stmt, 3) : 0)
            : NULL;
    out->category.data.custom.name_len = category_p ? (size_t)sqlite3_column_bytes(stmt, 3) : 0;
    out->timestamp = timestamp_p ? hu_strndup(alloc, timestamp_p, timestamp_len) : NULL;
    out->timestamp_len = timestamp_len;
    out->session_id = session_id_p ? hu_strndup(alloc, session_id_p, session_id_len) : NULL;
    out->session_id_len = session_id_len;
    out->source = source_p ? hu_strndup(alloc, source_p, source_len) : NULL;
    out->source_len = source_len;
    if (sqlite3_column_count(stmt) > 7)
        out->score = sqlite3_column_double(stmt, 7);
    else
        out->score = NAN;
    return HU_OK;
}

static const char *impl_name_prod(void *ctx) {
    (void)ctx;
    return "lucid";
}

static hu_error_t impl_store_prod(void *ctx, const char *key, size_t key_len, const char *content,
                                  size_t content_len, const hu_memory_category_t *category,
                                  const char *session_id, size_t session_id_len) {
    hu_lucid_memory_t *self = (hu_lucid_memory_t *)ctx;
    char ts[64];
    get_timestamp(ts, sizeof(ts));
    char *id = generate_id(self->alloc);
    if (!id)
        return HU_ERR_OUT_OF_MEMORY;
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
        hu_str_free(self->alloc, id);
        return HU_ERR_MEMORY_STORE;
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
    hu_str_free(self->alloc, id);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_STORE;
    return HU_OK;
}

static hu_error_t impl_store_ex_prod(void *ctx, const char *key, size_t key_len,
                                     const char *content, size_t content_len,
                                     const hu_memory_category_t *category, const char *session_id,
                                     size_t session_id_len, const hu_memory_store_opts_t *opts) {
    hu_lucid_memory_t *self = (hu_lucid_memory_t *)ctx;
    char ts[64];
    get_timestamp(ts, sizeof(ts));
    char *id = generate_id(self->alloc);
    if (!id)
        return HU_ERR_OUT_OF_MEMORY;
    const char *source = (opts && opts->source && opts->source_len > 0) ? opts->source : NULL;
    size_t source_len = source ? opts->source_len : 0;
    const char *cat_str = category_to_string(category);
    const char *sql =
        "INSERT INTO memories (id, key, content, category, session_id, source, created_at, "
        "updated_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
        "ON CONFLICT(key) DO UPDATE SET "
        "content = excluded.content, category = excluded.category, "
        "session_id = excluded.session_id, source = excluded.source, updated_at = "
        "excluded.updated_at";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        hu_str_free(self->alloc, id);
        return HU_ERR_MEMORY_STORE;
    }
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, key, (int)key_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, content, (int)content_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, cat_str, -1, SQLITE_STATIC);
    if (session_id && session_id_len > 0)
        sqlite3_bind_text(stmt, 5, session_id, (int)session_id_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 5);
    if (source && source_len > 0)
        sqlite3_bind_text(stmt, 6, source, (int)source_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 6);
    sqlite3_bind_text(stmt, 7, ts, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, ts, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    hu_str_free(self->alloc, id);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_STORE;
    return HU_OK;
}

static hu_error_t impl_recall_prod(void *ctx, hu_allocator_t *alloc, const char *query,
                                   size_t query_len, size_t limit, const char *session_id,
                                   size_t session_id_len, hu_memory_entry_t **out,
                                   size_t *out_count) {
    hu_lucid_memory_t *self = (hu_lucid_memory_t *)ctx;
    *out = NULL;
    *out_count = 0;
    if (!query || query_len == 0)
        return HU_OK;

    /* FTS5 BM25 search - build query from words (escape quotes, wrap in "", join with OR) */
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
            if (!first)
                fts_len += (size_t)snprintf(fts_buf + fts_len, sizeof(fts_buf) - fts_len, " OR ");
            fts_len += (size_t)snprintf(fts_buf + fts_len, sizeof(fts_buf) - fts_len, "\"%s\"",
                                        escaped_word);
            first = false;
        }
    }

    /* Try FTS5 first; fall back to LIKE when FTS unavailable or returns no rows */
    if (fts_len > 0 && self->has_fts5) {
        const char *sql =
            "SELECT m.id, m.key, m.content, m.category, m.updated_at, m.session_id, m.source, "
            "bm25(memories_fts) as score FROM memories_fts f "
            "JOIN memories m ON m.rowid = f.rowid "
            "WHERE memories_fts MATCH ?1 ORDER BY score LIMIT ?2";
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, fts_buf, (int)fts_len, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 2, (sqlite3_int64)limit);
            hu_memory_entry_t *entries =
                (hu_memory_entry_t *)alloc->alloc(alloc->ctx, limit * sizeof(hu_memory_entry_t));
            if (!entries) {
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            size_t count = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW && count < limit) {
                read_entry_from_row(stmt, alloc, &entries[count]);
                if (session_id && session_id_len > 0 && entries[count].session_id &&
                    (entries[count].session_id_len != session_id_len ||
                     memcmp(entries[count].session_id, session_id, session_id_len) != 0)) {
                    hu_memory_entry_free_fields(alloc, &entries[count]);
                    continue;
                }
                count++;
            }
            sqlite3_finalize(stmt);
            if (count > 0) {
                *out = entries;
                *out_count = count;
                return HU_OK;
            }
            alloc->free(alloc->ctx, entries, limit * sizeof(hu_memory_entry_t));
        } else if (stmt)
            sqlite3_finalize(stmt);
    }

    /* Fallback: LIKE search */
    char *like_pattern = (char *)alloc->alloc(alloc->ctx, query_len + 3);
    if (!like_pattern)
        return HU_ERR_OUT_OF_MEMORY;
    like_pattern[0] = '%';
    memcpy(like_pattern + 1, query, query_len);
    like_pattern[query_len + 1] = '%';
    like_pattern[query_len + 2] = '\0';
    const char *sql =
        "SELECT id, key, content, category, updated_at, session_id, source "
        "FROM memories WHERE content LIKE ?1 OR key LIKE ?1 ORDER BY updated_at DESC LIMIT ?2";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        alloc->free(alloc->ctx, like_pattern, query_len + 3);
        return HU_ERR_MEMORY_RECALL;
    }
    sqlite3_bind_text(stmt, 1, like_pattern, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)limit);
    hu_memory_entry_t *entries =
        (hu_memory_entry_t *)alloc->alloc(alloc->ctx, limit * sizeof(hu_memory_entry_t));
    if (!entries) {
        sqlite3_finalize(stmt);
        alloc->free(alloc->ctx, like_pattern, query_len + 3);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < limit) {
        read_entry_from_row(stmt, alloc, &entries[count]);
        if (session_id && session_id_len > 0 && entries[count].session_id &&
            (entries[count].session_id_len != session_id_len ||
             memcmp(entries[count].session_id, session_id, session_id_len) != 0)) {
            hu_memory_entry_free_fields(alloc, &entries[count]);
            continue;
        }
        count++;
    }
    sqlite3_finalize(stmt);
    alloc->free(alloc->ctx, like_pattern, query_len + 3);
    *out = entries;
    *out_count = count;
    return HU_OK;
}

static hu_error_t impl_get_prod(void *ctx, hu_allocator_t *alloc, const char *key, size_t key_len,
                                hu_memory_entry_t *out, bool *found) {
    hu_lucid_memory_t *self = (hu_lucid_memory_t *)ctx;
    *found = false;
    const char *sql = "SELECT id, key, content, category, updated_at, session_id, source "
                      "FROM memories WHERE key = ?1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(stmt, 1, key, (int)key_len, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        read_entry_from_row(stmt, alloc, out);
        *found = true;
    }
    sqlite3_finalize(stmt);
    return HU_OK;
}

static hu_error_t impl_list_prod(void *ctx, hu_allocator_t *alloc,
                                 const hu_memory_category_t *category, const char *session_id,
                                 size_t session_id_len, hu_memory_entry_t **out,
                                 size_t *out_count) {
    hu_lucid_memory_t *self = (hu_lucid_memory_t *)ctx;
    const char *sql;
    if (category)
        sql = "SELECT id, key, content, category, updated_at, session_id, source "
              "FROM memories WHERE category = ?1 ORDER BY updated_at DESC";
    else
        sql = "SELECT id, key, content, category, updated_at, session_id, source "
              "FROM memories ORDER BY updated_at DESC";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    if (category)
        sqlite3_bind_text(stmt, 1, category_to_string(category), -1, SQLITE_STATIC);
    size_t cap = 64;
    hu_memory_entry_t *entries =
        (hu_memory_entry_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_memory_entry_t));
    if (!entries) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            hu_memory_entry_t *n = (hu_memory_entry_t *)alloc->realloc(
                alloc->ctx, entries, cap * sizeof(hu_memory_entry_t),
                (cap * 2) * sizeof(hu_memory_entry_t));
            if (!n)
                break;
            entries = n;
            cap *= 2;
        }
        read_entry_from_row(stmt, alloc, &entries[count]);
        if (session_id && session_id_len > 0 && entries[count].session_id &&
            (entries[count].session_id_len != session_id_len ||
             memcmp(entries[count].session_id, session_id, session_id_len) != 0)) {
            hu_memory_entry_free_fields(alloc, &entries[count]);
            continue;
        }
        count++;
    }
    sqlite3_finalize(stmt);
    *out = entries;
    *out_count = count;
    return HU_OK;
}

static hu_error_t impl_forget_prod(void *ctx, const char *key, size_t key_len, bool *deleted) {
    hu_lucid_memory_t *self = (hu_lucid_memory_t *)ctx;
    const char *sql = "DELETE FROM memories WHERE key = ?1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(stmt, 1, key, (int)key_len, SQLITE_STATIC);
    int step_rc = sqlite3_step(stmt);
    if (step_rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return HU_ERR_MEMORY_BACKEND;
    }
    *deleted = sqlite3_changes(self->db) > 0;
    sqlite3_finalize(stmt);
    return HU_OK;
}

static hu_error_t impl_count_prod(void *ctx, size_t *out) {
    hu_lucid_memory_t *self = (hu_lucid_memory_t *)ctx;
    const char *sql = "SELECT COUNT(*) FROM memories";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    *out = sqlite3_step(stmt) == SQLITE_ROW ? (size_t)sqlite3_column_int64(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return HU_OK;
}

static bool impl_health_check_prod(void *ctx) {
    hu_lucid_memory_t *self = (hu_lucid_memory_t *)ctx;
    char *err = NULL;
    int rc = sqlite3_exec(self->db, "SELECT 1", NULL, NULL, &err);
    if (err)
        sqlite3_free(err);
    return rc == SQLITE_OK;
}

static void impl_deinit_prod(void *ctx) {
    hu_lucid_memory_t *self = (hu_lucid_memory_t *)ctx;
    if (self->db)
        sqlite3_close(self->db);
    if (self->workspace_dir && self->alloc)
        self->alloc->free(self->alloc->ctx, self->workspace_dir, strlen(self->workspace_dir) + 1);
    if (self->lucid_cmd && self->alloc)
        self->alloc->free(self->alloc->ctx, self->lucid_cmd, strlen(self->lucid_cmd) + 1);
    self->alloc->free(self->alloc->ctx, self, sizeof(hu_lucid_memory_t));
}

static const hu_memory_vtable_t lucid_vtable_prod = {
    .name = impl_name_prod,
    .store = impl_store_prod,
    .store_ex = impl_store_ex_prod,
    .recall = impl_recall_prod,
    .get = impl_get_prod,
    .list = impl_list_prod,
    .forget = impl_forget_prod,
    .count = impl_count_prod,
    .health_check = impl_health_check_prod,
    .deinit = impl_deinit_prod,
};

hu_memory_t hu_lucid_memory_create(hu_allocator_t *alloc, const char *db_path,
                                   const char *workspace_dir) {
    if (!alloc || !workspace_dir)
        return (hu_memory_t){.ctx = NULL, .vtable = NULL};
    sqlite3 *db = NULL;
    int rc = sqlite3_open(db_path ? db_path : ":memory:", &db);
    if (rc != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return (hu_memory_t){.ctx = NULL, .vtable = NULL};
    }
    sqlite3_busy_timeout(db, HU_SQLITE_BUSY_TIMEOUT_MS);
    sqlite3_exec(db, HU_SQL_PRAGMA_INIT, NULL, NULL, NULL);
    char *err = NULL;
    rc = sqlite3_exec(db, schema_sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err)
            sqlite3_free(err);
        sqlite3_close(db);
        return (hu_memory_t){.ctx = NULL, .vtable = NULL};
    }
    {
        char *alter_err = NULL;
        sqlite3_exec(db, "ALTER TABLE memories ADD COLUMN source TEXT", NULL, NULL, &alter_err);
        if (alter_err)
            sqlite3_free(alter_err);
    }
    bool has_fts5 = fts5_available(db);
    if (has_fts5) {
        sqlite3_exec(db,
                     "CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts "
                     "USING fts5(key, content, content=memories, content_rowid=rowid)",
                     NULL, NULL, NULL);
        sqlite3_exec(db, "INSERT INTO memories_fts(memories_fts) VALUES('rebuild')", NULL, NULL,
                     NULL);
        sqlite3_exec(db,
                     "CREATE TRIGGER IF NOT EXISTS memories_ai AFTER INSERT ON memories "
                     "BEGIN INSERT INTO memories_fts(rowid, key, content) VALUES "
                     "(new.rowid, new.key, new.content); END",
                     NULL, NULL, NULL);
        sqlite3_exec(db,
                     "CREATE TRIGGER IF NOT EXISTS memories_ad AFTER DELETE ON memories "
                     "BEGIN INSERT INTO memories_fts(memories_fts, rowid, key, content) "
                     "VALUES ('delete', old.rowid, old.key, old.content); END",
                     NULL, NULL, NULL);
        sqlite3_exec(db,
                     "CREATE TRIGGER IF NOT EXISTS memories_au AFTER UPDATE ON memories "
                     "BEGIN INSERT INTO memories_fts(memories_fts, rowid, key, content) "
                     "VALUES ('delete', old.rowid, old.key, old.content); "
                     "INSERT INTO memories_fts(rowid, key, content) VALUES "
                     "(new.rowid, new.key, new.content); END",
                     NULL, NULL, NULL);
    }
    hu_lucid_memory_t *self =
        (hu_lucid_memory_t *)alloc->alloc(alloc->ctx, sizeof(hu_lucid_memory_t));
    if (!self) {
        sqlite3_close(db);
        return (hu_memory_t){.ctx = NULL, .vtable = NULL};
    }
    memset(self, 0, sizeof(hu_lucid_memory_t));
    self->alloc = alloc;
    self->db = db;
    self->has_fts5 = has_fts5;
    self->workspace_dir = hu_strndup(alloc, workspace_dir, strlen(workspace_dir));
    self->lucid_cmd = hu_strndup(alloc, "lucid", 5);
    return (hu_memory_t){.ctx = self, .vtable = &lucid_vtable_prod};
}

#else /* !HU_ENABLE_SQLITE — stub */

static const char *impl_name_stub(void *ctx) {
    (void)ctx;
    return "lucid";
}
static hu_error_t impl_store_stub(void *ctx, const char *k, size_t kl, const char *c, size_t cl,
                                  const hu_memory_category_t *cat, const char *sid, size_t sidl) {
    (void)ctx;
    (void)k;
    (void)kl;
    (void)c;
    (void)cl;
    (void)cat;
    (void)sid;
    (void)sidl;
    return HU_ERR_NOT_SUPPORTED;
}
static hu_error_t impl_recall_stub(void *ctx, hu_allocator_t *a, const char *q, size_t ql,
                                   size_t lim, const char *sid, size_t sidl, hu_memory_entry_t **o,
                                   size_t *oc) {
    (void)ctx;
    (void)a;
    (void)q;
    (void)ql;
    (void)lim;
    (void)sid;
    (void)sidl;
    *o = NULL;
    *oc = 0;
    return HU_ERR_NOT_SUPPORTED;
}
static hu_error_t impl_get_stub(void *ctx, hu_allocator_t *a, const char *k, size_t kl,
                                hu_memory_entry_t *o, bool *f) {
    (void)ctx;
    (void)a;
    (void)k;
    (void)kl;
    (void)o;
    *f = false;
    return HU_ERR_NOT_SUPPORTED;
}
static hu_error_t impl_list_stub(void *ctx, hu_allocator_t *a, const hu_memory_category_t *c,
                                 const char *sid, size_t sidl, hu_memory_entry_t **o, size_t *oc) {
    (void)ctx;
    (void)a;
    (void)c;
    (void)sid;
    (void)sidl;
    *o = NULL;
    *oc = 0;
    return HU_ERR_NOT_SUPPORTED;
}
static hu_error_t impl_forget_stub(void *ctx, const char *k, size_t kl, bool *d) {
    (void)ctx;
    (void)k;
    (void)kl;
    *d = false;
    return HU_ERR_NOT_SUPPORTED;
}
static hu_error_t impl_count_stub(void *ctx, size_t *o) {
    (void)ctx;
    *o = 0;
    return HU_ERR_NOT_SUPPORTED;
}
static bool impl_health_check_stub(void *ctx) {
    (void)ctx;
    return false;
}

typedef struct hu_lucid_memory_stub {
    hu_allocator_t *alloc;
    void *db;
    char *workspace_dir;
    char *lucid_cmd;
} hu_lucid_memory_t;

static void impl_deinit_stub(void *ctx) {
    hu_lucid_memory_t *self = (hu_lucid_memory_t *)ctx;
    if (self) {
        if (self->workspace_dir && self->alloc)
            self->alloc->free(self->alloc->ctx, self->workspace_dir,
                              strlen(self->workspace_dir) + 1);
        if (self->lucid_cmd && self->alloc)
            self->alloc->free(self->alloc->ctx, self->lucid_cmd, strlen(self->lucid_cmd) + 1);
        if (self->alloc)
            self->alloc->free(self->alloc->ctx, self, sizeof(hu_lucid_memory_t));
    }
}

static const hu_memory_vtable_t lucid_vtable_stub = {
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

hu_memory_t hu_lucid_memory_create(hu_allocator_t *alloc, const char *db_path,
                                   const char *workspace_dir) {
    (void)db_path;
    if (!alloc || !workspace_dir)
        return (hu_memory_t){.ctx = NULL, .vtable = NULL};
    hu_lucid_memory_t *self =
        (hu_lucid_memory_t *)alloc->alloc(alloc->ctx, sizeof(hu_lucid_memory_t));
    if (!self)
        return (hu_memory_t){.ctx = NULL, .vtable = NULL};
    memset(self, 0, sizeof(hu_lucid_memory_t));
    self->alloc = alloc;
    self->workspace_dir = alloc->alloc(alloc->ctx, strlen(workspace_dir) + 1);
    if (!self->workspace_dir) {
        alloc->free(alloc->ctx, self, sizeof(hu_lucid_memory_t));
        return (hu_memory_t){.ctx = NULL, .vtable = NULL};
    }
    memcpy(self->workspace_dir, workspace_dir, strlen(workspace_dir) + 1);
    self->lucid_cmd = alloc->alloc(alloc->ctx, 6);
    if (!self->lucid_cmd) {
        alloc->free(alloc->ctx, self->workspace_dir, strlen(workspace_dir) + 1);
        alloc->free(alloc->ctx, self, sizeof(hu_lucid_memory_t));
        return (hu_memory_t){.ctx = NULL, .vtable = NULL};
    }
    memcpy(self->lucid_cmd, "lucid", 6);
    return (hu_memory_t){.ctx = self, .vtable = &lucid_vtable_stub};
}

#endif /* HU_ENABLE_SQLITE */
#endif /* HU_IS_TEST */
