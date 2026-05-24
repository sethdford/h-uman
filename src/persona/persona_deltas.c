#include "human/persona/persona_deltas.h"

#ifdef HU_ENABLE_SQLITE
#include "human/agent/theory_of_mind.h"
#include "human/memory/memory.h"
#include <sqlite3.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

hu_persona_evolver_config_t hu_persona_evolver_default_config(void) {
    hu_persona_evolver_config_t c = {0};
    c.apply_threshold = 0.75f;
    c.drop_threshold = 0.50f;
    c.corroboration_min = 3;
    c.rate_limit_per_hour = 10;
    c.max_apply = 32;
    return c;
}

#ifdef HU_ENABLE_SQLITE
/* Only referenced from the SQLite-enabled paths below; gate the
 * definition so the no-sqlite / minimal build doesn't trip
 * -Werror=unused-function. */
static int64_t now_ms_or(int64_t given) {
    if (given > 0)
        return given;
    return (int64_t)time(NULL) * 1000;
}

static hu_error_t ensure_schema(struct sqlite3 *db) {
    static const char *const ddl[] = {
        "CREATE TABLE IF NOT EXISTS persona_deltas ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "contact_id TEXT NOT NULL DEFAULT '',"
        "kind INTEGER NOT NULL,"
        "key TEXT,"
        "value TEXT,"
        "confidence REAL NOT NULL DEFAULT 0.5,"
        "source TEXT,"
        "proposed_at INTEGER NOT NULL,"
        "status INTEGER NOT NULL DEFAULT 0,"
        "status_reason TEXT,"
        "applied_at INTEGER NOT NULL DEFAULT 0)",
        "CREATE INDEX IF NOT EXISTS idx_persona_deltas_contact "
        "ON persona_deltas(contact_id, status)",
        "CREATE INDEX IF NOT EXISTS idx_persona_deltas_kind_key "
        "ON persona_deltas(contact_id, kind, key)",
        NULL,
    };
    for (size_t i = 0; ddl[i]; i++) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, ddl[i], -1, &st, NULL) != SQLITE_OK)
            return HU_ERR_IO;
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            return HU_ERR_IO;
        }
        sqlite3_finalize(st);
    }
    return HU_OK;
}

static hu_error_t persona_delta_propose_db(struct sqlite3 *db, const char *contact_id,
                                           size_t contact_id_len, hu_persona_delta_kind_t kind,
                                           const char *key, const char *value, float confidence,
                                           const char *source, int64_t proposed_at_ms,
                                           int64_t *out_delta_id) {
    if (!db || !contact_id || contact_id_len == 0 || !value || kind >= HU_PERSONA_DELTA_MAX)
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t e = ensure_schema(db);
    if (e != HU_OK)
        return e;

    if (confidence < 0.0f)
        confidence = 0.0f;
    if (confidence > 1.0f)
        confidence = 1.0f;

    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO persona_deltas(contact_id, kind, key, value, confidence,"
                      " source, proposed_at, status) VALUES(?, ?, ?, ?, ?, ?, ?, 0)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(st, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, (int)kind);
    sqlite3_bind_text(st, 3, key ? key : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, value, -1, SQLITE_STATIC);
    sqlite3_bind_double(st, 5, (double)confidence);
    sqlite3_bind_text(st, 6, source ? source : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 7, now_ms_or(proposed_at_ms));
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return HU_ERR_IO;
    }
    if (out_delta_id)
        *out_delta_id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(st);
    return HU_OK;
}

