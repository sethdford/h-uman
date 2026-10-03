/*
 * src/memory/repos/job_queue_repo_sqlite.c
 *
 * SQLite-backed durable job queue. Contract: include/human/memory/job_queue_repo.h.
 * Design: docs/plans/2026-10-03-durable-job-queue.md §3–§4.
 */
#include "human/memory/job_queue_repo.h"

#ifdef HU_ENABLE_SQLITE

#include "human/core/log.h"
#include "human/memory/repo_util.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static bool jq_kind_is_valid(const char *kind) {
    return kind && (strcmp(kind, HU_JOB_KIND_INBOUND_HOLD) == 0 ||
                    strcmp(kind, HU_JOB_KIND_SCHED_SEND) == 0);
}

hu_error_t hu_job_queue_repo_ensure_schema(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    static const char *ddl =
        "CREATE TABLE IF NOT EXISTS jobs ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  kind TEXT NOT NULL CHECK (kind IN ('inbound_hold','sched_send')),"
        "  payload BLOB,"
        "  contact TEXT,"
        "  channel TEXT,"
        "  due_at INTEGER NOT NULL,"
        "  created_at INTEGER NOT NULL,"
        "  state TEXT NOT NULL DEFAULT 'pending' CHECK (state IN ('pending','claimed',"
        "    'sending','done','failed','unknown','expired','canceled','shadow')),"
        "  attempts INTEGER NOT NULL DEFAULT 0,"
        "  lease_until INTEGER,"
        "  idempotency_key TEXT NOT NULL,"
        "  last_error TEXT,"
        "  updated_at INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_jobs_state_due ON jobs(state, due_at);"
        /* A named UNIQUE index rather than an inline column constraint, so
         * scripts/check-silent-success.sh can see what makes OR IGNORE fire. */
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_jobs_key ON jobs(idempotency_key);";
    return hu_repo_exec_ddl(db, ddl);
}

/* Bind text, or NULL for a NULL/empty string. */
static void jq_bind_opt_text(sqlite3_stmt *st, int idx, const char *s) {
    if (s && s[0])
        sqlite3_bind_text(st, idx, s, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, idx);
}

static hu_error_t jq_lookup_id(sqlite3 *db, const char *key, int64_t *out_id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id FROM jobs WHERE idempotency_key=?1;", -1, &st, NULL) !=
        SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW)
        *out_id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (rc == SQLITE_ROW)
        return HU_OK;
    return rc == SQLITE_DONE ? HU_ERR_NOT_FOUND : HU_ERR_MEMORY_STORE;
}

hu_error_t hu_job_queue_repo_enqueue(sqlite3 *db, const hu_job_spec_t *spec, int64_t now,
                                     int64_t *out_id, bool *out_inserted) {
    if (out_inserted)
        *out_inserted = false;
    if (!db || !spec || !jq_kind_is_valid(spec->kind) || !spec->idempotency_key ||
        !spec->idempotency_key[0] || strlen(spec->idempotency_key) >= HU_JOB_KEY_MAX ||
        spec->payload_len > HU_JOB_PAYLOAD_MAX || (spec->payload_len > 0 && !spec->payload))
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "INSERT OR IGNORE INTO jobs (kind, payload, contact, channel, due_at, "
                           "created_at, state, idempotency_key, updated_at) "
                           "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?6);",
                           -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(st, 1, spec->kind, -1, SQLITE_STATIC);
    if (spec->payload_len > 0)
        sqlite3_bind_blob(st, 2, spec->payload, (int)spec->payload_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 2);
    jq_bind_opt_text(st, 3, spec->contact);
    jq_bind_opt_text(st, 4, spec->channel);
    sqlite3_bind_int64(st, 5, spec->due_at);
    sqlite3_bind_int64(st, 6, now);
    sqlite3_bind_text(st, 7, spec->shadow ? "shadow" : "pending", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 8, spec->idempotency_key, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_STORE;
    /* OR IGNORE reports SQLITE_DONE either way; changes() tells them apart. */
    bool inserted = sqlite3_changes(db) == 1;
    if (out_inserted)
        *out_inserted = inserted;
    if (!out_id)
        return HU_OK;
    if (inserted) {
        *out_id = sqlite3_last_insert_rowid(db);
        return HU_OK;
    }
    return jq_lookup_id(db, spec->idempotency_key, out_id);
}

/* Run one UPDATE whose ?1..?n are int64 binds; *out_changes = rows changed. */
static hu_error_t jq_exec_i64(sqlite3 *db, const char *sql, const int64_t *binds, int n,
                              int64_t *out_changes) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    for (int i = 0; i < n; i++)
        sqlite3_bind_int64(st, i + 1, binds[i]);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_STORE;
    if (out_changes)
        *out_changes = sqlite3_changes(db);
    return HU_OK;
}

