#ifndef HU_MEMORY_JOB_QUEUE_REPO_H
#define HU_MEMORY_JOB_QUEUE_REPO_H

/*
 * Durable job queue — one memory.db table (`jobs`) that the daemon uses for
 * work that must survive a restart: held inbound messages while the model is
 * down, and scheduled contact sends. Design:
 * docs/plans/2026-10-03-durable-job-queue.md (§3 schema, §4 claim/lease).
 *
 * Delivery is AT-MOST-ONCE for contact sends (owner ruling: a missed message
 * beats a duplicate). The state machine:
 *
 *   enqueue ──► pending ──claim_due──► claimed ──mark_sending──► sending ──finish──► done|failed
 *                  ▲                      │                         │
 *                  └──── release ─────────┘                         └─ recover_on_start ─► unknown
 *   pending|claimed ──finish──► canceled|expired       pending ──expire_older_than──► expired
 *   enqueue(shadow=true) ──► shadow (a log row; never claimed)
 *
 * - claim_due moves due pending rows to claimed inside ONE transaction and
 *   stamps lease_until = now + lease_s. That lease_until is also the claim's
 *   fencing token: mark_sending / release only apply when the row is still
 *   claimed with the same lease_until, so a claimer whose lease expired (and
 *   whose row another pass re-claimed) can never send it.
 * - mark_sending commits state='sending' and attempts+1 BEFORE the caller
 *   calls the channel's send. If the process dies after that commit, the row
 *   is found in `sending` at the next start and moved to `unknown` — never
 *   re-claimed, never re-sent.
 * - A claimed row whose lease expired is returned to pending (by claim_due at
 *   runtime and by recover_on_start); nothing was sent for it, so retrying is
 *   safe.
 *
 * Recall (memory) bounded context: the legal home for a raw sqlite3 include
 * (sqlite-includer-ratchet.md). Free-function shape like
 * outbound_sends_repo.h; claim pattern from reminder_repo.h.
 *
 * Callers run hu_job_queue_repo_ensure_schema once (daemon start); the other
 * functions do not re-run the DDL on every call.
 *
 * Transactions: claim_due, mark_sending and recover_on_start each run their
 * own BEGIN IMMEDIATE ... COMMIT and refuse with HU_ERR_IO_BUSY (logged once)
 * when the connection already has a transaction open. The daemon's memory.db
 * handle is shared (FULLMUTEX) with gateway worker threads; joining someone
 * else's transaction would let their ROLLBACK revert a committed `sending`
 * and turn it into a re-send. A failed COMMIT is reported as an error, so a
 * caller never sends on a transition that did not stick.
 *
 * Durability covers a process crash, not power loss: the shared connection
 * keeps SQLite's default sync settings, and on macOS fsync does not flush the
 * drive cache (PRAGMA fullfsync is off). See docs/guides/job-queue.md.
 */

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_JOB_KIND_INBOUND_HOLD "inbound_hold"
#define HU_JOB_KIND_SCHED_SEND   "sched_send"

#define HU_JOB_STATE_DONE     "done"
#define HU_JOB_STATE_FAILED   "failed"
#define HU_JOB_STATE_CANCELED "canceled"
#define HU_JOB_STATE_EXPIRED  "expired"

#define HU_JOB_KIND_MAX     16
#define HU_JOB_CONTACT_MAX  128
#define HU_JOB_CHANNEL_MAX  32
#define HU_JOB_KEY_MAX      192
#define HU_JOB_PAYLOAD_MAX  4096
#define HU_JOB_LAST_ERR_MAX 256

/* What enqueue writes. contact / channel may be NULL. payload may be NULL
 * when payload_len is 0; longer than HU_JOB_PAYLOAD_MAX is refused so a claim
 * never hands back a truncated payload. */
typedef struct hu_job_spec {
    const char *kind; /* HU_JOB_KIND_* */
    const void *payload;
    size_t payload_len;
    const char *contact;
    const char *channel;
    int64_t due_at;              /* unix seconds */
    const char *idempotency_key; /* non-empty, < HU_JOB_KEY_MAX */
    bool shadow;                 /* true: state='shadow' (never claimed) */
} hu_job_spec_t;

/* A claimed job. lease_until is the fencing token mark_sending / release
 * must present. */
