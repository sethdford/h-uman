/* PostgreSQL memory backend — full interface with libpq when SC_ENABLE_POSTGRES.
 * In SC_IS_TEST: in-memory mock.
 * When SC_ENABLE_POSTGRES is not set, all operations return SC_ERR_NOT_SUPPORTED.
 * This is intentional, documented stub behavior. */

#include "seaclaw/memory/engines.h"
#include "seaclaw/memory.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include <stdlib.h>
#include <string.h>

#ifdef SC_ENABLE_POSTGRES
#include <libpq-fe.h>
#endif

#define MOCK_MAX_ENTRIES 32

typedef struct mock_entry {
    char *key;
    char *content;
    char *category;
    char *session_id;
} mock_entry_t;

typedef struct sc_postgres_memory {
    sc_allocator_t *alloc;
#ifdef SC_ENABLE_POSTGRES
    PGconn *conn;
    char *schema_q;
    char *table_q;
#else
    void *unused_pg;
#endif
#if defined(SC_IS_TEST) && SC_IS_TEST
    mock_entry_t entries[MOCK_MAX_ENTRIES];
    size_t entry_count;
#endif
} sc_postgres_memory_t;

__attribute__((unused))
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

#if defined(SC_IS_TEST) && SC_IS_TEST
static void mock_free_entry(sc_allocator_t *alloc, mock_entry_t *e) {
    if (!alloc || !e) return;
    if (e->key) alloc->free(alloc->ctx, e->key, strlen(e->key) + 1);
    if (e->content) alloc->free(alloc->ctx, e->content, strlen(e->content) + 1);
    if (e->category) alloc->free(alloc->ctx, e->category, strlen(e->category) + 1);
    if (e->session_id) alloc->free(alloc->ctx, e->session_id, strlen(e->session_id) + 1);
    e->key = e->content = e->category = e->session_id = NULL;
}

static mock_entry_t *mock_find_by_key(sc_postgres_memory_t *self,
    const char *key, size_t key_len) {
    for (size_t i = 0; i < self->entry_count; i++) {
        mock_entry_t *e = &self->entries[i];
        if (e->key && strlen(e->key) == key_len && memcmp(e->key, key, key_len) == 0)
            return e;
    }
    return NULL;
}

static int mock_contains_substring(const char *haystack, size_t hlen,
    const char *needle, size_t nlen) {
    if (nlen == 0) return 1;
    if (hlen < nlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (memcmp(haystack + i, needle, nlen) == 0) return 1;
    }
    return 0;
}
#endif /* SC_IS_TEST */

static const char *impl_name(void *ctx) {
    (void)ctx;
    return "postgres";
}

