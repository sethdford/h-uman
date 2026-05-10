#include "human/memory/neural_memory.h"

#ifdef HU_ENABLE_SQLITE

#include "human/memory/graph.h"
#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* SQLite handle from the graph's internal DB — same pattern as cross_graph.c. */

/* Maximum blob payload: 100 MB.  Larger inputs are rejected before any SQL
 * work; the per-user quota and eviction are handled by W14. */
#define NEURAL_BLOB_MAX_BYTES (100ULL * 1024ULL * 1024ULL)

/* ── DDL helpers ──────────────────────────────────────────────────────────── */

static int run_ddl(struct sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK)
        return rc;
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE || rc == SQLITE_OK) ? SQLITE_OK : rc;
}

static hu_error_t ensure_schema(struct sqlite3 *db) {
    static const char *const stmts[] = {
        "CREATE TABLE IF NOT EXISTS neural_kv_cache ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "prompt_hash TEXT NOT NULL,"
        "model_version TEXT NOT NULL,"
        "prompt_token_count INTEGER NOT NULL,"
        "blob BLOB NOT NULL,"
        "created_at INTEGER NOT NULL,"
        "UNIQUE(prompt_hash, model_version))",

        "CREATE TABLE IF NOT EXISTS neural_reasoning_traces ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "contact_id TEXT NOT NULL DEFAULT '',"
        "goal_verb TEXT NOT NULL,"
        "anchors TEXT NOT NULL DEFAULT '',"
        "cot_text TEXT NOT NULL,"
        "outcome TEXT,"
        "confidence_mean REAL NOT NULL DEFAULT 1.0,"
        "confidence_variance REAL NOT NULL DEFAULT 0.0,"
        "recorded_at INTEGER NOT NULL)",

        "CREATE INDEX IF NOT EXISTS idx_traces_goal "
        "ON neural_reasoning_traces(contact_id, goal_verb)",

        "CREATE TABLE IF NOT EXISTS neural_blobs ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "contact_id TEXT NOT NULL DEFAULT '',"
        "mime_type TEXT NOT NULL,"
        "bytes BLOB NOT NULL,"
        "caption TEXT,"
        "created_at INTEGER NOT NULL)",

        NULL,
    };
    for (size_t i = 0; stmts[i]; i++) {
        if (run_ddl(db, stmts[i]) != SQLITE_OK)
            return HU_ERR_IO;
    }
    return HU_OK;
}

static struct sqlite3 *get_db(hu_memory_facade_t *m) {
    hu_graph_t *g = hu_memory_facade_graph_handle(m);
    return g ? hu_graph_sqlite_connection(g) : NULL;
}

/* ── Anchor CSV helpers ───────────────────────────────────────────────────── */

/* Serialize int64 array to comma-separated string in caller-provided buffer.
 * Silently truncates if buf is too small — callers pass NEURAL_ANC_BUF_SIZE. */
#define NEURAL_ANC_BUF_SIZE 4096

static void serialize_anchors(char *buf, size_t cap,
                               const int64_t *ids, size_t n) {
    size_t pos = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        /* leave room for max int64 digits + comma + NUL */
        if (pos + 22 >= cap)
            break;
        int w = snprintf(buf + pos, cap - pos, "%s%lld",
                         i > 0 ? "," : "", (long long)ids[i]);
        if (w <= 0)
            break;
        pos += (size_t)w;
    }
}

/* Deserialize comma-separated string back to a heap-allocated int64 array.
 * Returns the count; *out is NULL on empty CSV or OOM. */
static size_t deserialize_anchors(hu_allocator_t *alloc, const char *csv,
                                   int64_t **out) {
    *out = NULL;
    if (!csv || !*csv)
        return 0;

    /* count tokens to know allocation size */
    size_t n = 1;
    for (const char *p = csv; *p; p++) {
        if (*p == ',')
            n++;
    }

    int64_t *ids = alloc->alloc(alloc->ctx, n * sizeof(int64_t));
    if (!ids)
        return 0;

    size_t count = 0;
    const char *p = csv;
    while (*p && count < n) {
        char *end = NULL;
        long long v = strtoll(p, &end, 10);
        if (end == p)
            break;
        ids[count++] = (int64_t)v;
        p = end;
        if (*p == ',')
            p++;
    }

    if (count == 0) {
        alloc->free(alloc->ctx, ids, n * sizeof(int64_t));
        *out = NULL;
        return 0;
    }

    *out = ids;
    return count;
}

