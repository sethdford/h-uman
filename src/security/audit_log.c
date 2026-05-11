/*
 * W15 — SQLite-backed audit log implementation.
 *
 * Schema creation uses sqlite3_prepare_v2 / step / finalize (never
 * sqlite3_exec). All bind calls use SQLITE_STATIC (NULL constant) for
 * string/blob parameters — never SQLITE_TRANSIENT.
 *
 * NOT in first commit (follow-up PRs within W15):
 *   - HMAC per-row tamper detection.
 *   - Encryption under the keystore.
 *   - CLI subcommands.
 */

#include "human/security/audit_log.h"
#include "human/core/error.h"

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── struct ─────────────────────────────────────────────────────────────── */

struct hu_audit_log {
    hu_allocator_t *alloc;
#ifdef HU_ENABLE_SQLITE
    sqlite3 *db;
#endif
    char contact_id[256]; /* default contact scope for appended events */
};

/* ── SQLite helpers ─────────────────────────────────────────────────────── */

#ifdef HU_ENABLE_SQLITE

static const char *const k_schema_stmts[] = {
    "CREATE TABLE IF NOT EXISTS audit_log ("
    "id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "contact_id  TEXT    NOT NULL DEFAULT '',"
    "operation   TEXT    NOT NULL,"
    "kind        INTEGER NOT NULL,"
    "target_id   INTEGER,"
    "actor       TEXT    NOT NULL,"
    "occurred_at INTEGER NOT NULL,"
    "summary     TEXT"
    ")",
    "CREATE INDEX IF NOT EXISTS idx_audit_actor "
    "ON audit_log(actor)",
    "CREATE INDEX IF NOT EXISTS idx_audit_occurred "
    "ON audit_log(occurred_at)",
    NULL,
};

static hu_error_t run_ddl(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE || rc == SQLITE_OK) ? HU_OK : HU_ERR_IO;
}

static hu_error_t ensure_schema(sqlite3 *db) {
    for (size_t i = 0; k_schema_stmts[i]; i++) {
        hu_error_t err = run_ddl(db, k_schema_stmts[i]);
        if (err != HU_OK)
            return err;
    }
    return HU_OK;
}

/* Map enum to canonical string stored in the DB. */
static const char *op_to_str(hu_audit_operation_t op) {
    switch (op) {
    case HU_AUDIT_OP_READ:   return "read";
    case HU_AUDIT_OP_WRITE:  return "write";
    case HU_AUDIT_OP_ERASE:  return "erase";
    case HU_AUDIT_OP_EXPORT: return "export";
    default:                  return "unknown";
    }
}

static hu_audit_operation_t str_to_op(const char *s) {
    if (!s)                         return HU_AUDIT_OP_READ;
    if (strcmp(s, "write")  == 0)   return HU_AUDIT_OP_WRITE;
    if (strcmp(s, "erase")  == 0)   return HU_AUDIT_OP_ERASE;
    if (strcmp(s, "export") == 0)   return HU_AUDIT_OP_EXPORT;
    return HU_AUDIT_OP_READ;
}

#endif /* HU_ENABLE_SQLITE */

/* ── lifecycle ──────────────────────────────────────────────────────────── */

hu_error_t hu_audit_log_open(hu_allocator_t *alloc, const char *db_path,
                              const char *contact_id, hu_audit_log_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

#ifndef HU_ENABLE_SQLITE
    (void)db_path;
    (void)contact_id;
    return HU_ERR_NOT_SUPPORTED;
#else
    hu_audit_log_t *log = alloc->alloc(alloc->ctx, sizeof(*log));
    if (!log)
        return HU_ERR_OUT_OF_MEMORY;
    memset(log, 0, sizeof(*log));
    log->alloc = alloc;

    if (contact_id && *contact_id)
        strncpy(log->contact_id, contact_id, sizeof(log->contact_id) - 1);

    int rc = sqlite3_open(db_path ? db_path : ":memory:", &log->db);
    if (rc != SQLITE_OK) {
        alloc->free(alloc->ctx, log, sizeof(*log));
        return HU_ERR_IO;
    }

    hu_error_t err = ensure_schema(log->db);
    if (err != HU_OK) {
        sqlite3_close(log->db);
        alloc->free(alloc->ctx, log, sizeof(*log));
        return err;
    }

    *out = log;
    return HU_OK;
#endif /* HU_ENABLE_SQLITE */
}