static sc_error_t impl_store(void *ctx,
    const char *key, size_t key_len,
    const char *content, size_t content_len,
    const sc_memory_category_t *category,
    const char *session_id, size_t session_id_len) {
#if defined(SC_IS_TEST) && SC_IS_TEST
    sc_postgres_memory_t *self = (sc_postgres_memory_t *)ctx;
    sc_allocator_t *alloc = self->alloc;
    mock_entry_t *existing = mock_find_by_key(self, key, key_len);
    const char *cat_str = category_to_string(category);

    if (existing) {
        if (existing->content) alloc->free(alloc->ctx, existing->content, strlen(existing->content) + 1);
        if (existing->category) alloc->free(alloc->ctx, existing->category, strlen(existing->category) + 1);
        if (existing->session_id) alloc->free(alloc->ctx, existing->session_id, strlen(existing->session_id) + 1);
        existing->content = sc_strndup(alloc, content, content_len);
        existing->category = sc_strndup(alloc, cat_str, strlen(cat_str));
        existing->session_id = (session_id && session_id_len > 0)
            ? sc_strndup(alloc, session_id, session_id_len) : NULL;
        return existing->content ? SC_OK : SC_ERR_OUT_OF_MEMORY;
    }

    if (self->entry_count >= MOCK_MAX_ENTRIES) return SC_ERR_OUT_OF_MEMORY;
    mock_entry_t *e = &self->entries[self->entry_count];
    e->key = sc_strndup(alloc, key, key_len);
    e->content = sc_strndup(alloc, content, content_len);
    e->category = sc_strndup(alloc, cat_str, strlen(cat_str));
    e->session_id = (session_id && session_id_len > 0)
        ? sc_strndup(alloc, session_id, session_id_len) : NULL;
    if (!e->key || !e->content || !e->category) {
        mock_free_entry(alloc, e);
        return SC_ERR_OUT_OF_MEMORY;
    }
    self->entry_count++;
    return SC_OK;

#elif defined(SC_ENABLE_POSTGRES)
    sc_postgres_memory_t *self = (sc_postgres_memory_t *)ctx;
    if (!self->conn || PQstatus(self->conn) != CONNECTION_OK)
        return SC_ERR_MEMORY_BACKEND;

    const char *cat_str = category_to_string(category);
    const char *params[] = { key, content, cat_str, session_id ? session_id : "" };
    int param_lens[] = { (int)key_len, (int)content_len, (int)strlen(cat_str),
        session_id ? (int)session_id_len : 0 };

    char sql[512];
    (void)snprintf(sql, sizeof(sql),
        "INSERT INTO %s.%s (key, content, category, session_id, created_at, updated_at) "
        "VALUES ($1::text, $2::text, $3::text, $4::text, NOW(), NOW()) "
        "ON CONFLICT (key) DO UPDATE SET content=$2::text, category=$3::text, session_id=$4::text, updated_at=NOW()",
        self->schema_q, self->table_q);

    PGresult *res = PQexecParams(self->conn, sql, 4, NULL,
        (const char *const *)params, param_lens, NULL, 0);
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return SC_ERR_MEMORY_STORE;
    }
    PQclear(res);
    return SC_OK;
#else
    (void)ctx;(void)key;(void)key_len;(void)content;(void)content_len;
    (void)category;(void)session_id;(void)session_id_len;
    return SC_ERR_NOT_SUPPORTED;
#endif
}