/* Return true when csv contains at least one of the query anchor IDs.
 * Uses whole-token matching: "12" does not match "123". */
static int anchors_overlap(const char *csv, const int64_t *query, size_t n) {
    if (!query || n == 0)
        return 1; /* no anchor filter — everything passes */
    if (!csv || !*csv)
        return 0;

    for (size_t i = 0; i < n; i++) {
        char needle[24];
        int nlen = snprintf(needle, sizeof(needle), "%lld", (long long)query[i]);
        if (nlen <= 0)
            continue;
        const char *p = csv;
        while ((p = strstr(p, needle)) != NULL) {
            int pre_ok  = (p == csv)            || (p[-1]        == ',');
            int post_ok = (p[nlen] == '\0') || (p[nlen]       == ',');
            if (pre_ok && post_ok)
                return 1;
            p++;
        }
    }
    return 0;
}

/* ── KV-cache ─────────────────────────────────────────────────────────────── */

hu_error_t hu_kv_cache_get(hu_memory_facade_t *m, const char *prompt_hash,
                            const char *model_version,
                            hu_allocator_t *alloc, hu_kv_cache_entry_t **out) {
    if (!m || !prompt_hash || !model_version || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    struct sqlite3 *db = get_db(m);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (ensure_schema(db) != HU_OK)
        return HU_ERR_IO;

    static const char *sql =
        "SELECT prompt_token_count, blob, created_at "
        "FROM neural_kv_cache "
        "WHERE prompt_hash = ? AND model_version = ?";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(st, 1, prompt_hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, model_version, -1, SQLITE_STATIC);

    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        return (rc == SQLITE_DONE) ? HU_ERR_NOT_FOUND : HU_ERR_IO;
    }

    int64_t     token_count = sqlite3_column_int64(st, 0);
    const void *blob_data   = sqlite3_column_blob(st, 1);
    int         blob_bytes  = sqlite3_column_bytes(st, 1);
    int64_t     created_at  = sqlite3_column_int64(st, 2);

    hu_kv_cache_entry_t *e = alloc->alloc(alloc->ctx, sizeof(*e));
    if (!e) {
        sqlite3_finalize(st);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(e, 0, sizeof(*e));

    strncpy(e->prompt_hash,    prompt_hash,    sizeof(e->prompt_hash)    - 1);
    strncpy(e->model_version,  model_version,  sizeof(e->model_version)  - 1);
    e->prompt_token_count = token_count;
    e->created_at         = created_at;
    e->blob_len           = (size_t)blob_bytes;
    e->blob               = NULL;

    if (blob_bytes > 0 && blob_data) {
        e->blob = alloc->alloc(alloc->ctx, (size_t)blob_bytes);
        if (!e->blob) {
            sqlite3_finalize(st);
            alloc->free(alloc->ctx, e, sizeof(*e));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(e->blob, blob_data, (size_t)blob_bytes);
    }

    sqlite3_finalize(st);
    *out = e;
    return HU_OK;
}

hu_error_t hu_kv_cache_put(hu_memory_facade_t *m, const hu_kv_cache_entry_t *entry) {
    if (!m || !entry || !entry->prompt_hash[0] || !entry->model_version[0])
        return HU_ERR_INVALID_ARGUMENT;

    struct sqlite3 *db = get_db(m);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (ensure_schema(db) != HU_OK)
        return HU_ERR_IO;

    static const char *sql =
        "INSERT INTO neural_kv_cache "
        "(prompt_hash, model_version, prompt_token_count, blob, created_at) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(prompt_hash, model_version) "
        "DO UPDATE SET "
        "prompt_token_count = excluded.prompt_token_count, "
        "blob = excluded.blob, "
        "created_at = excluded.created_at";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    int64_t now = (int64_t)time(NULL);
    sqlite3_bind_text(st, 1, entry->prompt_hash,   -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, entry->model_version, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, entry->prompt_token_count);

    if (entry->blob && entry->blob_len > 0) {
        sqlite3_bind_blob(st, 4, entry->blob, (int)entry->blob_len, SQLITE_STATIC);
    } else {
        sqlite3_bind_blob(st, 4, "", 0, SQLITE_STATIC);
    }

    sqlite3_bind_int64(st, 5, entry->created_at ? entry->created_at : now);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_IO;
}

hu_error_t hu_kv_cache_invalidate_for_model(hu_memory_facade_t *m,
                                              const char *model_version) {
    if (!m || !model_version)
        return HU_ERR_INVALID_ARGUMENT;

    struct sqlite3 *db = get_db(m);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (ensure_schema(db) != HU_OK)
        return HU_ERR_IO;

    static const char *sql =
        "DELETE FROM neural_kv_cache WHERE model_version = ?";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(st, 1, model_version, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_IO;
}

void hu_kv_cache_entry_free(hu_allocator_t *alloc, hu_kv_cache_entry_t *e) {
    if (!e)
        return;
    if (e->blob)
        alloc->free(alloc->ctx, e->blob, e->blob_len);
    alloc->free(alloc->ctx, e, sizeof(*e));
}

/* ── Reasoning traces ─────────────────────────────────────────────────────── */

hu_error_t hu_reasoning_trace_record(hu_memory_facade_t *m, const char *contact_id,
                                      size_t cid_len,
                                      const hu_reasoning_trace_t *trace,
                                      int64_t *out_id) {
    if (!m || !trace || !trace->cot_text || !out_id)
        return HU_ERR_INVALID_ARGUMENT;

    struct sqlite3 *db = get_db(m);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (ensure_schema(db) != HU_OK)
        return HU_ERR_IO;

    char anc_buf[NEURAL_ANC_BUF_SIZE];
    serialize_anchors(anc_buf, sizeof(anc_buf),
                      trace->anchor_entity_ids, trace->anchors_count);

    static const char *sql =
        "INSERT INTO neural_reasoning_traces "
        "(contact_id, goal_verb, anchors, cot_text, outcome, "
        " confidence_mean, confidence_variance, recorded_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    int64_t now = (int64_t)time(NULL);

    sqlite3_bind_text(st, 1, contact_id ? contact_id : "", (int)cid_len,
                      SQLITE_STATIC);
    sqlite3_bind_text(st, 2, trace->goal_verb, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, anc_buf, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, trace->cot_text, (int)trace->cot_len, SQLITE_STATIC);

    if (trace->outcome) {
        sqlite3_bind_text(st, 5, trace->outcome, -1, SQLITE_STATIC);
    } else {
        sqlite3_bind_null(st, 5);
    }

    sqlite3_bind_double(st, 6, (double)trace->belief.mean);
    sqlite3_bind_double(st, 7, (double)trace->belief.variance);
    sqlite3_bind_int64(st, 8,
                       trace->recorded_at ? trace->recorded_at : now);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE)
        return HU_ERR_IO;

    *out_id = sqlite3_last_insert_rowid(db);
    return HU_OK;
}

hu_error_t hu_reasoning_trace_recall(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                      const char *contact_id, size_t cid_len,
                                      const char *goal_verb, size_t goal_len,
                                      const int64_t *anchors, size_t anchors_count,
                                      size_t limit,
                                      hu_reasoning_trace_t **out, size_t *out_count) {
    if (!m || !alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    *out       = NULL;
    *out_count = 0;

    struct sqlite3 *db = get_db(m);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (ensure_schema(db) != HU_OK)
        return HU_ERR_IO;

    /* When anchor filtering is requested, fetch a larger window and filter in
     * C; otherwise the SQL LIMIT is exact. Cap at 1000 to bound memory. */
    size_t effective_limit = (limit == 0) ? 1000 : limit;
    size_t fetch_limit     = (anchors_count > 0)
                                 ? (effective_limit * 4 < 1000
                                        ? effective_limit * 4
                                        : (size_t)1000)
                                 : effective_limit;

    static const char *sql =
        "SELECT id, goal_verb, anchors, cot_text, outcome, "
        "       confidence_mean, confidence_variance, recorded_at "
        "FROM neural_reasoning_traces "
        "WHERE contact_id = ? AND goal_verb = ? "
        "ORDER BY recorded_at DESC "
        "LIMIT ?";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(st, 1, contact_id ? contact_id : "", (int)cid_len,
                      SQLITE_STATIC);
    sqlite3_bind_text(st, 2, goal_verb ? goal_verb : "", (int)goal_len,
                      SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (int64_t)fetch_limit);

    /* Collect matching rows into a temporary stack of pointers; we don't know
     * the final count until anchor filtering is done. */
    hu_reasoning_trace_t *arr = NULL;
    size_t arr_cap = 0;
    size_t arr_len = 0;
    hu_error_t err = HU_OK;

    while (sqlite3_step(st) == SQLITE_ROW && arr_len < effective_limit) {
        const char *row_anchors =
            (const char *)sqlite3_column_text(st, 2);

        if (!anchors_overlap(row_anchors, anchors, anchors_count))
            continue;

        /* Grow output array if needed. */
        if (arr_len == arr_cap) {
            size_t new_cap = arr_cap ? arr_cap * 2 : 8;
            hu_reasoning_trace_t *tmp =
                alloc->alloc(alloc->ctx, new_cap * sizeof(*arr));
            if (!tmp) {
                err = HU_ERR_OUT_OF_MEMORY;
                break;
            }
            if (arr) {
                memcpy(tmp, arr, arr_len * sizeof(*arr));
                alloc->free(alloc->ctx, arr, arr_cap * sizeof(*arr));
            }
            arr     = tmp;
            arr_cap = new_cap;
        }

        hu_reasoning_trace_t *t = &arr[arr_len];
        memset(t, 0, sizeof(*t));

        t->id = sqlite3_column_int64(st, 0);
        strncpy(t->goal_verb,
                (const char *)sqlite3_column_text(st, 1),
                sizeof(t->goal_verb) - 1);

        /* Deserialize anchors CSV. */
        t->anchors_count = deserialize_anchors(
            alloc,
            (const char *)sqlite3_column_text(st, 2),
            &t->anchor_entity_ids);

        /* cot_text */
        const char *cot = (const char *)sqlite3_column_text(st, 3);
        size_t cot_bytes = cot ? (size_t)sqlite3_column_bytes(st, 3) : 0;
        t->cot_len = cot_bytes;
        if (cot_bytes > 0) {
            t->cot_text = alloc->alloc(alloc->ctx, cot_bytes + 1);
            if (!t->cot_text) {
                /* Free already-filled fields of this partial row. */
                if (t->anchor_entity_ids)
                    alloc->free(alloc->ctx, t->anchor_entity_ids,
                                t->anchors_count * sizeof(int64_t));
                err = HU_ERR_OUT_OF_MEMORY;
                break;
            }
            memcpy(t->cot_text, cot, cot_bytes);
            t->cot_text[cot_bytes] = '\0';
        }

        /* outcome (nullable) */
        const char *out_str = (const char *)sqlite3_column_text(st, 4);
        if (out_str) {
            size_t out_bytes = (size_t)sqlite3_column_bytes(st, 4);
            t->outcome = alloc->alloc(alloc->ctx, out_bytes + 1);
            if (!t->outcome) {
                if (t->cot_text)
                    alloc->free(alloc->ctx, t->cot_text, t->cot_len + 1);
                if (t->anchor_entity_ids)
                    alloc->free(alloc->ctx, t->anchor_entity_ids,
                                t->anchors_count * sizeof(int64_t));
                err = HU_ERR_OUT_OF_MEMORY;
                break;
            }
            memcpy(t->outcome, out_str, out_bytes);
            t->outcome[out_bytes] = '\0';
        }

        t->belief.mean     = (float)sqlite3_column_double(st, 5);
        t->belief.variance = (float)sqlite3_column_double(st, 6);
        t->recorded_at     = sqlite3_column_int64(st, 7);

        arr_len++;
    }

    sqlite3_finalize(st);

    if (err != HU_OK) {
        /* Free whatever we did manage to allocate. */
        hu_reasoning_traces_free(alloc, arr, arr_len);
        return err;
    }

    *out       = arr;
    *out_count = arr_len;
    return HU_OK;
}

void hu_reasoning_traces_free(hu_allocator_t *alloc,
                               hu_reasoning_trace_t *traces, size_t n) {
    if (!traces)
        return;
    for (size_t i = 0; i < n; i++) {
        if (traces[i].cot_text)
            alloc->free(alloc->ctx, traces[i].cot_text,
                        traces[i].cot_len + 1);
        if (traces[i].outcome)
            alloc->free(alloc->ctx, traces[i].outcome,
                        strlen(traces[i].outcome) + 1);
        if (traces[i].anchor_entity_ids)
            alloc->free(alloc->ctx, traces[i].anchor_entity_ids,
                        traces[i].anchors_count * sizeof(int64_t));
    }
    alloc->free(alloc->ctx, traces, n * sizeof(hu_reasoning_trace_t));
}

/* ── Multimodal blobs ─────────────────────────────────────────────────────── */

hu_error_t hu_memory_blob_put(hu_memory_facade_t *m, const char *contact_id,
                               size_t cid_len, const hu_memory_blob_t *blob,
                               int64_t *out_id) {
    if (!m || !blob || !out_id)
        return HU_ERR_INVALID_ARGUMENT;

    /* Reject blobs that exceed the per-user size budget (W14 evicts totals). */
    if (blob->bytes_len > NEURAL_BLOB_MAX_BYTES)
        return HU_ERR_INVALID_ARGUMENT;

    struct sqlite3 *db = get_db(m);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (ensure_schema(db) != HU_OK)
        return HU_ERR_IO;

    static const char *sql =
        "INSERT INTO neural_blobs "
        "(contact_id, mime_type, bytes, caption, created_at) "
        "VALUES (?, ?, ?, ?, ?)";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    int64_t now = (int64_t)time(NULL);

    sqlite3_bind_text(st, 1, contact_id ? contact_id : "", (int)cid_len,
                      SQLITE_STATIC);
    sqlite3_bind_text(st, 2, blob->mime_type, -1, SQLITE_STATIC);

    if (blob->bytes && blob->bytes_len > 0) {
        sqlite3_bind_blob(st, 3, blob->bytes, (int)blob->bytes_len, SQLITE_STATIC);
    } else {
        sqlite3_bind_blob(st, 3, "", 0, SQLITE_STATIC);
    }

    if (blob->caption && blob->caption_len > 0) {
        sqlite3_bind_text(st, 4, blob->caption, (int)blob->caption_len,
                          SQLITE_STATIC);
    } else {
        sqlite3_bind_null(st, 4);
    }

    sqlite3_bind_int64(st, 5, blob->created_at ? blob->created_at : now);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE)
        return HU_ERR_IO;

    *out_id = sqlite3_last_insert_rowid(db);
    return HU_OK;
}

hu_error_t hu_memory_blob_get(hu_memory_facade_t *m, hu_allocator_t *alloc,
                               int64_t blob_id, hu_memory_blob_t **out) {
    if (!m || !alloc || !out || blob_id <= 0)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;

    struct sqlite3 *db = get_db(m);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (ensure_schema(db) != HU_OK)
        return HU_ERR_IO;

    static const char *sql =
        "SELECT mime_type, bytes, caption, created_at "
        "FROM neural_blobs "
        "WHERE id = ?";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_int64(st, 1, blob_id);

    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        return (rc == SQLITE_DONE) ? HU_ERR_NOT_FOUND : HU_ERR_IO;
    }

    const char *mime     = (const char *)sqlite3_column_text(st, 0);
    const void *raw_data = sqlite3_column_blob(st, 1);
    int         raw_len  = sqlite3_column_bytes(st, 1);
    const char *caption  = (const char *)sqlite3_column_text(st, 2);
    int         cap_len  = caption ? sqlite3_column_bytes(st, 2) : 0;
    int64_t     created  = sqlite3_column_int64(st, 3);

    hu_memory_blob_t *b = alloc->alloc(alloc->ctx, sizeof(*b));
    if (!b) {
        sqlite3_finalize(st);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(b, 0, sizeof(*b));

    if (mime)
        strncpy(b->mime_type, mime, sizeof(b->mime_type) - 1);

    b->bytes_len = (size_t)raw_len;
    b->created_at = created;
    b->bytes = NULL;

    if (raw_len > 0 && raw_data) {
        b->bytes = alloc->alloc(alloc->ctx, (size_t)raw_len);
        if (!b->bytes) {
            sqlite3_finalize(st);
            alloc->free(alloc->ctx, b, sizeof(*b));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(b->bytes, raw_data, (size_t)raw_len);
    }

    b->caption     = NULL;
    b->caption_len = (size_t)cap_len;

    if (cap_len > 0 && caption) {
        b->caption = alloc->alloc(alloc->ctx, (size_t)cap_len + 1);
        if (!b->caption) {
            if (b->bytes)
                alloc->free(alloc->ctx, b->bytes, b->bytes_len);
            sqlite3_finalize(st);
            alloc->free(alloc->ctx, b, sizeof(*b));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(b->caption, caption, (size_t)cap_len);
        b->caption[cap_len] = '\0';
    }

    b->id = blob_id;

    sqlite3_finalize(st);
    *out = b;
    return HU_OK;
}

void hu_memory_blob_free(hu_allocator_t *alloc, hu_memory_blob_t *blob) {
    if (!blob)
        return;
    if (blob->bytes)
        alloc->free(alloc->ctx, blob->bytes, blob->bytes_len);
    if (blob->caption)
        alloc->free(alloc->ctx, blob->caption, blob->caption_len + 1);
    alloc->free(alloc->ctx, blob, sizeof(*blob));
}

#endif /* HU_ENABLE_SQLITE */