void hu_audit_log_close(hu_audit_log_t *log, hu_allocator_t *alloc) {
    if (!log)
        return;
#ifdef HU_ENABLE_SQLITE
    if (log->db)
        sqlite3_close(log->db);
#endif
    alloc->free(alloc->ctx, log, sizeof(*log));
}

/* ── append ─────────────────────────────────────────────────────────────── */

hu_error_t hu_audit_log_append(hu_audit_log_t *log,
                                const hu_audit_log_event_t *ev) {
    if (!log || !ev)
        return HU_ERR_INVALID_ARGUMENT;
#ifndef HU_ENABLE_SQLITE
    return HU_ERR_NOT_SUPPORTED;
#else
    const char *sql =
        "INSERT INTO audit_log"
        " (contact_id, operation, kind, target_id, actor, occurred_at, summary)"
        " VALUES (?, ?, ?, ?, ?, ?, ?)";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(log->db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    /* Use the event's contact_id if set, else fall back to the log default. */
    const char *cid = (ev->contact_id && *ev->contact_id)
                          ? ev->contact_id
                          : log->contact_id;

    int64_t occurred_at = ev->occurred_at;
    if (occurred_at == 0) {
        /* Default to current time in Unix ms. */
        struct timespec ts = {0, 0};
#if defined(CLOCK_REALTIME)
        clock_gettime(CLOCK_REALTIME, &ts);
        occurred_at = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#else
        occurred_at = (int64_t)time(NULL) * 1000;
#endif
    }

    sqlite3_bind_text(st, 1, cid,                    -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, op_to_str(ev->operation), -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (int64_t)ev->kind);
    if (ev->target_id)
        sqlite3_bind_int64(st, 4, ev->target_id);
    else
        sqlite3_bind_null(st, 4);
    sqlite3_bind_text(st, 5, ev->actor   ? ev->actor   : "unknown", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 6, occurred_at);
    if (ev->summary && *ev->summary)
        sqlite3_bind_text(st, 7, ev->summary, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 7);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_IO;
#endif /* HU_ENABLE_SQLITE */
}

/* ── query ──────────────────────────────────────────────────────────────── */

#ifdef HU_ENABLE_SQLITE

/* Duplicate a SQLite column text value using the given allocator. */
static char *dup_col(hu_allocator_t *alloc, sqlite3_stmt *st, int col) {
    const char *s = (const char *)sqlite3_column_text(st, col);
    if (!s)
        return NULL;
    size_t len = strlen(s) + 1;
    char *copy = alloc->alloc(alloc->ctx, len);
    if (copy)
        memcpy(copy, s, len);
    return copy;
}

#endif

hu_error_t hu_audit_log_query(hu_audit_log_t *log, const hu_audit_query_t *q,
                               hu_allocator_t *alloc,
                               hu_audit_log_event_t **out, size_t *out_count) {
    if (!log || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

#ifndef HU_ENABLE_SQLITE
    (void)q;
    (void)alloc;
    return HU_ERR_NOT_SUPPORTED;
#else
    /* Build a dynamic SQL query based on which filters are active. */
    char sql[512];
    int  n = 0;
    bool has_actor = (q && q->actor && *q->actor);
    bool has_since = (q && q->since_ms > 0);

    n += snprintf(sql + n, sizeof(sql) - (size_t)n,
                  "SELECT contact_id, operation, kind, target_id, actor,"
                  " occurred_at, summary"
                  " FROM audit_log");

    if (has_actor || has_since) {
        n += snprintf(sql + n, sizeof(sql) - (size_t)n, " WHERE");
        bool first = true;
        if (has_actor) {
            n += snprintf(sql + n, sizeof(sql) - (size_t)n, " actor = ?");
            first = false;
        }
        if (has_since) {
            n += snprintf(sql + n, sizeof(sql) - (size_t)n,
                          "%s occurred_at >= ?", first ? "" : " AND");
        }
    }

    n += snprintf(sql + n, sizeof(sql) - (size_t)n, " ORDER BY id ASC");

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(log->db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    /* Bind filter parameters. */
    int bind_idx = 1;
    if (has_actor)
        sqlite3_bind_text(st, bind_idx++, q->actor, -1, SQLITE_STATIC);
    if (has_since)
        sqlite3_bind_int64(st, bind_idx++, q->since_ms);

    /* Collect results — two-pass: count then allocate. */
    size_t capacity = 16;
    size_t count = 0;
    hu_audit_log_event_t *events = alloc->alloc(alloc->ctx,
                                             capacity * sizeof(*events));
    if (!events) {
        sqlite3_finalize(st);
        return HU_ERR_OUT_OF_MEMORY;
    }

    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (count == capacity) {
            size_t new_cap = capacity * 2;
            hu_audit_log_event_t *grown = alloc->alloc(alloc->ctx,
                                                    new_cap * sizeof(*events));
            if (!grown) {
                /* Free what we have so far, then bail. */
                for (size_t i = 0; i < count; i++) {
                    alloc->free(alloc->ctx, events[i].contact_id,
                                events[i].contact_id
                                    ? strlen(events[i].contact_id) + 1 : 0);
                    alloc->free(alloc->ctx, events[i].actor,
                                events[i].actor
                                    ? strlen(events[i].actor) + 1 : 0);
                    alloc->free(alloc->ctx, events[i].summary,
                                events[i].summary
                                    ? strlen(events[i].summary) + 1 : 0);
                }
                alloc->free(alloc->ctx, events, capacity * sizeof(*events));
                sqlite3_finalize(st);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(grown, events, count * sizeof(*events));
            alloc->free(alloc->ctx, events, capacity * sizeof(*events));
            events   = grown;
            capacity = new_cap;
        }

        hu_audit_log_event_t *ev = &events[count++];
        memset(ev, 0, sizeof(*ev));

        ev->contact_id  = dup_col(alloc, st, 0);
        ev->operation   = str_to_op((const char *)sqlite3_column_text(st, 1));
        ev->kind        = (hu_memory_kind_t)sqlite3_column_int(st, 2);
        ev->target_id   = sqlite3_column_int64(st, 3);
        ev->actor       = dup_col(alloc, st, 4);
        ev->occurred_at = sqlite3_column_int64(st, 5);
        ev->summary     = dup_col(alloc, st, 6);
    }

    sqlite3_finalize(st);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        hu_audit_log_events_free(alloc, events, count);
        alloc->free(alloc->ctx, events, capacity * sizeof(*events));
        return HU_ERR_IO;
    }

    if (count == 0) {
        /* No matching events. Free the pre-allocated buffer so callers
         * can rely on count==0 meaning nothing to free (matches the
         * fix pattern in src/memory/engines/sqlite.c::impl_list and
         * src/memory/graph.c). */
        alloc->free(alloc->ctx, events, capacity * sizeof(*events));
        *out = NULL;
        *out_count = 0;
        return HU_OK;
    }

    *out       = events;
    *out_count = count;
    return HU_OK;
#endif /* HU_ENABLE_SQLITE */
}

/* ── free ───────────────────────────────────────────────────────────────── */

void hu_audit_log_events_free(hu_allocator_t *alloc,
                           hu_audit_log_event_t *events, size_t count) {
    if (!alloc || !events || count == 0)
        return;
    for (size_t i = 0; i < count; i++) {
        if (events[i].contact_id)
            alloc->free(alloc->ctx, events[i].contact_id,
                        strlen(events[i].contact_id) + 1);
        if (events[i].actor)
            alloc->free(alloc->ctx, events[i].actor,
                        strlen(events[i].actor) + 1);
        if (events[i].summary)
            alloc->free(alloc->ctx, events[i].summary,
                        strlen(events[i].summary) + 1);
    }
    /* Note: caller frees the array itself (or passes it back through
     * hu_audit_log_query's alloc). We free it here for convenience. */
    alloc->free(alloc->ctx, events, count * sizeof(*events));
}