static sc_error_t impl_recall(void *ctx, sc_allocator_t *alloc,
    const char *query, size_t query_len,
    size_t limit,
    const char *session_id, size_t session_id_len,
    sc_memory_entry_t **out, size_t *out_count) {
#if defined(SC_IS_TEST) && SC_IS_TEST
    sc_postgres_memory_t *self = (sc_postgres_memory_t *)ctx;
    *out = NULL;
    *out_count = 0;
    if (query_len == 0) return SC_OK;  /* empty query returns empty (match Zig) */

    size_t cap = 0;
    size_t n = 0;
    sc_memory_entry_t *results = NULL;

    for (size_t i = 0; i < self->entry_count && n < limit; i++) {
        mock_entry_t *e = &self->entries[i];
        if (!e->key) continue;
        bool match = mock_contains_substring(e->key, strlen(e->key), query, query_len)
            || mock_contains_substring(e->content, strlen(e->content), query, query_len);
        if (session_id && session_id_len > 0 && e->session_id) {
            if (strlen(e->session_id) != session_id_len ||
                memcmp(e->session_id, session_id, session_id_len) != 0)
                match = false;
        } else if (session_id && session_id_len > 0 && !e->session_id)
            match = false;

        if (!match) continue;

        if (n >= cap) {
            size_t new_cap = cap ? cap * 2 : 4;
            sc_memory_entry_t *tmp = (sc_memory_entry_t *)alloc->realloc(
                alloc->ctx, results, cap * sizeof(sc_memory_entry_t),
                new_cap * sizeof(sc_memory_entry_t));
            if (!tmp) {
                for (size_t j = 0; j < n; j++) sc_memory_entry_free_fields(alloc, &results[j]);
                if (results) alloc->free(alloc->ctx, results, cap * sizeof(sc_memory_entry_t));
                return SC_ERR_OUT_OF_MEMORY;
            }
            results = tmp;
            cap = new_cap;
        }

        sc_memory_entry_t *r = &results[n];
        memset(r, 0, sizeof(*r));
        r->id = r->key = sc_strndup(alloc, e->key, strlen(e->key));
        r->key_len = strlen(e->key);
        r->id_len = r->key_len;
        r->content = sc_strndup(alloc, e->content, strlen(e->content));
        r->content_len = strlen(e->content);
        r->timestamp = sc_sprintf(alloc, "0");
        r->timestamp_len = r->timestamp ? strlen(r->timestamp) : 0;
        if (e->session_id) {
            r->session_id = sc_strndup(alloc, e->session_id, strlen(e->session_id));
            r->session_id_len = strlen(e->session_id);
        }
        r->category.tag = SC_MEMORY_CATEGORY_CUSTOM;
        r->category.data.custom.name = sc_strndup(alloc, e->category, strlen(e->category));
        r->category.data.custom.name_len = strlen(e->category);
        if (!r->key || !r->content) {
            for (size_t j = 0; j <= n; j++) sc_memory_entry_free_fields(alloc, &results[j]);
            alloc->free(alloc->ctx, results, cap * sizeof(sc_memory_entry_t));
            return SC_ERR_OUT_OF_MEMORY;
        }
        n++;
    }

    *out = results;
    *out_count = n;
    return SC_OK;

#elif defined(SC_ENABLE_POSTGRES)
    sc_postgres_memory_t *self = (sc_postgres_memory_t *)ctx;
    if (!self->conn || PQstatus(self->conn) != CONNECTION_OK) {
        *out = NULL;
        *out_count = 0;
        return SC_ERR_MEMORY_BACKEND;
    }

    char sql[512];
    char pattern[260];
    size_t plen = query_len < 255 ? query_len : 255;
    memcpy(pattern, query, plen);
    pattern[plen] = '\0';
    char param[512];
    (void)snprintf(param, sizeof(param), "%%%s%%", pattern);

    (void)snprintf(sql, sizeof(sql),
        "SELECT key, content, category, session_id, updated_at FROM %s.%s WHERE key ILIKE $1 OR content ILIKE $1 ORDER BY updated_at DESC LIMIT %zu",
        self->schema_q, self->table_q, limit);

    const char *params[] = { param };
    int param_lens[] = { (int)strlen(param) };

    PGresult *res = PQexecParams(self->conn, sql, 1, NULL,
        (const char *const *)params, param_lens, NULL, 0);
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (res) PQclear(res);
        *out = NULL;
        *out_count = 0;
        return SC_ERR_MEMORY_RECALL;
    }

    int rows = PQntuples(res);
    if (rows == 0) {
        PQclear(res);
        *out = NULL;
        *out_count = 0;
        return SC_OK;
    }

    sc_memory_entry_t *entries = (sc_memory_entry_t *)alloc->alloc(
        alloc->ctx, (size_t)rows * sizeof(sc_memory_entry_t));
    if (!entries) {
        PQclear(res);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memset(entries, 0, (size_t)rows * sizeof(sc_memory_entry_t));

    for (int i = 0; i < rows; i++) {
        sc_memory_entry_t *e = &entries[i];
        const char *k = PQgetvalue(res, i, 0);
        const char *c = PQgetvalue(res, i, 1);
        const char *cat = PQgetvalue(res, i, 2);
        const char *sid = PQgetvalue(res, i, 3);
        const char *ts = PQgetvalue(res, i, 4);
        size_t klen = k ? strlen(k) : 0;
        size_t clen = c ? strlen(c) : 0;
        e->key = sc_strndup(alloc, k, klen);
        e->key_len = klen;
        e->id = e->key;
        e->id_len = klen;
        e->content = sc_strndup(alloc, c, clen);
        e->content_len = clen;
        e->timestamp = ts ? sc_strndup(alloc, ts, strlen(ts)) : NULL;
        e->timestamp_len = e->timestamp ? strlen(e->timestamp) : 0;
        e->session_id = sid && sid[0] ? sc_strndup(alloc, sid, strlen(sid)) : NULL;
        e->session_id_len = e->session_id ? strlen(e->session_id) : 0;
        e->category.tag = SC_MEMORY_CATEGORY_CUSTOM;
        if (cat && cat[0]) {
            e->category.data.custom.name = sc_strndup(alloc, cat, strlen(cat));
            e->category.data.custom.name_len = strlen(cat);
        }
    }
    PQclear(res);
    *out = entries;
    *out_count = (size_t)rows;
    return SC_OK;
#else
    (void)ctx;(void)alloc;(void)query;(void)query_len;(void)limit;
    (void)session_id;(void)session_id_len;
    *out = NULL;
    *out_count = 0;
    return SC_ERR_NOT_SUPPORTED;
#endif
}

