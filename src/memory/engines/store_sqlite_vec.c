/* store_sqlite_vec.c — hu_vector_store_t over sqlite-vec's vec0 virtual table.
 *
 * Lives in engines/ because it speaks SQL to the memory DB directly (the
 * sqlite-includer ratchet exempts this directory). Phase 2 of
 * docs/plans/2026-08-02-semantic-retrieval/spec.md. */
#include "human/memory/vector/store_sqlite_vec.h"

#include "human/core/log.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

#include "sqlite-vec.h"

typedef struct vec_store_ctx {
    hu_allocator_t *alloc;
    sqlite3 *db;
    size_t dim;
} vec_store_ctx_t;

bool hu_sqlite_vec_register(void) {
    /* sqlite3_auto_extension is deprecated on Apple platforms (process-global
     * auto-extensions unsupported), so the extension is loaded PER CONNECTION
     * in hu_vector_store_sqlite_vec_create via sqlite3_vec_init(db, ...).
     * Kept as an idempotent no-op for callers that expect a registration step. */
    return true;
}

static bool ensure_schema(sqlite3 *db, size_t dim) {
    char sql[256];
    int n = snprintf(sql, sizeof(sql),
                     "CREATE VIRTUAL TABLE IF NOT EXISTS memories_vec USING vec0("
                     "id TEXT PRIMARY KEY, embedding float[%zu]);",
                     dim);
    if (n <= 0 || (size_t)n >= sizeof(sql))
        return false;
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        hu_log_error("memory.vec", NULL, "vec0 schema failed: %s", err ? err : "?");
        sqlite3_free(err);
        return false;
    }
    if (sqlite3_exec(db,
                     "CREATE TABLE IF NOT EXISTS memories_vec_meta("
                     "id TEXT PRIMARY KEY, content TEXT NOT NULL, dim INTEGER NOT NULL);",
                     NULL, NULL, &err) != SQLITE_OK) {
        hu_log_error("memory.vec", NULL, "vec meta schema failed: %s", err ? err : "?");
        sqlite3_free(err);
        return false;
    }
    return true;
}

static hu_error_t insert_impl(void *vctx, hu_allocator_t *alloc, const char *id, size_t id_len,
                              const hu_embedding_t *embedding, const char *content,
                              size_t content_len) {
    vec_store_ctx_t *ctx = (vec_store_ctx_t *)vctx;
    (void)alloc;
    if (!ctx || !id || id_len == 0 || !embedding || !embedding->values)
        return HU_ERR_INVALID_ARGUMENT;
    if (embedding->dim != ctx->dim)
        return HU_ERR_INVALID_ARGUMENT; /* a wrong-dim vector is a different embedder */
    /* vec0 has no upsert: delete-then-insert inside one transaction. */
    sqlite3_exec(ctx->db, "SAVEPOINT vec_upsert;", NULL, NULL, NULL);
    sqlite3_stmt *st = NULL;
    hu_error_t err = HU_ERR_IO;
    if (sqlite3_prepare_v2(ctx->db, "DELETE FROM memories_vec WHERE id = ?;", -1, &st, NULL) ==
        SQLITE_OK) {
        sqlite3_bind_text(st, 1, id, (int)id_len, SQLITE_STATIC);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(ctx->db, "INSERT INTO memories_vec(id, embedding) VALUES(?, ?);", -1,
                           &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, id, (int)id_len, SQLITE_STATIC);
        sqlite3_bind_blob(st, 2, embedding->values, (int)(embedding->dim * sizeof(float)),
                          SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_DONE)
            err = HU_OK;
    }
    sqlite3_finalize(st);
    st = NULL;
    if (err == HU_OK) {
        err = HU_ERR_IO;
        if (sqlite3_prepare_v2(ctx->db,
                               "INSERT OR REPLACE INTO memories_vec_meta(id, content, dim) "
                               "VALUES(?, ?, ?);",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, id, (int)id_len, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, content ? content : "", content ? (int)content_len : 0,
                              SQLITE_STATIC);
            sqlite3_bind_int64(st, 3, (sqlite3_int64)embedding->dim);
            if (sqlite3_step(st) == SQLITE_DONE)
                err = HU_OK;
        }
        sqlite3_finalize(st);
    }
    sqlite3_exec(ctx->db,
                 err == HU_OK ? "RELEASE vec_upsert;"
                              : "ROLLBACK TO vec_upsert; RELEASE vec_upsert;",
                 NULL, NULL, NULL);
    return err;
}