static void jq_copy_text(char *dst, size_t cap, const unsigned char *src) {
    snprintf(dst, cap, "%s", src ? (const char *)src : "");
}

/* Columns: id, kind, payload, contact, channel, due_at, created_at, attempts,
 * idempotency_key. lease_until is set by the caller. */
static void jq_read_job(sqlite3_stmt *st, hu_job_t *j) {
    memset(j, 0, sizeof(*j));
    j->id = sqlite3_column_int64(st, 0);
    jq_copy_text(j->kind, sizeof(j->kind), sqlite3_column_text(st, 1));
    const void *blob = sqlite3_column_blob(st, 2);
    int blen = sqlite3_column_bytes(st, 2);
    if (blob && blen > 0) {
        size_t n = (size_t)blen < sizeof(j->payload) ? (size_t)blen : sizeof(j->payload);
        memcpy(j->payload, blob, n);
        j->payload_len = n;
    }
    jq_copy_text(j->contact, sizeof(j->contact), sqlite3_column_text(st, 3));
    jq_copy_text(j->channel, sizeof(j->channel), sqlite3_column_text(st, 4));
    j->due_at = sqlite3_column_int64(st, 5);
    j->created_at = sqlite3_column_int64(st, 6);
    j->attempts = sqlite3_column_int64(st, 7);
    jq_copy_text(j->idempotency_key, sizeof(j->idempotency_key), sqlite3_column_text(st, 8));
}

static hu_error_t jq_select_due(sqlite3 *db, const char *kind, int64_t now, hu_job_t *out,
                                size_t cap, size_t *out_n) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT id, kind, payload, contact, channel, due_at, created_at, "
                           "attempts, idempotency_key FROM jobs WHERE state='pending' "
                           "AND due_at <= ?1 AND (?2 IS NULL OR kind = ?2) "
                           "ORDER BY due_at, id LIMIT ?3;",
                           -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_int64(st, 1, now);
    jq_bind_opt_text(st, 2, kind);
    sqlite3_bind_int64(st, 3, (int64_t)cap);
    int rc = SQLITE_DONE;
    while (*out_n < cap && (rc = sqlite3_step(st)) == SQLITE_ROW)
        jq_read_job(st, &out[(*out_n)++]);
    if (*out_n < cap && rc != SQLITE_DONE) {
        sqlite3_finalize(st);
        return HU_ERR_MEMORY_STORE;
    }
    sqlite3_finalize(st);
    return HU_OK;
}

/* Open this module's own write transaction. The memory.db handle is shared
 * (FULLMUTEX) with gateway worker threads: if one of them has a transaction
 * open, our statements would join it and its ROLLBACK could undo a committed
 * `sending`, which recovery would then hand out again. Refuse instead. The
 * BEGIN itself is the atomic test: it fails when a transaction is open. */
static hu_error_t jq_begin(sqlite3 *db) {
    static atomic_bool warned = false;
    if (sqlite3_get_autocommit(db) &&
        sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) == SQLITE_OK)
        return HU_OK;
    if (sqlite3_get_autocommit(db))
        return HU_ERR_MEMORY_STORE; /* BEGIN failed for another reason (locked) */
    hu_log_warn_once(&warned, "jobq", NULL,
                     "[jobq] refused: a transaction is already open on the shared memory.db "
                     "connection; the queue never joins another subsystem's transaction");
    return HU_ERR_IO_BUSY;
}

/* COMMIT on success, else ROLLBACK. A failed COMMIT is an error: the caller
 * must not act (send) on a transition that did not stick. */