static sc_error_t impl_get(void *ctx, sc_allocator_t *alloc,
    const char *key, size_t key_len,
    sc_memory_entry_t *out, bool *found) {
#if defined(SC_IS_TEST) && SC_IS_TEST
    sc_postgres_memory_t *self = (sc_postgres_memory_t *)ctx;
    mock_entry_t *e = mock_find_by_key(self, key, key_len);
    *found = false;
    memset(out, 0, sizeof(*out));

    if (!e) return SC_OK;

    *found = true;
    out->id = out->key = sc_strndup(alloc, e->key, strlen(e->key));
    out->key_len = strlen(e->key);
    out->id_len = out->key_len;
    out->content = sc_strndup(alloc, e->content, strlen(e->content));
    out->content_len = strlen(e->content);
    out->timestamp = sc_sprintf(alloc, "0");
    out->timestamp_len = out->timestamp ? strlen(out->timestamp) : 0;
    if (e->session_id) {
        out->session_id = sc_strndup(alloc, e->session_id, strlen(e->session_id));
        out->session_id_len = strlen(e->session_id);
    }
    out->category.tag = SC_MEMORY_CATEGORY_CUSTOM;
    out->category.data.custom.name = sc_strndup(alloc, e->category, strlen(e->category));
    out->category.data.custom.name_len = strlen(e->category);
    return SC_OK;

#elif defined(SC_ENABLE_POSTGRES)
    sc_postgres_memory_t *self = (sc_postgres_memory_t *)ctx;
    *found = false;
    memset(out, 0, sizeof(*out));
    if (!self->conn || PQstatus(self->conn) != CONNECTION_OK)
        return SC_ERR_MEMORY_BACKEND;

    char sql[384];
    (void)snprintf(sql, sizeof(sql),
        "SELECT key, content, category, session_id, updated_at FROM %s.%s WHERE key = $1",
        self->schema_q, self->table_q);
    const char *params[] = { key };
    int param_lens[] = { (int)key_len };

    PGresult *res = PQexecParams(self->conn, sql, 1, NULL,
        (const char *const *)params, param_lens, NULL, 0);
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (res) PQclear(res);
        return SC_ERR_MEMORY_RECALL;
    }
    if (PQntuples(res) == 0) {
        PQclear(res);
        return SC_OK;
    }

    const char *k = PQgetvalue(res, 0, 0);
    const char *c = PQgetvalue(res, 0, 1);
    const char *cat = PQgetvalue(res, 0, 2);
    const char *sid = PQgetvalue(res, 0, 3);
    const char *ts = PQgetvalue(res, 0, 4);
    size_t klen = k ? strlen(k) : 0;
    size_t clen = c ? strlen(c) : 0;
    out->key = sc_strndup(alloc, k, klen);
    out->key_len = klen;
    out->id = out->key;
    out->id_len = klen;
    out->content = sc_strndup(alloc, c, clen);
    out->content_len = clen;
    out->timestamp = ts ? sc_strndup(alloc, ts, strlen(ts)) : NULL;
    out->timestamp_len = out->timestamp ? strlen(out->timestamp) : 0;
    out->session_id = sid && sid[0] ? sc_strndup(alloc, sid, strlen(sid)) : NULL;
    out->session_id_len = out->session_id ? strlen(out->session_id) : 0;
    out->category.tag = SC_MEMORY_CATEGORY_CUSTOM;
    if (cat && cat[0]) {
        out->category.data.custom.name = sc_strndup(alloc, cat, strlen(cat));
        out->category.data.custom.name_len = strlen(cat);
    }
    PQclear(res);
    *found = true;
    return SC_OK;
