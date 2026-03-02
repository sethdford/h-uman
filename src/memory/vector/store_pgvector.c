#include "seaclaw/memory/vector/store_pgvector.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <stdlib.h>

#if defined(SC_ENABLE_POSTGRES)
#include <libpq-fe.h>

static bool is_safe_identifier(const char *id) {
    if (!id || !id[0]) return false;
    for (const char *p = id; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) return false;
    }
    return true;
}

typedef struct pgvector_ctx {
    sc_allocator_t *alloc;
    PGconn *conn;
    char *connection_url;
    char *table_name;
    size_t dimensions;
} pgvector_ctx_t;

static sc_error_t pgvector_upsert_impl(void *ctx, sc_allocator_t *alloc,
    const char *id, size_t id_len,
    const float *embedding, size_t dims,
    const char *metadata, size_t metadata_len) {
    (void)metadata;
    (void)metadata_len;
    pgvector_ctx_t *p = (pgvector_ctx_t *)ctx;
    if (!p || !p->conn || !embedding) return SC_ERR_INVALID_ARGUMENT;

    char *key_z = sc_strndup(alloc, id, id_len);
    if (!key_z) return SC_ERR_OUT_OF_MEMORY;

    char vec_buf[4096];
    size_t pos = 0;
    vec_buf[pos++] = '[';
    for (size_t i = 0; i < dims && pos < sizeof(vec_buf) - 32; i++) {
        if (i > 0) vec_buf[pos++] = ',';
        pos += (size_t)snprintf(vec_buf + pos, sizeof(vec_buf) - pos, "%f", embedding[i]);
    }
    vec_buf[pos++] = ']';
    vec_buf[pos] = '\0';

    char sql[8192];
    int slen = snprintf(sql, sizeof(sql),
        "INSERT INTO %s (key, embedding, updated_at) VALUES ($1, $2::vector, now()) "
        "ON CONFLICT (key) DO UPDATE SET embedding = $2::vector, updated_at = now()",
        p->table_name);
    if (slen >= (int)sizeof(sql) || slen < 0) {
        alloc->free(alloc->ctx, key_z, id_len + 1);
        return SC_ERR_INVALID_ARGUMENT;
    }

    const char *params[2] = { key_z, vec_buf };
    PGresult *res = PQexecParams(p->conn, sql, 2, NULL, params, NULL, NULL, 0);
    alloc->free(alloc->ctx, key_z, id_len + 1);
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return SC_ERR_MEMORY_BACKEND;
    }
    PQclear(res);
    return SC_OK;
}

static sc_error_t pgvector_search_impl(void *ctx, sc_allocator_t *alloc,
    const float *query_embedding, size_t dims,
    size_t limit,
    sc_vector_search_result_t **results, size_t *result_count) {
    pgvector_ctx_t *p = (pgvector_ctx_t *)ctx;
    if (!p || !p->conn || !query_embedding || !results || !result_count)
        return SC_ERR_INVALID_ARGUMENT;

    char vec_buf[4096];
    size_t pos = 0;
    vec_buf[pos++] = '[';
    for (size_t i = 0; i < dims && pos < sizeof(vec_buf) - 32; i++) {
        if (i > 0) vec_buf[pos++] = ',';
        pos += (size_t)snprintf(vec_buf + pos, sizeof(vec_buf) - pos, "%f", query_embedding[i]);
    }
    vec_buf[pos++] = ']';
    vec_buf[pos] = '\0';

    size_t lim = limit > 0 ? limit : 10;
    char sql[8192];
    int slen = snprintf(sql, sizeof(sql),
        "SELECT key, 1 - (embedding <=> $1::vector) AS sim FROM %s "
        "ORDER BY embedding <=> $1::vector LIMIT %zu",
        p->table_name, lim);
    if (slen >= (int)sizeof(sql) || slen < 0) return SC_ERR_INVALID_ARGUMENT;

    const char *params[1] = { vec_buf };
    PGresult *res = PQexecParams(p->conn, sql, 1, NULL, params, NULL, NULL, 0);
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (res) PQclear(res);
        *results = NULL;
        *result_count = 0;
        return SC_ERR_MEMORY_BACKEND;
    }

    int nrows = PQntuples(res);
    if (nrows == 0) {
        PQclear(res);
        *results = NULL;
        *result_count = 0;
        return SC_OK;
    }
    sc_vector_search_result_t *arr = (sc_vector_search_result_t *)alloc->alloc(alloc->ctx,
        (size_t)nrows * sizeof(sc_vector_search_result_t));
    if (!arr) {
        PQclear(res);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memset(arr, 0, (size_t)nrows * sizeof(sc_vector_search_result_t));

    for (int i = 0; i < nrows; i++) {
        const char *key = PQgetvalue(res, i, 0);
        const char *sim = PQgetvalue(res, i, 1);
        if (key) arr[i].id = sc_strdup(alloc, key);
        arr[i].score = (float)atof(sim ? sim : "0");
    }
    PQclear(res);
    *results = arr;
    *result_count = (size_t)nrows;
    return SC_OK;
}

