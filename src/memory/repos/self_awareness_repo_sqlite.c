/* src/memory/repos/self_awareness_repo_sqlite.c
 * Repository for the self_awareness aggregate (self_awareness_stats +
 * reciprocity_scores). The only place these tables' SQL + the raw sqlite3
 * handle live. Domain code (src/context/self_awareness.c) depends on
 * hu_self_awareness_repo_t, never on this file. The migrated executors already
 * used BOUND PARAMETERS; SQL + schema reproduced VERBATIM (no behavior change).
 * DDD Phase 3. */
#ifdef HU_ENABLE_SQLITE
#include "human/memory/self_awareness_repo.h"
#include <sqlite3.h>
#include <string.h>
#include <time.h>

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

/* Schema reproduced verbatim from src/memory/engines/sqlite.c. */
static hu_error_t ensure_schema(sqlite3 *db) {
    hu_error_t e = run_stmt(db, "CREATE TABLE IF NOT EXISTS self_awareness_stats("
                                "contact_id TEXT PRIMARY KEY,"
                                "messages_sent_week INTEGER DEFAULT 0,"
                                "initiations_week INTEGER DEFAULT 0,"
                                "last_topic TEXT,"
                                "topic_repeat_count INTEGER DEFAULT 0,"
                                "updated_at INTEGER)");
    if (e != HU_OK)
        return e;
    return run_stmt(db, "CREATE TABLE IF NOT EXISTS reciprocity_scores("
                        "contact_id TEXT NOT NULL,"
                        "metric TEXT NOT NULL,"
                        "value REAL,"
                        "updated_at INTEGER,"
                        "PRIMARY KEY (contact_id, metric))");
}

static hu_error_t repo_stats_record_send(void *ctx, const char *contact_id, size_t cid_len,
                                         int init_inc, const char *topic, size_t topic_len,
                                         int64_t now_ts) {
    repo_ctx_t *c = ctx;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(c->db,
                                "INSERT INTO self_awareness_stats (contact_id, messages_sent_week, "
                                "initiations_week, last_topic, topic_repeat_count, updated_at) "
                                "VALUES (?, 1, ?, ?, 1, ?) "
                                "ON CONFLICT(contact_id) DO UPDATE SET "
                                "messages_sent_week = messages_sent_week + 1, "
                                "initiations_week = initiations_week + excluded.initiations_week, "
                                "last_topic = excluded.last_topic, "
                                "topic_repeat_count = CASE WHEN COALESCE(last_topic,'') = "
                                "COALESCE(excluded.last_topic,'') THEN topic_repeat_count + 1 "
                                "ELSE 1 END, updated_at = excluded.updated_at",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return HU_ERR_MEMORY_BACKEND;
    }
    sqlite3_bind_text(stmt, 1, contact_id, (int)cid_len, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, init_inc);
    if (topic && topic_len > 0)
        sqlite3_bind_text(stmt, 3, topic, (int)topic_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 3);
    sqlite3_bind_int64(stmt, 4, now_ts);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

static hu_error_t repo_stats_get(void *ctx, const char *contact_id, size_t cid_len, bool *found,
                                 hu_self_awareness_stats_row_t *out) {
    repo_ctx_t *c = ctx;
    if (found)
        *found = false;
    if (out)
        memset(out, 0, sizeof(*out));

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(c->db,
                                "SELECT messages_sent_week, initiations_week, last_topic, "
                                "topic_repeat_count FROM self_awareness_stats WHERE contact_id = ?",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return HU_ERR_MEMORY_BACKEND;
    }
    sqlite3_bind_text(stmt, 1, contact_id, (int)cid_len, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return HU_OK; /* no row -> found stays false */
    }
    if (out) {
        out->messages_sent_week = (uint32_t)sqlite3_column_int(stmt, 0);
        out->initiations_week = (uint32_t)sqlite3_column_int(stmt, 1);
        const char *lt = (const char *)sqlite3_column_text(stmt, 2);
        size_t lt_len = lt ? (size_t)sqlite3_column_bytes(stmt, 2) : 0;
        if (lt && lt_len > 0) {
            if (lt_len >= sizeof(out->last_topic))
                lt_len = sizeof(out->last_topic) - 1;
            memcpy(out->last_topic, lt, lt_len);
            out->last_topic[lt_len] = '\0';
            out->last_topic_len = lt_len;
        }
        out->topic_repeat_count = (uint32_t)sqlite3_column_int(stmt, 3);
    }
    if (found)
        *found = true;
    sqlite3_finalize(stmt);
    return HU_OK;
}

static double repo_reciprocity_get(void *ctx, const char *contact_id, size_t cid_len,
                                   const char *metric) {
    repo_ctx_t *c = ctx;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        c->db, "SELECT value FROM reciprocity_scores WHERE contact_id = ? AND metric = ?", -1,
        &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return 0.0;
    }
    sqlite3_bind_text(stmt, 1, contact_id, (int)cid_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, metric, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    double v = 0.0;
    if (rc == SQLITE_ROW)
        v = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

static hu_error_t repo_reciprocity_set(void *ctx, const char *contact_id, size_t cid_len,
                                       const char *metric, double value, int64_t now_ts) {
    repo_ctx_t *c = ctx;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(c->db,
                                "INSERT OR REPLACE INTO reciprocity_scores "
                                "(contact_id, metric, value, updated_at) VALUES (?, ?, ?, ?)",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return HU_ERR_MEMORY_BACKEND;
    }
    sqlite3_bind_text(stmt, 1, contact_id, (int)cid_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, metric, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 3, value);
    sqlite3_bind_int64(stmt, 4, now_ts);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

static void repo_deinit(void *ctx) {
    repo_ctx_t *c = ctx;
    if (c)
        c->alloc->free(c->alloc->ctx, c, sizeof(*c));
}

static const hu_self_awareness_repo_vtable_t k_vt = {
    .stats_record_send = repo_stats_record_send,
    .stats_get = repo_stats_get,
    .reciprocity_get = repo_reciprocity_get,
    .reciprocity_set = repo_reciprocity_set,
    .deinit = repo_deinit,
};

hu_error_t hu_self_awareness_repo_create(hu_memory_t *mem, hu_allocator_t *alloc,
                                         hu_self_awareness_repo_t *out) {
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