#else
    (void)ctx;(void)alloc;(void)key;(void)key_len;(void)out;
    *found = false;
    return SC_ERR_NOT_SUPPORTED;
#endif
}

static sc_error_t impl_list(void *ctx, sc_allocator_t *alloc,
    const sc_memory_category_t *category,
    const char *session_id, size_t session_id_len,
    sc_memory_entry_t **out, size_t *out_count) {
#if defined(SC_IS_TEST) && SC_IS_TEST
    sc_postgres_memory_t *self = (sc_postgres_memory_t *)ctx;
    const char *cat_filter = category ? category_to_string(category) : NULL;
    *out = NULL;
    *out_count = 0;

    size_t cap = 0;
    size_t n = 0;
    sc_memory_entry_t *results = NULL;

    for (size_t i = 0; i < self->entry_count; i++) {
        mock_entry_t *e = &self->entries[i];
        if (!e->key) continue;
        if (cat_filter && (!e->category || strcmp(e->category, cat_filter) != 0))
            continue;
        if (session_id && session_id_len > 0) {
            if (!e->session_id) continue;
            if (strlen(e->session_id) != session_id_len ||
                memcmp(e->session_id, session_id, session_id_len) != 0)
                continue;
        }

        if (n >= cap) {
            size_t new_cap = cap ? cap * 2 : 4;
            sc_memory_entry_t *tmp = (sc_memory_entry_t *)alloc->realloc(
                alloc->ctx, results, cap * sizeof(sc_memory_entry_t),
                new_cap * sizeof(sc_memory_entry_t));
            if (!tmp) {
                for (size_t j = 0; j < n; j++) sc_memory_entry_free_fields(alloc, &results[j]);
                if (results) alloc->free(alloc->ctx, results, cap * sizeof(sc_memory_entry_t));
                return SC_ERR_OUT_OF_MEMORY;
            }
            results = tmp;
            cap = new_cap;
        }

        sc_memory_entry_t *r = &results[n];
        memset(r, 0, sizeof(*r));
        r->id = r->key = sc_strndup(alloc, e->key, strlen(e->key));
        r->key_len = strlen(e->key);
        r->id_len = r->key_len;
        r->content = sc_strndup(alloc, e->content, strlen(e->content));
        r->content_len = strlen(e->content);
        r->timestamp = sc_sprintf(alloc, "0");
        r->timestamp_len = r->timestamp ? strlen(r->timestamp) : 0;
        if (e->session_id) {
            r->session_id = sc_strndup(alloc, e->session_id, strlen(e->session_id));
            r->session_id_len = strlen(e->session_id);
        }
        r->category.tag = SC_MEMORY_CATEGORY_CUSTOM;
        r->category.data.custom.name = sc_strndup(alloc, e->category, strlen(e->category));
        r->category.data.custom.name_len = strlen(e->category);
        n++;
    }

    *out = results;
    *out_count = n;
    return SC_OK;

#elif defined(SC_ENABLE_POSTGRES)
    sc_postgres_memory_t *self = (sc_postgres_memory_t *)ctx;
    if (!self->conn || PQstatus(self->conn) != CONNECTION_OK) {
        *out = NULL;
        *out_count = 0;
        return SC_ERR_MEMORY_BACKEND;
    }

    const char *cat_str = category ? category_to_string(category) : NULL;
    char sql[512];
    int sn;
    if (cat_str && session_id && session_id_len > 0)
        sn = (int)snprintf(sql, sizeof(sql),
            "SELECT key, content, category, session_id, updated_at FROM %s.%s WHERE category = $1 AND session_id = $2 ORDER BY updated_at DESC",
            self->schema_q, self->table_q);
    else if (cat_str)
        sn = (int)snprintf(sql, sizeof(sql),
            "SELECT key, content, category, session_id, updated_at FROM %s.%s WHERE category = $1 ORDER BY updated_at DESC",
            self->schema_q, self->table_q);
    else if (session_id && session_id_len > 0)
        sn = (int)snprintf(sql, sizeof(sql),
            "SELECT key, content, category, session_id, updated_at FROM %s.%s WHERE session_id = $1 ORDER BY updated_at DESC",
            self->schema_q, self->table_q);
    else
        sn = (int)snprintf(sql, sizeof(sql),
            "SELECT key, content, category, session_id, updated_at FROM %s.%s ORDER BY updated_at DESC",
            self->schema_q, self->table_q);

    if (sn < 0 || (size_t)sn >= sizeof(sql)) {
        *out = NULL;
        *out_count = 0;
        return SC_ERR_INVALID_ARGUMENT;
    }

    PGresult *res = NULL;
    if (cat_str && session_id && session_id_len > 0) {
        const char *params[] = { cat_str, session_id };
        int param_lens[] = { (int)strlen(cat_str), (int)session_id_len };
        res = PQexecParams(self->conn, sql, 2, NULL,
            (const char *const *)params, param_lens, NULL, 0);
    } else if (cat_str) {
        const char *params[] = { cat_str };
        int param_lens[] = { (int)strlen(cat_str) };
        res = PQexecParams(self->conn, sql, 1, NULL,
            (const char *const *)params, param_lens, NULL, 0);
    } else if (session_id && session_id_len > 0) {
        const char *params[] = { session_id };
        int param_lens[] = { (int)session_id_len };
        res = PQexecParams(self->conn, sql, 1, NULL,
            (const char *const *)params, param_lens, NULL, 0);
    } else {
        res = PQexec(self->conn, sql);
    }

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (res) PQclear(res);
        *out = NULL;
        *out_count = 0;
        return SC_ERR_MEMORY_RECALL;
    }

    int rows = PQntuples(res);
    if (rows == 0) {
        PQclear(res);
        *out = NULL;
        *out_count = 0;
        return SC_OK;
    }

    sc_memory_entry_t *entries = (sc_memory_entry_t *)alloc->alloc(
        alloc->ctx, (size_t)rows * sizeof(sc_memory_entry_t));
    if (!entries) {
        PQclear(res);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memset(entries, 0, (size_t)rows * sizeof(sc_memory_entry_t));

    for (int i = 0; i < rows; i++) {
        sc_memory_entry_t *e = &entries[i];
        const char *k = PQgetvalue(res, i, 0);
        const char *c = PQgetvalue(res, i, 1);
        const char *cat = PQgetvalue(res, i, 2);
        const char *sid = PQgetvalue(res, i, 3);
        const char *ts = PQgetvalue(res, i, 4);
        size_t klen = k ? strlen(k) : 0;
        size_t clen = c ? strlen(c) : 0;
        e->key = sc_strndup(alloc, k, klen);
        e->key_len = klen;
        e->id = e->key;
        e->id_len = klen;
        e->content = sc_strndup(alloc, c, clen);
        e->content_len = clen;
        e->timestamp = ts ? sc_strndup(alloc, ts, strlen(ts)) : NULL;
        e->timestamp_len = e->timestamp ? strlen(e->timestamp) : 0;
        e->session_id = sid && sid[0] ? sc_strndup(alloc, sid, strlen(sid)) : NULL;
        e->session_id_len = e->session_id ? strlen(e->session_id) : 0;
        e->category.tag = SC_MEMORY_CATEGORY_CUSTOM;
        if (cat && cat[0]) {
            e->category.data.custom.name = sc_strndup(alloc, cat, strlen(cat));
            e->category.data.custom.name_len = strlen(cat);
        }
    }
    PQclear(res);
    *out = entries;
    *out_count = (size_t)rows;
    return SC_OK;
#else
    (void)ctx;(void)alloc;(void)category;(void)session_id;(void)session_id_len;
    *out = NULL;
    *out_count = 0;
    return SC_ERR_NOT_SUPPORTED;
#endif
}