typedef struct hu_job {
    int64_t id;
    char kind[HU_JOB_KIND_MAX];
    unsigned char payload[HU_JOB_PAYLOAD_MAX];
    size_t payload_len;
    char contact[HU_JOB_CONTACT_MAX];
    char channel[HU_JOB_CHANNEL_MAX];
    int64_t due_at;
    int64_t created_at;
    int64_t attempts;
    int64_t lease_until;
    char idempotency_key[HU_JOB_KEY_MAX];
} hu_job_t;

/* Row count per state. */
typedef struct hu_job_queue_counts {
    int64_t pending;
    int64_t claimed;
    int64_t sending;
    int64_t done;
    int64_t failed;
    int64_t unknown;
    int64_t expired;
    int64_t canceled;
    int64_t shadow;
} hu_job_queue_counts_t;

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotent CREATE TABLE/INDEX IF NOT EXISTS. */
hu_error_t hu_job_queue_repo_ensure_schema(sqlite3 *db);

/* INSERT OR IGNORE on idempotency_key. *out_inserted is false (and the
 * result still HU_OK) when a row with that key already exists; *out_id is
 * then the EXISTING row's id. Either out pointer may be NULL. */
hu_error_t hu_job_queue_repo_enqueue(sqlite3 *db, const hu_job_spec_t *spec, int64_t now,
                                     int64_t *out_id, bool *out_inserted);

/* In one transaction: return claimed rows whose lease expired (< now) to
 * pending, then claim up to `cap` pending rows with due_at <= now (oldest
 * due first), optionally of one `kind` (NULL = any), moving them to claimed
 * with lease_until = now + lease_s. */
hu_error_t hu_job_queue_repo_claim_due(sqlite3 *db, const char *kind, int64_t now, int64_t lease_s,
                                       hu_job_t *out, size_t cap, size_t *out_n);

/* claimed -> sending, attempts+1, committed before returning. Only when the
 * row is still claimed with `lease_until` and that lease has not expired
 * (lease_until >= now). HU_ERR_NOT_FOUND otherwise: the caller lost the
 * claim and MUST NOT send. */
hu_error_t hu_job_queue_repo_mark_sending(sqlite3 *db, int64_t id, int64_t lease_until,
                                          int64_t now);

/* Terminal transition with an optional last_error (NULL keeps none).
 *   done             from sending
 *   failed           from claimed or sending (explicit not-delivered)
 *   canceled|expired from pending or claimed
 * A `claimed` row is only finished when `lease_until` is that claim's token
 * (same fencing as mark_sending), so a stale worker cannot finish a row a
 * newer claim owns; pass 0 when the caller holds no claim. Any other state
 * string is HU_ERR_INVALID_ARGUMENT; a row not in an allowed source state (or
 * claimed under another token) is HU_ERR_NOT_FOUND. */
hu_error_t hu_job_queue_repo_finish(sqlite3 *db, int64_t id, const char *state, int64_t lease_until,
                                    const char *last_error, int64_t now);

/* claimed -> pending (a gate deferred it). No attempt is counted. Same
 * fencing as mark_sending. new_due_at > 0 reschedules; 0 keeps due_at. */
hu_error_t hu_job_queue_repo_release(sqlite3 *db, int64_t id, int64_t lease_until,
                                     int64_t new_due_at, int64_t now);

/* Daemon start, one transaction: every `sending` row -> unknown (its send
 * may or may not have happened; it is never re-claimed), and every claimed
 * row with an expired lease -> pending. Either out pointer may be NULL. */
hu_error_t hu_job_queue_repo_recover_on_start(sqlite3 *db, int64_t now, int64_t *out_unknown,
                                              int64_t *out_requeued);

/* pending rows of `kind` (required) older than `cutoff` -> expired. Age is
 * measured on created_at for inbound_hold (how long it has been held) and on
 * due_at for sched_send (how overdue it is), so a send scheduled far ahead
 * is never expired before it is due. *out_n = rows expired (may be NULL). */
hu_error_t hu_job_queue_repo_expire_older_than(sqlite3 *db, const char *kind, int64_t cutoff,
                                               int64_t now, int64_t *out_n);

/* pending rows of `kind` (required) with due_at <= now: what claim_due
 * would hand out. Lets a caller skip expensive work (a model probe) when
 * there is nothing to release. */
hu_error_t hu_job_queue_repo_count_due(sqlite3 *db, const char *kind, int64_t now, int64_t *out_n);

hu_error_t hu_job_queue_repo_counts(sqlite3 *db, hu_job_queue_counts_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HU_ENABLE_SQLITE */

#endif /* HU_MEMORY_JOB_QUEUE_REPO_H */