static hu_error_t search_impl(void *vctx, hu_allocator_t *alloc, const hu_embedding_t *query,
                              size_t limit, hu_vector_entry_t **out, size_t *out_count) {
    vec_store_ctx_t *ctx = (vec_store_ctx_t *)vctx;
    if (out)
        *out = NULL;
    if (out_count)
        *out_count = 0;
    if (!ctx || !alloc || !query || !query->values || query->dim != ctx->dim || limit == 0 ||
        !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    if (limit > 256)
        limit = 256;
    sqlite3_stmt *st = NULL;
    /* Exact KNN. vec0's distance is L2 on the stored vectors; our vectors are
     * L2-normalised so score = 1 - d^2/2 is cosine similarity. */
    const char *sql = "SELECT v.id, v.distance, m.content FROM memories_vec v "
                      "LEFT JOIN memories_vec_meta m ON m.id = v.id "
                      "WHERE v.embedding MATCH ? AND k = ? ORDER BY v.distance;";
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_blob(st, 1, query->values, (int)(query->dim * sizeof(float)), SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)limit);
    hu_vector_entry_t *res = (hu_vector_entry_t *)alloc->alloc(alloc->ctx, limit * sizeof(*res));
    if (!res) {
        sqlite3_finalize(st);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(res, 0, limit * sizeof(*res));
    size_t n = 0;
    hu_error_t err = HU_OK;
    while (n < limit && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *id = sqlite3_column_text(st, 0);
        double d = sqlite3_column_double(st, 1);
        const unsigned char *content = sqlite3_column_text(st, 2);
        size_t id_len = id ? strlen((const char *)id) : 0;
        size_t c_len = content ? strlen((const char *)content) : 0;
        char *idc = (char *)alloc->alloc(alloc->ctx, id_len + 1);
        char *cc = (char *)alloc->alloc(alloc->ctx, c_len + 1);
        if (!idc || !cc) {
            if (idc)
                alloc->free(alloc->ctx, idc, id_len + 1);
            if (cc)
                alloc->free(alloc->ctx, cc, c_len + 1);
            err = HU_ERR_OUT_OF_MEMORY;
            break;
        }
        memcpy(idc, id ? (const char *)id : "", id_len);
        idc[id_len] = '\0';
        memcpy(cc, content ? (const char *)content : "", c_len);
        cc[c_len] = '\0';
        res[n].id = idc;
        res[n].id_len = id_len;
        res[n].content = cc;
        res[n].content_len = c_len;
        res[n].embedding.values = NULL;
        res[n].embedding.dim = 0;
        double sim = 1.0 - (d * d) / 2.0;
        res[n].score = (float)(sim < -1.0 ? -1.0 : (sim > 1.0 ? 1.0 : sim));
        n++;
    }
    sqlite3_finalize(st);
    if (err != HU_OK) {
        for (size_t i = 0; i < n; i++) {
            alloc->free(alloc->ctx, (void *)res[i].id, res[i].id_len + 1);
            alloc->free(alloc->ctx, (void *)res[i].content, res[i].content_len + 1);
        }
        alloc->free(alloc->ctx, res, limit * sizeof(*res));
        return err;
    }
    *out = res;
    *out_count = n;
    return HU_OK;
}

static hu_error_t remove_impl(void *vctx, const char *id, size_t id_len) {
    vec_store_ctx_t *ctx = (vec_store_ctx_t *)vctx;
    if (!ctx || !id || id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *st = NULL;
    hu_error_t err = HU_ERR_IO;
    if (sqlite3_prepare_v2(ctx->db, "DELETE FROM memories_vec WHERE id = ?;", -1, &st, NULL) ==
        SQLITE_OK) {
        sqlite3_bind_text(st, 1, id, (int)id_len, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_DONE)
            err = HU_OK;
    }
    sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(ctx->db, "DELETE FROM memories_vec_meta WHERE id = ?;", -1, &st, NULL) ==
        SQLITE_OK) {
        sqlite3_bind_text(st, 1, id, (int)id_len, SQLITE_STATIC);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    return err;
}

static size_t count_impl(void *vctx) {
    vec_store_ctx_t *ctx = (vec_store_ctx_t *)vctx;
    if (!ctx)
        return 0;
    sqlite3_stmt *st = NULL;
    size_t n = 0;
    if (sqlite3_prepare_v2(ctx->db, "SELECT COUNT(*) FROM memories_vec_meta;", -1, &st, NULL) ==
        SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            n = (size_t)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

static void deinit_impl(void *vctx, hu_allocator_t *alloc) {
    if (vctx && alloc)
        alloc->free(alloc->ctx, vctx, sizeof(vec_store_ctx_t)); /* db is not ours */
}

static const hu_vector_store_vtable_t vec_vtable = {
    .insert = insert_impl,
    .search = search_impl,
    .remove = remove_impl,
    .count = count_impl,
    .deinit = deinit_impl,
};

hu_vector_store_t hu_vector_store_sqlite_vec_create(hu_allocator_t *alloc, struct sqlite3 *db,
                                                    size_t dim) {
    hu_vector_store_t vs = {.ctx = NULL, .vtable = &vec_vtable};
    if (!alloc || !db || dim == 0)
        return vs;
    /* The extension must already be loaded on THIS connection. The engine
     * registers the auto-extension before opening; a connection opened
     * earlier (tests) can still load it explicitly here. */
    char *err = NULL;
    if (sqlite3_vec_init(db, &err, NULL) != SQLITE_OK) {
        /* Already loaded on this connection is the common benign case. */
        sqlite3_free(err);
    }
    if (!ensure_schema(db, dim))
        return vs;
    vec_store_ctx_t *ctx = (vec_store_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*ctx));
    if (!ctx)
        return vs;
    ctx->alloc = alloc;
    ctx->db = db;
    ctx->dim = dim;
    vs.ctx = ctx;
    return vs;
}

#else /* !HU_ENABLE_SQLITE */

bool hu_sqlite_vec_register(void) {
    return false;
}

hu_vector_store_t hu_vector_store_sqlite_vec_create(hu_allocator_t *alloc, struct sqlite3 *db,
                                                    size_t dim) {
    (void)alloc;
    (void)db;
    (void)dim;
    hu_vector_store_t vs = {.ctx = NULL, .vtable = NULL};
    return vs;
}

#endif