static sc_error_t impl_forget(void *ctx,
    const char *key, size_t key_len,
    bool *deleted) {
#if defined(SC_IS_TEST) && SC_IS_TEST
    sc_postgres_memory_t *self = (sc_postgres_memory_t *)ctx;
    *deleted = false;
    for (size_t i = 0; i < self->entry_count; i++) {
        mock_entry_t *e = &self->entries[i];
        if (e->key && strlen(e->key) == key_len && memcmp(e->key, key, key_len) == 0) {
            mock_free_entry(self->alloc, e);
            memmove(&self->entries[i], &self->entries[i + 1],
                (self->entry_count - i - 1) * sizeof(mock_entry_t));
            memset(&self->entries[self->entry_count - 1], 0, sizeof(mock_entry_t));
            self->entry_count--;
            *deleted = true;
            return SC_OK;
        }
    }
    return SC_OK;

#elif defined(SC_ENABLE_POSTGRES)
    sc_postgres_memory_t *self = (sc_postgres_memory_t *)ctx;
    *deleted = false;
    if (!self->conn || PQstatus(self->conn) != CONNECTION_OK)
        return SC_ERR_MEMORY_BACKEND;

    char sql[256];
    (void)snprintf(sql, sizeof(sql), "DELETE FROM %s.%s WHERE key = $1", self->schema_q, self->table_q);
    const char *params[] = { key };
    int param_lens[] = { (int)key_len };

    PGresult *res = PQexecParams(self->conn, sql, 1, NULL,
        (const char *const *)params, param_lens, NULL, 0);
    if (!res || (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK)) {
        if (res) PQclear(res);
        return SC_ERR_MEMORY_STORE;
    }
    *deleted = (PQcmdTuples(res) && atoi(PQcmdTuples(res)) > 0);
    PQclear(res);
    return SC_OK;
#else
    (void)ctx;(void)key;(void)key_len;
    *deleted = false;
    return SC_ERR_NOT_SUPPORTED;
#endif
}

