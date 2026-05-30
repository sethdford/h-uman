/* src/memory/repos/opinions_repo_sqlite.c
 * The opinions repository: the only place opinions SQL + the raw sqlite3 handle
 * live. Domain code (src/memory/opinions.c) depends on hu_opinions_repo_t, never
 * on this file. SQL is reproduced VERBATIM from the pre-migration opinions.c so
 * behavior is byte-identical. DDD Phase 3 (one aggregate of ~24). */
#ifdef HU_ENABLE_SQLITE
#include "human/core/string.h"
#include "human/memory/opinions.h" /* hu_opinion_t, hu_opinions_free */
#include "human/memory/opinions_repo.h"
#include "human/memory/sql_transaction.h"
#include <sqlite3.h>
#include <string.h>

typedef struct {
    sqlite3 *db;
    hu_allocator_t *alloc;
} repo_ctx_t;

static hu_error_t run_stmt(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

/* Schema reproduced verbatim from src/memory/engines/sqlite.c (the canonical
 * opinions table), so a fresh :memory: db is self-sufficient for tests. */
static hu_error_t ensure_schema(sqlite3 *db) {
    hu_error_t e = run_stmt(db, "CREATE TABLE IF NOT EXISTS opinions("
                                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                "topic TEXT,"
                                "position TEXT,"
                                "confidence REAL,"
                                "first_expressed INTEGER,"
                                "last_expressed INTEGER,"
                                "superseded_by INTEGER)");
    if (e != HU_OK)
        return e;
    return run_stmt(db, "CREATE INDEX IF NOT EXISTS idx_opinions_topic ON opinions(topic)");
}

static hu_error_t repo_find_active(void *ctx, hu_allocator_t *alloc, const char *topic,
                                   size_t topic_len, bool *found, int64_t *out_id,
                                   char **out_position, size_t *out_position_len) {
    repo_ctx_t *c = ctx;
    if (found)
        *found = false;
    if (out_position)
        *out_position = NULL;
    if (out_position_len)
        *out_position_len = 0;

    sqlite3_stmt *sel = NULL;
    int rc = sqlite3_prepare_v2(c->db,
                                "SELECT id, position FROM opinions WHERE topic=? AND "
                                "superseded_by IS NULL LIMIT 1",
                                -1, &sel, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(sel, 1, topic, (int)topic_len, SQLITE_STATIC);
    rc = sqlite3_step(sel);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        sqlite3_finalize(sel);
        return HU_ERR_MEMORY_BACKEND;
    }
    if (rc == SQLITE_ROW) {
        if (out_id)
            *out_id = sqlite3_column_int64(sel, 0);
        const char *pos = (const char *)sqlite3_column_text(sel, 1);
        size_t pos_len = pos ? (size_t)sqlite3_column_bytes(sel, 1) : 0;
        if (out_position && pos && pos_len > 0) {
            *out_position = hu_strndup(alloc, pos, pos_len);
            if (out_position_len)
                *out_position_len = pos_len;
        }
        if (found)
            *found = true;
    }
    sqlite3_finalize(sel);
    return HU_OK;
}

static hu_error_t repo_update_last_expressed(void *ctx, int64_t id, int64_t last_expressed,
                                             float confidence) {
    repo_ctx_t *c = ctx;
    sqlite3_stmt *upd = NULL;
    int rc = sqlite3_prepare_v2(
        c->db, "UPDATE opinions SET last_expressed=?, confidence=? WHERE id=?", -1, &upd, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_int64(upd, 1, last_expressed);
    sqlite3_bind_double(upd, 2, (double)confidence);
    sqlite3_bind_int64(upd, 3, id);
    rc = sqlite3_step(upd);
    sqlite3_finalize(upd);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

/* Shared INSERT helper — superseded_by NULL. */
static hu_error_t do_insert(sqlite3 *db, const char *topic, size_t topic_len, const char *position,
                            size_t position_len, float confidence, int64_t first_expressed,
                            int64_t last_expressed, int64_t *out_id) {
    sqlite3_stmt *ins = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "INSERT INTO opinions(topic,position,confidence,first_expressed,"
                                "last_expressed,superseded_by) VALUES(?,?,?,?,?,NULL)",
                                -1, &ins, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(ins, 1, topic, (int)topic_len, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, position, (int)position_len, SQLITE_STATIC);
    sqlite3_bind_double(ins, 3, (double)confidence);
    sqlite3_bind_int64(ins, 4, first_expressed);
    sqlite3_bind_int64(ins, 5, last_expressed);
    rc = sqlite3_step(ins);
    if (rc != SQLITE_DONE) {
        sqlite3_finalize(ins);
        return HU_ERR_MEMORY_BACKEND;
    }
    if (out_id)
        *out_id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(ins);
    return HU_OK;
}

static hu_error_t repo_insert(void *ctx, const char *topic, size_t topic_len, const char *position,
                              size_t position_len, float confidence, int64_t first_expressed,
                              int64_t last_expressed, int64_t *out_id) {
    repo_ctx_t *c = ctx;
    return do_insert(c->db, topic, topic_len, position, position_len, confidence, first_expressed,
                     last_expressed, out_id);
}

static hu_error_t repo_insert_superseding(void *ctx, const char *topic, size_t topic_len,
                                          const char *position, size_t position_len,
                                          float confidence, int64_t now_ts, int64_t old_id,
                                          int64_t *out_new_id) {
    repo_ctx_t *c = ctx;
    hu_sql_txn_t txn = {0};
    if (hu_sql_txn_begin(&txn, c->db) != HU_OK)
        return HU_ERR_MEMORY_BACKEND;

    int64_t new_id = 0;
    if (do_insert(c->db, topic, topic_len, position, position_len, confidence, now_ts, now_ts,
                  &new_id) != HU_OK) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_MEMORY_BACKEND;
    }

    sqlite3_stmt *sup = NULL;
    int rc =
        sqlite3_prepare_v2(c->db, "UPDATE opinions SET superseded_by=? WHERE id=?", -1, &sup, NULL);
    if (rc != SQLITE_OK) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_MEMORY_BACKEND;
    }
    sqlite3_bind_int64(sup, 1, new_id);
    sqlite3_bind_int64(sup, 2, old_id);
    rc = sqlite3_step(sup);
    sqlite3_finalize(sup);
    if (rc != SQLITE_DONE) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_MEMORY_BACKEND;
    }
    if (hu_sql_txn_commit(&txn) != HU_OK) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_MEMORY_BACKEND;
    }
    if (out_new_id)
        *out_new_id = new_id;
    return HU_OK;
}