static sc_error_t pgvector_delete_impl(void *ctx, sc_allocator_t *alloc,
    const char *id, size_t id_len) {
    pgvector_ctx_t *p = (pgvector_ctx_t *)ctx;
    if (!p || !p->conn) return SC_ERR_INVALID_ARGUMENT;

    char *key_z = sc_strndup(alloc, id, id_len);
    if (!key_z) return SC_ERR_OUT_OF_MEMORY;

    char sql[512];
    snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE key = $1", p->table_name);
    const char *params[1] = { key_z };
    PGresult *res = PQexecParams(p->conn, sql, 1, NULL, params, NULL, NULL, 0);
    alloc->free(alloc->ctx, key_z, id_len + 1);
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return SC_ERR_MEMORY_BACKEND;
    }
    PQclear(res);
    return SC_OK;
}

static size_t pgvector_count_impl(void *ctx) {
    pgvector_ctx_t *p = (pgvector_ctx_t *)ctx;
    if (!p || !p->conn) return 0;

    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", p->table_name);
    PGresult *res = PQexec(p->conn, sql);
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (res) PQclear(res);
        return 0;
    }
    const char *val = PQgetvalue(res, 0, 0);
    size_t n = val ? (size_t)atoll(val) : 0;
    PQclear(res);
    return n;
}

static void pgvector_deinit_impl(void *ctx, sc_allocator_t *alloc) {
    pgvector_ctx_t *p = (pgvector_ctx_t *)ctx;
    if (!p || !alloc) return;
    if (p->conn) PQfinish(p->conn);
    if (p->connection_url) alloc->free(alloc->ctx, p->connection_url, strlen(p->connection_url) + 1);
    if (p->table_name) alloc->free(alloc->ctx, p->table_name, strlen(p->table_name) + 1);
    alloc->free(alloc->ctx, p, sizeof(pgvector_ctx_t));
}

static const sc_vector_store_vtable_t pgvector_vtable = {
    .upsert = pgvector_upsert_impl,
    .search = pgvector_search_impl,
    .delete = pgvector_delete_impl,
    .count = pgvector_count_impl,
    .deinit = pgvector_deinit_impl,
};

#else

typedef struct pgvector_ctx {
    sc_allocator_t *alloc;
    void *conn;
    char *connection_url;
    char *table_name;
    size_t dimensions;
} pgvector_ctx_t;

static sc_error_t pgvector_upsert_impl(void *ctx, sc_allocator_t *alloc,
    const char *id, size_t id_len,
    const float *embedding, size_t dims,
    const char *metadata, size_t metadata_len) {
    (void)ctx;(void)alloc;(void)id;(void)id_len;(void)embedding;(void)dims;(void)metadata;(void)metadata_len;
    return SC_ERR_NOT_SUPPORTED;
}

static sc_error_t pgvector_search_impl(void *ctx, sc_allocator_t *alloc,
    const float *query_embedding, size_t dims,
    size_t limit,
    sc_vector_search_result_t **results, size_t *result_count) {
    (void)ctx;(void)alloc;(void)query_embedding;(void)dims;(void)limit;
    *results = NULL;
    *result_count = 0;
    return SC_ERR_NOT_SUPPORTED;
}

static sc_error_t pgvector_delete_impl(void *ctx, sc_allocator_t *alloc,
    const char *id, size_t id_len) {
    (void)ctx;(void)alloc;(void)id;(void)id_len;
    return SC_ERR_NOT_SUPPORTED;
}

static size_t pgvector_count_impl(void *ctx) {
    (void)ctx;
    return 0;
}

static void pgvector_deinit_impl(void *ctx, sc_allocator_t *alloc) {
    pgvector_ctx_t *p = (pgvector_ctx_t *)ctx;
    if (!p || !alloc) return;
    if (p->connection_url) alloc->free(alloc->ctx, p->connection_url, strlen(p->connection_url) + 1);
    if (p->table_name) alloc->free(alloc->ctx, p->table_name, strlen(p->table_name) + 1);
    alloc->free(alloc->ctx, p, sizeof(pgvector_ctx_t));
}

static const sc_vector_store_vtable_t pgvector_vtable = {
    .upsert = pgvector_upsert_impl,
    .search = pgvector_search_impl,
    .delete = pgvector_delete_impl,
    .count = pgvector_count_impl,
    .deinit = pgvector_deinit_impl,
};
#endif

sc_vector_store_t sc_vector_store_pgvector_create(sc_allocator_t *alloc,
    const sc_pgvector_config_t *config) {
    sc_vector_store_t s = { .ctx = NULL, .vtable = &pgvector_vtable };
    if (!alloc || !config) return s;

    const char *table_val = (config->table_name && config->table_name[0])
        ? config->table_name : "memory_vectors";
#if defined(SC_ENABLE_POSTGRES)
    if (!is_safe_identifier(table_val)) return s;
#endif

    pgvector_ctx_t *p = (pgvector_ctx_t *)alloc->alloc(alloc->ctx, sizeof(pgvector_ctx_t));
    if (!p) return s;
    memset(p, 0, sizeof(*p));
    p->alloc = alloc;
    p->connection_url = config->connection_url ? sc_strdup(alloc, config->connection_url) : NULL;
    p->table_name = sc_strdup(alloc, table_val);
    p->dimensions = config->dimensions;

#if defined(SC_ENABLE_POSTGRES)
    if (p->connection_url) {
        p->conn = PQconnectdb(p->connection_url);
        if (!p->conn || PQstatus(p->conn) != CONNECTION_OK) {
            if (p->conn) PQfinish(p->conn);
            p->conn = NULL;
        }
    }
#endif

    s.ctx = p;
    return s;
}