static sc_error_t impl_count(void *ctx, size_t *out) {
#if defined(SC_IS_TEST) && SC_IS_TEST
    sc_postgres_memory_t *self = (sc_postgres_memory_t *)ctx;
    *out = self->entry_count;
    return SC_OK;

#elif defined(SC_ENABLE_POSTGRES)
    sc_postgres_memory_t *self = (sc_postgres_memory_t *)ctx;
    if (!self->conn || PQstatus(self->conn) != CONNECTION_OK) {
        *out = 0;
        return SC_ERR_MEMORY_BACKEND;
    }
    char sql[256];
    (void)snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s.%s", self->schema_q, self->table_q);
    PGresult *res = PQexec(self->conn, sql);
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (res) PQclear(res);
        *out = 0;
        return SC_ERR_MEMORY_RECALL;
    }
    const char *v = PQgetvalue(res, 0, 0);
    *out = v ? (size_t)atoll(v) : 0;
    PQclear(res);
    return SC_OK;
#else
    (void)ctx;
    *out = 0;
    return SC_ERR_NOT_SUPPORTED;
#endif
}

static bool impl_health_check(void *ctx) {
#if defined(SC_IS_TEST) && SC_IS_TEST
    (void)ctx;
    return true;
#elif defined(SC_ENABLE_POSTGRES)
    sc_postgres_memory_t *self = (sc_postgres_memory_t *)ctx;
    return self->conn && PQstatus(self->conn) == CONNECTION_OK;
#else
    (void)ctx;
    return false;
#endif
}

static void impl_deinit(void *ctx) {
    sc_postgres_memory_t *self = (sc_postgres_memory_t *)ctx;
    if (!self || !self->alloc) return;

#if defined(SC_IS_TEST) && SC_IS_TEST
    for (size_t i = 0; i < self->entry_count; i++)
        mock_free_entry(self->alloc, &self->entries[i]);
    self->entry_count = 0;
#elif defined(SC_ENABLE_POSTGRES)
    if (self->conn) {
        PQfinish(self->conn);
        self->conn = NULL;
    }
    if (self->schema_q) {
        self->alloc->free(self->alloc->ctx, self->schema_q, strlen(self->schema_q) + 1);
        self->schema_q = NULL;
    }
    if (self->table_q) {
        self->alloc->free(self->alloc->ctx, self->table_q, strlen(self->table_q) + 1);
        self->table_q = NULL;
    }
#endif
    self->alloc->free(self->alloc->ctx, self, sizeof(sc_postgres_memory_t));
}

