/*
 * W15 — Audit log for memory operations.
 *
 * Every memory read/write/erase/export appends a row to an immutable,
 * append-only SQLite table. The log can be queried with optional filters.
 *
 * Schema:
 *   CREATE TABLE IF NOT EXISTS audit_log (
 *       id           INTEGER PRIMARY KEY AUTOINCREMENT,
 *       contact_id   TEXT    NOT NULL DEFAULT '',
 *       operation    TEXT    NOT NULL,
 *       kind         INTEGER NOT NULL,
 *       target_id    INTEGER,
 *       actor        TEXT    NOT NULL,
 *       occurred_at  INTEGER NOT NULL,
 *       summary      TEXT
 *   );
 *
 * NOT in first commit (follow-up PRs within W15):
 *   - HMAC-per-row tamper detection.
 *   - Encryption of the audit log under the keystore.
 *   - CLI subcommands audit/export/forget.
 */
#ifndef HU_AUDIT_LOG_H
#define HU_AUDIT_LOG_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/memory.h"  /* hu_memory_kind_t */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Discriminator for the operation field. */
typedef enum hu_audit_operation {
    HU_AUDIT_OP_READ   = 0,
    HU_AUDIT_OP_WRITE  = 1,
    HU_AUDIT_OP_ERASE  = 2,
    HU_AUDIT_OP_EXPORT = 3,
} hu_audit_operation_t;

/* A single audit event.
 *
 * When passed to hu_audit_log_append: strings are borrowed (caller retains
 * ownership; must remain valid for the duration of the call).
 *
 * When returned from hu_audit_log_query: strings are owned by the struct;
 * free the entire result set with hu_audit_events_free. */
typedef struct hu_audit_event {
    hu_audit_operation_t operation;
    hu_memory_kind_t     kind;
    int64_t              target_id;   /* 0 if not applicable */
    char                *actor;       /* who triggered the operation */
    int64_t              occurred_at; /* Unix ms timestamp */
    char                *summary;     /* nullable human-readable note */
    char                *contact_id;  /* user/contact scope */
} hu_audit_event_t;

/* Query filter for hu_audit_log_query. Set a field to NULL / 0 to skip. */
typedef struct hu_audit_query {
    const char *actor;    /* NULL → all actors */
    int64_t     since_ms; /* 0 → all time */
} hu_audit_query_t;

/* Opaque audit log handle. */
typedef struct hu_audit_log hu_audit_log_t;

/* Open a SQLite-backed audit log. `db_path` is the path to the SQLite file;
 * pass NULL for an in-memory database (useful in tests). `contact_id` is the
 * default user/contact scope stamped on appended events when the event's own
 * contact_id is NULL or empty. */
hu_error_t hu_audit_log_open(hu_allocator_t *alloc, const char *db_path,
                              const char *contact_id, hu_audit_log_t **out);

/* Release resources and close the database connection. */
void hu_audit_log_close(hu_audit_log_t *log, hu_allocator_t *alloc);

/* Append one event to the log. All string fields in `ev` are borrowed. */
hu_error_t hu_audit_log_append(hu_audit_log_t *log, const hu_audit_event_t *ev);

/* Query the log. Allocates *out (array of hu_audit_event_t, *out_count long)
 * via `alloc`. Strings inside each event are also allocated; free the whole
 * result set with hu_audit_events_free. */
hu_error_t hu_audit_log_query(hu_audit_log_t *log, const hu_audit_query_t *q,
                               hu_allocator_t *alloc,
                               hu_audit_event_t **out, size_t *out_count);

/* Free a result set returned by hu_audit_log_query. */
void hu_audit_events_free(hu_allocator_t *alloc,
                           hu_audit_event_t *events, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* HU_AUDIT_LOG_H */