static hu_error_t jq_end(sqlite3 *db, hu_error_t err) {
    if (err == HU_OK && sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK)
        return HU_OK;
    (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    return err == HU_OK ? HU_ERR_MEMORY_STORE : err;
}

hu_error_t hu_job_queue_repo_claim_due(sqlite3 *db, const char *kind, int64_t now, int64_t lease_s,
                                       hu_job_t *out, size_t cap, size_t *out_n) {
    if (!db || !out || cap == 0 || !out_n || lease_s <= 0 || (kind && !jq_kind_is_valid(kind)))
        return HU_ERR_INVALID_ARGUMENT;
    *out_n = 0;
    hu_error_t err = jq_begin(db);
    if (err != HU_OK)
        return err;
    /* A claim whose lease ran out never reached mark_sending (or it would be
     * in `sending`), so nothing was sent: it is safe to hand out again. */
    int64_t requeue[2] = {now, now};
    err = jq_exec_i64(db,
                      "UPDATE jobs SET state='pending', lease_until=NULL, updated_at=?1 "
                      "WHERE state='claimed' AND lease_until < ?2;",
                      requeue, 2, NULL);
    if (err == HU_OK)
        err = jq_select_due(db, kind, now, out, cap, out_n);
    int64_t lease_until = now + lease_s;
    for (size_t i = 0; err == HU_OK && i < *out_n; i++) {
        int64_t claim[3] = {lease_until, now, out[i].id};
        err = jq_exec_i64(db,
                          "UPDATE jobs SET state='claimed', lease_until=?1, updated_at=?2 "
                          "WHERE id=?3 AND state='pending';",
                          claim, 3, NULL);
        out[i].lease_until = lease_until;
    }
    err = jq_end(db, err);
    if (err != HU_OK)
        *out_n = 0;
    return err;
}

/* Exactly-one-row fenced transition: HU_ERR_NOT_FOUND when nothing matched. */
static hu_error_t jq_update_one(sqlite3 *db, const char *sql, const int64_t *binds, int n) {
    int64_t changed = 0;
    hu_error_t err = jq_exec_i64(db, sql, binds, n, &changed);
    if (err != HU_OK)
        return err;
    return changed == 1 ? HU_OK : HU_ERR_NOT_FOUND;
}

hu_error_t hu_job_queue_repo_mark_sending(sqlite3 *db, int64_t id, int64_t lease_until,
                                          int64_t now) {
    if (!db || id <= 0)
        return HU_ERR_INVALID_ARGUMENT;
    int64_t b[3] = {now, id, lease_until};
    /* Own transaction: the row is durably `sending` once COMMIT returns. */
    hu_error_t err = jq_begin(db);
    if (err != HU_OK)
        return err;
    err = jq_update_one(db,
                        "UPDATE jobs SET state='sending', attempts=attempts+1, lease_until=NULL, "
                        "updated_at=?1 WHERE id=?2 AND state='claimed' AND lease_until=?3 "
                        "AND lease_until >= ?1;",
                        b, 3);
    return jq_end(db, err);
}

hu_error_t hu_job_queue_repo_release(sqlite3 *db, int64_t id, int64_t lease_until,
                                     int64_t new_due_at, int64_t now) {
    if (!db || id <= 0 || new_due_at < 0)
        return HU_ERR_INVALID_ARGUMENT;
    int64_t b[4] = {now, id, lease_until, new_due_at};
    return jq_update_one(db,
                         "UPDATE jobs SET state='pending', lease_until=NULL, updated_at=?1, "
                         "due_at=CASE WHEN ?4 > 0 THEN ?4 ELSE due_at END "
                         "WHERE id=?2 AND state='claimed' AND lease_until=?3;",
                         b, 4);
}

/* Allowed source states for each terminal state, as an SQL IN list. */
static const char *jq_finish_sources(const char *state) {
    if (!state)
        return NULL;
    if (strcmp(state, HU_JOB_STATE_DONE) == 0)
        return "('sending')";
    if (strcmp(state, HU_JOB_STATE_FAILED) == 0)
        return "('claimed','sending')";
    if (strcmp(state, HU_JOB_STATE_CANCELED) == 0 || strcmp(state, HU_JOB_STATE_EXPIRED) == 0)
        return "('pending','claimed')";
    return NULL;
}

hu_error_t hu_job_queue_repo_finish(sqlite3 *db, int64_t id, const char *state, int64_t lease_until,
                                    const char *last_error, int64_t now) {
    const char *sources = jq_finish_sources(state);
    if (!db || id <= 0 || !sources)
        return HU_ERR_INVALID_ARGUMENT;
    char sql[256];
    snprintf(sql, sizeof(sql),
             "UPDATE jobs SET state=?1, last_error=?2, lease_until=NULL, updated_at=?3 "
             "WHERE id=?4 AND state IN %s AND (state <> 'claimed' OR lease_until = ?5);",
             sources);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(st, 1, state, -1, SQLITE_STATIC);
    char err_buf[HU_JOB_LAST_ERR_MAX];
    if (last_error) {
        snprintf(err_buf, sizeof(err_buf), "%s", last_error);
        sqlite3_bind_text(st, 2, err_buf, -1, SQLITE_STATIC);
    } else {
        sqlite3_bind_null(st, 2);
    }
    sqlite3_bind_int64(st, 3, now);
    sqlite3_bind_int64(st, 4, id);
    sqlite3_bind_int64(st, 5, lease_until);
    return hu_repo_step_update_one(db, st);
}

hu_error_t hu_job_queue_repo_recover_on_start(sqlite3 *db, int64_t now, int64_t *out_unknown,
                                              int64_t *out_requeued) {
    if (out_unknown)
        *out_unknown = 0;
    if (out_requeued)
        *out_requeued = 0;
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t err = jq_begin(db);
    if (err != HU_OK)
        return err;
    int64_t unknown = 0, requeued = 0;
    int64_t b[2] = {now, now};
    /* The send may or may not have happened: at-most-once means never again. */
    err = jq_exec_i64(db,
                      "UPDATE jobs SET state='unknown', last_error='in flight at "
                      "restart', updated_at=?1 WHERE state='sending';",
                      b, 1, &unknown);
    if (err == HU_OK)
        err = jq_exec_i64(db,
                          "UPDATE jobs SET state='pending', lease_until=NULL, updated_at=?1 "
                          "WHERE state='claimed' AND lease_until < ?2;",
                          b, 2, &requeued);
    err = jq_end(db, err);
    if (err == HU_OK && out_unknown)
        *out_unknown = unknown;
    if (err == HU_OK && out_requeued)
        *out_requeued = requeued;
    return err;
}

hu_error_t hu_job_queue_repo_expire_older_than(sqlite3 *db, const char *kind, int64_t cutoff,
                                               int64_t now, int64_t *out_n) {
    if (out_n)
        *out_n = 0;
    if (!db || !jq_kind_is_valid(kind))
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *st = NULL;
    /* A held message ages from when it was held; a scheduled send ages from
     * when it was due, so one scheduled far ahead never expires early. */
    if (sqlite3_prepare_v2(db,
                           "UPDATE jobs SET state='expired', last_error='max age', updated_at=?1 "
                           "WHERE state='pending' AND kind = ?3 AND (CASE kind WHEN 'sched_send' "
                           "THEN due_at ELSE created_at END) < ?2;",
                           -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_int64(st, 1, now);
    sqlite3_bind_int64(st, 2, cutoff);
    jq_bind_opt_text(st, 3, kind);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_STORE;
    if (out_n)
        *out_n = sqlite3_changes(db);
    return HU_OK;
}

hu_error_t hu_job_queue_repo_count_due(sqlite3 *db, const char *kind, int64_t now, int64_t *out_n) {
    if (!db || !out_n || !jq_kind_is_valid(kind))
        return HU_ERR_INVALID_ARGUMENT;
    *out_n = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT COUNT(*) FROM jobs WHERE state='pending' AND kind=?1 AND "
                           "due_at <= ?2;",
                           -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(st, 1, kind, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, now);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW)
        *out_n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? HU_OK : HU_ERR_MEMORY_STORE;
}

hu_error_t hu_job_queue_repo_counts(sqlite3 *db, hu_job_queue_counts_t *out) {
    if (!db || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT state, COUNT(*) FROM jobs GROUP BY state;", -1, &st, NULL) !=
        SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    struct {
        const char *name;
        int64_t *slot;
    } map[] = {{"pending", &out->pending}, {"claimed", &out->claimed},   {"sending", &out->sending},
               {"done", &out->done},       {"failed", &out->failed},     {"unknown", &out->unknown},
               {"expired", &out->expired}, {"canceled", &out->canceled}, {"shadow", &out->shadow}};
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *s = (const char *)sqlite3_column_text(st, 0);
        for (size_t i = 0; s && i < sizeof(map) / sizeof(map[0]); i++)
            if (strcmp(s, map[i].name) == 0)
                *map[i].slot = sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_STORE;
}

#endif /* HU_ENABLE_SQLITE */