static hu_error_t repo_query(void *ctx, hu_allocator_t *alloc, const char *topic, size_t topic_len,
                             bool superseded_only, hu_opinion_t **out, size_t *out_count) {
    repo_ctx_t *c = ctx;
    *out = NULL;
    *out_count = 0;

    const char *sql =
        superseded_only ? "SELECT id,topic,position,confidence,first_expressed,last_expressed,"
                          "superseded_by FROM opinions WHERE topic=? AND superseded_by IS NOT NULL"
                        : "SELECT id,topic,position,confidence,first_expressed,last_expressed,"
                          "superseded_by FROM opinions WHERE topic=? AND superseded_by IS NULL";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(c->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(stmt, 1, topic, (int)topic_len, SQLITE_STATIC);

    size_t cap = 16;
    size_t count = 0;
    hu_opinion_t *arr = (hu_opinion_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_opinion_t));
    if (!arr) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            size_t old_cap = cap;
            cap *= 2;
            hu_opinion_t *n = (hu_opinion_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_opinion_t));
            if (!n) {
                hu_opinions_free(alloc, arr, count);
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(n, arr, count * sizeof(hu_opinion_t));
            alloc->free(alloc->ctx, arr, old_cap * sizeof(hu_opinion_t));
            arr = n;
        }

        hu_opinion_t *o = &arr[count];
        memset(o, 0, sizeof(*o));
        o->id = sqlite3_column_int64(stmt, 0);
        const char *t = (const char *)sqlite3_column_text(stmt, 1);
        size_t t_len = t ? (size_t)sqlite3_column_bytes(stmt, 1) : 0;
        if (t && t_len > 0) {
            o->topic = hu_strndup(alloc, t, t_len);
            o->topic_len = t_len;
        }
        const char *p = (const char *)sqlite3_column_text(stmt, 2);
        size_t p_len = p ? (size_t)sqlite3_column_bytes(stmt, 2) : 0;
        if (p && p_len > 0) {
            o->position = hu_strndup(alloc, p, p_len);
            o->position_len = p_len;
        }
        o->confidence = (float)sqlite3_column_double(stmt, 3);
        o->first_expressed = sqlite3_column_int64(stmt, 4);
        o->last_expressed = sqlite3_column_int64(stmt, 5);
        o->superseded_by = sqlite3_column_int64(stmt, 6);
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0 && arr) {
        alloc->free(alloc->ctx, arr, cap * sizeof(hu_opinion_t));
        arr = NULL;
    }
    *out = arr;
    *out_count = count;
    return HU_OK;
}

static void repo_deinit(void *ctx) {
    repo_ctx_t *c = ctx;
    if (c)
        c->alloc->free(c->alloc->ctx, c, sizeof(*c));
}

static const hu_opinions_repo_vtable_t k_vt = {
    .find_active = repo_find_active,
    .update_last_expressed = repo_update_last_expressed,
    .insert = repo_insert,
    .insert_superseding = repo_insert_superseding,
    .query = repo_query,
    .deinit = repo_deinit,
};

hu_error_t hu_opinions_repo_create(hu_memory_t *mem, hu_allocator_t *alloc,
                                   hu_opinions_repo_t *out) {
    if (!mem || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3 *db = hu_sqlite_memory_get_db(mem); /* legal HERE — engine layer */
    if (!db)
        return HU_ERR_NOT_SUPPORTED; /* non-sqlite backend */
    if (ensure_schema(db) != HU_OK)
        return HU_ERR_MEMORY_BACKEND;
    repo_ctx_t *c = alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    c->db = db;
    c->alloc = alloc;
    out->ctx = c;
    out->vtable = &k_vt;
    return HU_OK;
}
#endif /* HU_ENABLE_SQLITE */