static const sc_memory_vtable_t postgres_vtable = {
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

static bool pg_is_safe_identifier(const char *id) {
    if (!id || !id[0]) return false;
    for (const char *p = id; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) return false;
    }
    return true;
}

sc_memory_t sc_postgres_memory_create(sc_allocator_t *alloc,
    const char *url, const char *schema, const char *table) {
    if (!alloc) return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    const char *s = schema ? schema : "public";
    const char *t = table ? table : "memories";
    if (!pg_is_safe_identifier(s) || !pg_is_safe_identifier(t))
        return (sc_memory_t){ .ctx = NULL, .vtable = NULL };

#if defined(SC_IS_TEST) && SC_IS_TEST
    (void)url;(void)schema;(void)table;
    sc_postgres_memory_t *self = (sc_postgres_memory_t *)alloc->alloc(
        alloc->ctx, sizeof(sc_postgres_memory_t));
    if (!self) return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    memset(self, 0, sizeof(sc_postgres_memory_t));
    self->alloc = alloc;
    return (sc_memory_t){ .ctx = self, .vtable = &postgres_vtable };

#elif defined(SC_ENABLE_POSTGRES)
    sc_postgres_memory_t *self = (sc_postgres_memory_t *)alloc->alloc(
        alloc->ctx, sizeof(sc_postgres_memory_t));
    if (!self) return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    memset(self, 0, sizeof(sc_postgres_memory_t));
    self->alloc = alloc;

    self->conn = PQconnectdb(url);
    if (!self->conn || PQstatus(self->conn) != CONNECTION_OK) {
        if (self->conn) PQfinish(self->conn);
        alloc->free(alloc->ctx, self, sizeof(sc_postgres_memory_t));
        return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    }

    self->schema_q = sc_strdup(alloc, schema);
    self->table_q = sc_strdup(alloc, table);
    if (!self->schema_q || !self->table_q) {
        if (self->schema_q) alloc->free(alloc->ctx, self->schema_q, strlen(self->schema_q) + 1);
        if (self->table_q) alloc->free(alloc->ctx, self->table_q, strlen(self->table_q) + 1);
        PQfinish(self->conn);
        alloc->free(alloc->ctx, self, sizeof(sc_postgres_memory_t));
        return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    }

    /* CREATE TABLE IF NOT EXISTS on connect */
    {
        char ddl[512];
        int n = (int)snprintf(ddl, sizeof(ddl),
            "CREATE TABLE IF NOT EXISTS %s.%s ("
            "key TEXT PRIMARY KEY, content TEXT NOT NULL, category TEXT NOT NULL DEFAULT 'core', "
            "session_id TEXT, created_at TIMESTAMPTZ DEFAULT NOW(), updated_at TIMESTAMPTZ DEFAULT NOW()",
            self->schema_q, self->table_q);
        if (n > 0 && (size_t)n < sizeof(ddl)) {
            PGresult *mres = PQexec(self->conn, ddl);
            if (mres) {
                if (PQresultStatus(mres) != PGRES_COMMAND_OK && PQresultStatus(mres) != PGRES_TUPLES_OK) {
                    PQclear(mres);
                } else {
                    PQclear(mres);
                }
            }
        }
    }

    return (sc_memory_t){ .ctx = self, .vtable = &postgres_vtable };
#else
    (void)url;(void)schema;(void)table;
    sc_postgres_memory_t *self = (sc_postgres_memory_t *)alloc->alloc(
        alloc->ctx, sizeof(sc_postgres_memory_t));
    if (!self) return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    memset(self, 0, sizeof(sc_postgres_memory_t));
    self->alloc = alloc;
    return (sc_memory_t){ .ctx = self, .vtable = &postgres_vtable };
#endif
}