static hu_error_t persona_delta_list_db(struct sqlite3 *db, hu_allocator_t *alloc,
                                        const char *contact_id, size_t contact_id_len,
                                        hu_persona_delta_status_t status_filter, size_t limit,
                                        hu_persona_delta_t **out, size_t *out_count) {
    if (!db || !alloc || !contact_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    if (limit == 0)
        limit = 64;
    hu_error_t e = ensure_schema(db);
    if (e != HU_OK)
        return e;

    sqlite3_stmt *st = NULL;
    const char *sql_all =
        "SELECT id, kind, key, value, confidence, proposed_at, source, status, status_reason "
        "FROM persona_deltas WHERE contact_id = ? ORDER BY proposed_at DESC LIMIT ?";
    const char *sql_filt =
        "SELECT id, kind, key, value, confidence, proposed_at, source, status, status_reason "
        "FROM persona_deltas WHERE contact_id = ? AND status = ? ORDER BY proposed_at DESC LIMIT ?";
    bool filt = (int)status_filter >= 0;
    if (status_filter > HU_DELTA_STATUS_QUARANTINED)
        filt = false;
    if (sqlite3_prepare_v2(db, filt ? sql_filt : sql_all, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(st, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
    if (filt) {
        sqlite3_bind_int(st, 2, (int)status_filter);
        sqlite3_bind_int(st, 3, (int)limit);
    } else {
        sqlite3_bind_int(st, 2, (int)limit);
    }

    hu_persona_delta_t *arr =
        (hu_persona_delta_t *)alloc->alloc(alloc->ctx, sizeof(hu_persona_delta_t) * limit);
    if (!arr) {
        sqlite3_finalize(st);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t n = 0;
    while (sqlite3_step(st) == SQLITE_ROW && n < limit) {
        hu_persona_delta_t *d = &arr[n++];
        memset(d, 0, sizeof(*d));
        d->id = sqlite3_column_int64(st, 0);
        d->kind = (hu_persona_delta_kind_t)sqlite3_column_int(st, 1);
        const char *k = (const char *)sqlite3_column_text(st, 2);
        const char *v = (const char *)sqlite3_column_text(st, 3);
        snprintf(d->key, sizeof(d->key), "%s", k ? k : "");
        snprintf(d->value, sizeof(d->value), "%s", v ? v : "");
        d->confidence = (float)sqlite3_column_double(st, 4);
        d->proposed_at_ms = sqlite3_column_int64(st, 5);
        const char *src = (const char *)sqlite3_column_text(st, 6);
        snprintf(d->source, sizeof(d->source), "%s", src ? src : "");
        d->status = (hu_persona_delta_status_t)sqlite3_column_int(st, 7);
        const char *reason = (const char *)sqlite3_column_text(st, 8);
        snprintf(d->status_reason, sizeof(d->status_reason), "%s", reason ? reason : "");
    }
    sqlite3_finalize(st);
    *out = arr;
    *out_count = n;
    return HU_OK;
}

#endif

hu_error_t hu_persona_delta_propose(struct hu_graph *graph, const char *contact_id,
                                    size_t contact_id_len, hu_persona_delta_kind_t kind,
                                    const char *key, const char *value, float confidence,
                                    const char *source, int64_t proposed_at_ms,
                                    int64_t *out_delta_id) {
    if (!graph || !contact_id || contact_id_len == 0 || !value || kind >= HU_PERSONA_DELTA_MAX)
        return HU_ERR_INVALID_ARGUMENT;

#ifdef HU_ENABLE_SQLITE
    struct sqlite3 *db = hu_memory_sqlite_from_graph((struct hu_graph *)graph);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    return persona_delta_propose_db(db, contact_id, contact_id_len, kind, key, value, confidence,
                                    source, proposed_at_ms, out_delta_id);
#else
    (void)contact_id_len;
    (void)kind;
    (void)key;
    (void)value;
    (void)confidence;
    (void)source;
    (void)proposed_at_ms;
    (void)out_delta_id;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t hu_persona_delta_propose_facade(hu_memory_facade_t *m, const char *contact_id,
                                           size_t contact_id_len, hu_persona_delta_kind_t kind,
                                           const char *key, const char *value, float confidence,
                                           const char *source, int64_t proposed_at_ms,
                                           int64_t *out_delta_id) {
#ifndef HU_ENABLE_SQLITE
    (void)m;
    (void)contact_id;
    (void)contact_id_len;
    (void)kind;
    (void)key;
    (void)value;
    (void)confidence;
    (void)source;
    (void)proposed_at_ms;
    (void)out_delta_id;
    return HU_ERR_NOT_SUPPORTED;
#else
    if (!m || !contact_id || contact_id_len == 0 || !value || kind >= HU_PERSONA_DELTA_MAX)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = hu_memory_facade_sqlite_db(m);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    return persona_delta_propose_db(db, contact_id, contact_id_len, kind, key, value, confidence,
                                    source, proposed_at_ms, out_delta_id);
#endif
}

hu_error_t hu_persona_delta_list(struct hu_graph *graph, hu_allocator_t *alloc,
                                 const char *contact_id, size_t contact_id_len,
                                 hu_persona_delta_status_t status_filter, size_t limit,
                                 hu_persona_delta_t **out, size_t *out_count) {
    if (!graph || !alloc || !contact_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    if (limit == 0)
        limit = 64;

#ifdef HU_ENABLE_SQLITE
    struct sqlite3 *db = hu_memory_sqlite_from_graph((struct hu_graph *)graph);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    return persona_delta_list_db(db, alloc, contact_id, contact_id_len, status_filter, limit, out,
                                 out_count);
#else
    (void)contact_id_len;
    (void)status_filter;
    (void)limit;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t hu_persona_delta_list_facade(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                        const char *contact_id, size_t contact_id_len,
                                        hu_persona_delta_status_t status_filter, size_t limit,
                                        hu_persona_delta_t **out, size_t *out_count) {
#ifndef HU_ENABLE_SQLITE
    (void)m;
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    (void)status_filter;
    (void)limit;
    (void)out;
    (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
#else
    if (!m || !alloc || !contact_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    if (limit == 0)
        limit = 64;
    struct sqlite3 *db = hu_memory_facade_sqlite_db(m);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    return persona_delta_list_db(db, alloc, contact_id, contact_id_len, status_filter, limit, out,
                                 out_count);
#endif
}

void hu_persona_delta_free(hu_allocator_t *alloc, hu_persona_delta_t *deltas, size_t count) {
    if (!alloc || !deltas)
        return;
    alloc->free(alloc->ctx, deltas, sizeof(hu_persona_delta_t) * count);
}

#ifdef HU_ENABLE_SQLITE

/* Mark a delta with a new status + reason. */
static void mark_status(struct sqlite3 *db, int64_t id, hu_persona_delta_status_t st_new,
                        const char *reason, int64_t when_ms) {
    sqlite3_stmt *st = NULL;
    const char *sql =
        "UPDATE persona_deltas SET status = ?, status_reason = ?, applied_at = ? WHERE id = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int(st, 1, (int)st_new);
    sqlite3_bind_text(st, 2, reason ? reason : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, when_ms);
    sqlite3_bind_int64(st, 4, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* Count distinct evidence (deltas with the same kind + key + value pattern)
 * for corroboration scoring. */
static int corroboration_count(struct sqlite3 *db, const char *cid, int cid_len, int kind,
                               const char *key, const char *value) {
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT COUNT(*) FROM persona_deltas WHERE contact_id = ? AND kind = ? AND key = ? "
        "AND value = ? AND status IN (0,1)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, kind);
    sqlite3_bind_text(st, 3, key ? key : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, value ? value : "", -1, SQLITE_STATIC);
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* Count proposals from the same source within the last hour (rate limit). */
static int recent_source_count(struct sqlite3 *db, const char *cid, int cid_len, const char *source,
                               int64_t now_ms) {
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT COUNT(*) FROM persona_deltas WHERE contact_id = ? AND source = ? "
                      "AND proposed_at >= ?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, source ? source : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, now_ms - 3600 * 1000);
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

static hu_error_t persona_evolver_run_db(struct sqlite3 *db, const char *contact_id,
                                         size_t contact_id_len,
                                         const hu_persona_evolver_config_t *cfg,
                                         hu_persona_evolver_report_t *out_report) {
    if (!db || !contact_id || !out_report)
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t e = ensure_schema(db);
    if (e != HU_OK)
        return e;

    hu_persona_evolver_config_t local = cfg ? *cfg : hu_persona_evolver_default_config();
    int64_t now = now_ms_or(local.now_ms);
    if (local.apply_threshold <= 0.0f)
        local.apply_threshold = 0.75f;
    if (local.drop_threshold <= 0.0f)
        local.drop_threshold = 0.50f;
    if (local.corroboration_min == 0)
        local.corroboration_min = 3;
    if (local.rate_limit_per_hour == 0)
        local.rate_limit_per_hour = 10;
    if (local.max_apply == 0)
        local.max_apply = 32;

    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT id, kind, key, value, confidence, source FROM persona_deltas "
                      "WHERE contact_id = ? AND status = 0 ORDER BY proposed_at ASC LIMIT 256";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(st, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);

    size_t applied = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        out_report->proposed_total++;
        int64_t id = sqlite3_column_int64(st, 0);
        int kind = sqlite3_column_int(st, 1);
        const char *key = (const char *)sqlite3_column_text(st, 2);
        const char *value = (const char *)sqlite3_column_text(st, 3);
        float conf = (float)sqlite3_column_double(st, 4);
        const char *source = (const char *)sqlite3_column_text(st, 5);

        int recent = recent_source_count(db, contact_id, (int)contact_id_len, source, now);
        if (recent > (int)local.rate_limit_per_hour) {
            mark_status(db, id, HU_DELTA_STATUS_QUARANTINED, "rate-limit", now);
            out_report->quarantined++;
            continue;
        }

        if (conf < local.drop_threshold) {
            mark_status(db, id, HU_DELTA_STATUS_DROPPED, "low-confidence", now);
            out_report->dropped++;
            continue;
        }

        if (conf < local.apply_threshold) {
            int n = corroboration_count(db, contact_id, (int)contact_id_len, kind, key, value);
            if (n < (int)local.corroboration_min) {
                out_report->still_pending++;
                continue;
            }
        }

        if (applied >= local.max_apply) {
            out_report->still_pending++;
            continue;
        }
        char reason[120];
        snprintf(reason, sizeof(reason), "applied conf=%.2f", (double)conf);
        mark_status(db, id, HU_DELTA_STATUS_APPLIED, reason, now);

        /* Spec 4 Phase C / AC-TOM-5: record this persona-delta apply as
         * a self-change event against the contact. Magnitude carries the
         * delta's confidence so downstream callers can weight staleness
         * gaps (D-TOM-5). Errors are silently ignored — TOM persistence
         * failures must never block the evolver path. */
        char contact_id_buf[256];
        size_t cid_copy = contact_id_len < sizeof(contact_id_buf) - 1 ? contact_id_len
                                                                      : sizeof(contact_id_buf) - 1;
        memcpy(contact_id_buf, contact_id, cid_copy);
        contact_id_buf[cid_copy] = '\0';
        (void)hu_tom_self_change_events_init_table(db);
        (void)hu_tom_record_self_change_event(db, contact_id_buf, HU_TOM_SELF_CHANGE_PERSONA_DELTA,
                                              /* session_key */ NULL, 0,
                                              /* turn_number */ 0,
                                              /* magnitude */ (double)conf, now);

        out_report->applied++;
        applied++;
    }
    sqlite3_finalize(st);
    return HU_OK;
}

#endif

hu_error_t hu_persona_evolver_run(struct hu_graph *graph, const char *contact_id,
                                  size_t contact_id_len, const hu_persona_evolver_config_t *cfg,
                                  hu_persona_evolver_report_t *out_report) {
    if (!graph || !contact_id || !out_report)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out_report, 0, sizeof(*out_report));

#ifdef HU_ENABLE_SQLITE
    struct sqlite3 *db = hu_memory_sqlite_from_graph((struct hu_graph *)graph);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    return persona_evolver_run_db(db, contact_id, contact_id_len, cfg, out_report);
#else
    (void)contact_id_len;
    (void)cfg;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t hu_persona_evolver_run_facade(hu_memory_facade_t *m, const char *contact_id,
                                         size_t contact_id_len,
                                         const hu_persona_evolver_config_t *cfg,
                                         hu_persona_evolver_report_t *out_report) {
    if (!m || !contact_id || !out_report)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out_report, 0, sizeof(*out_report));
#ifndef HU_ENABLE_SQLITE
    (void)contact_id_len;
    (void)cfg;
    return HU_ERR_NOT_SUPPORTED;
#else
    struct sqlite3 *db = hu_memory_facade_sqlite_db(m);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    return persona_evolver_run_db(db, contact_id, contact_id_len, cfg, out_report);
#endif
}
