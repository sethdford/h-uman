/*
 * US-43.3 — iMessage non-allowlisted courtesy reply.
 *
 * Single polite courtesy reply per unknown handle, gated by per-handle 24h dedup
 * and a 50/day aggregate cap. Outbound delivery happens off-thread via a side-
 * channel ring drained by the agent loop (NOT the poll thread) to avoid re-entry.
 *
 * The decision itself is a pure predicate (`hu_imessage_should_courtesy_reply`)
 * over four boolean inputs — see `.claude/rules/security-predicate-extraction.md`.
 * All ambiguous I/O states must surface as `dedup_io_ok=false`, which fail-CLOSES
 * the predicate (AC-43.3.4).
 */
#ifndef HU_CHANNELS_IMESSAGE_COURTESY_H
#define HU_CHANNELS_IMESSAGE_COURTESY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "human/core/allocator.h"
#include "human/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-handle dedup window: a handle that received a courtesy reply less than
 * 24 hours ago is silenced. */
#define HU_IMESSAGE_COURTESY_PER_HANDLE_HOURS 24.0

/* Aggregate-per-UTC-day cap on courtesy replies — system-wide, regardless of
 * handle. A determined spoofer hitting 50 distinct handles in one UTC day is
 * blocked on handle #51. The day boundary is `floor(epoch_secs/86400)` UTC. */
#define HU_IMESSAGE_COURTESY_AGGREGATE_PER_DAY 50

/* Maximum entries the pending-courtesy ring can hold. Drained per agent-loop
 * tick. Sized at the per-day cap so a worst-case spoof storm cannot lose
 * pending sends silently. */
#define HU_IMESSAGE_COURTESY_RING_CAPACITY HU_IMESSAGE_COURTESY_AGGREGATE_PER_DAY

/* Maximum length of a courtesy reply body. */
#define HU_IMESSAGE_COURTESY_REPLY_MAX 256

/* Maximum length of a target handle stored in the ring. */
#define HU_IMESSAGE_COURTESY_HANDLE_MAX 128

/*
 * The security decision. Pure: same inputs → same output.
 *
 *   handle_in_allowlist   true ⇒ caller MUST NOT route through this predicate;
 *                         defense-in-depth, predicate returns false.
 *   hours_since_last_reply how long since this handle last received a courtesy
 *                         reply (read from dedup log under flock).
 *   aggregate_today       count of courtesy replies sent in the current UTC day.
 *   dedup_io_ok           false if the dedup log could not be opened, scanned,
 *                         or written. Fail-CLOSES (AC-43.3.4).
 *
 * Returns true iff a courtesy reply should be sent.
 */
bool hu_imessage_should_courtesy_reply(bool handle_in_allowlist, double hours_since_last_reply,
                                       int aggregate_today, bool dedup_io_ok);

/*
 * Strip handle-shaped patterns from a sender name. When the name looks like a
 * phone number, an email, or any handle-ish glob, returns the safe literal
 * "there" so the reply body never echoes the inbound identifier.
 *
 *   in        sender name as observed in the inbound row.
 *   out       buffer to receive the safe name; must be at least `out_cap` bytes.
 *   out_cap   capacity of `out` including the NUL terminator.
 *
 * Always NUL-terminates `out` (sets `out[0]='\0'` on empty/null input). Returns
 * the number of bytes written excluding the NUL.
 */
size_t hu_imessage_courtesy_sanitize_name(const char *in, char *out, size_t out_cap);

/*
 * Composes the courtesy reply body for a sanitized name. The body NEVER contains
 * raw handle-shaped tokens; callers must pass a name that has already passed
 * through `hu_imessage_courtesy_sanitize_name`.
 *
 *   sanitized_name   safe-name produced by the sanitizer above.
 *   out              buffer to receive the reply body.
 *   out_cap          capacity of `out` including the NUL terminator.
 *
 * Returns the number of bytes written excluding the NUL.
 */
size_t hu_imessage_courtesy_compose_reply(const char *sanitized_name, char *out, size_t out_cap);

/*
 * Dedup-log evaluation result. Populated from
 * `~/.human/imessage_courtesy.log` under flock(LOCK_EX). All fields are valid
 * iff `io_ok` is true; on false the caller MUST treat the predicate as failed.
 */
typedef struct hu_imessage_courtesy_state {
    bool io_ok;              /* false ⇒ fail-CLOSED at the predicate */
    double hours_since_last; /* large positive value if no prior record */
    int aggregate_today;     /* count of courtesy replies sent in the current UTC day */
} hu_imessage_courtesy_state_t;

/*
 * Open + flock(LOCK_EX) the dedup log, scan it, populate `state`, then OPTIONALLY
 * append a fresh `<handle>\t<now>\n` record and fsync — all under the same lock
 * to close the TOCTOU window between check and record (AC-43.3.2/.4).
 *
 *   handle        the non-allowlisted sender handle.
 *   now_epoch     epoch seconds at the call site (test seam — caller may inject).
 *   record_after  true: if the populated state would lead the predicate to fire,
 *                 append a record before releasing the lock.
 *   recorded_out  set to true iff a record was actually appended.
 *   state_out     populated read-side view of the log (under-lock snapshot).
 *
 * Returns `HU_OK` on success (including the "no record appended" case).
 * On any I/O / lock error, `state_out->io_ok` is false and the function returns
 * `HU_OK` so callers can route through the predicate fail-CLOSED path.
 *
 * Test seam: respects the `HU_IMESSAGE_COURTESY_LOG_PATH` env var when set.
 */
hu_error_t hu_imessage_courtesy_eval_and_record(const char *handle, int64_t now_epoch,
                                                bool record_after, bool *recorded_out,
                                                hu_imessage_courtesy_state_t *state_out);

/* Pending-courtesy ring API. The ring is owned by the iMessage channel context;
 * these helpers operate on a caller-provided opaque pointer that hands ownership
 * lifetime to the channel struct. */

typedef struct hu_imessage_courtesy_pending {
    char handle[HU_IMESSAGE_COURTESY_HANDLE_MAX];
    char body[HU_IMESSAGE_COURTESY_REPLY_MAX];
} hu_imessage_courtesy_pending_t;

typedef struct hu_imessage_courtesy_ring {
    hu_imessage_courtesy_pending_t slots[HU_IMESSAGE_COURTESY_RING_CAPACITY];
    size_t head; /* next slot to fill */
    size_t tail; /* next slot to drain */
    size_t count;
    uint64_t refused_enqueues; /* counter for observability when full */
} hu_imessage_courtesy_ring_t;

void hu_imessage_courtesy_ring_init(hu_imessage_courtesy_ring_t *ring);

/* Enqueue a pending courtesy reply. Returns true on success, false if the ring
 * is full (refused_enqueues is incremented in the false branch). */
bool hu_imessage_courtesy_ring_enqueue(hu_imessage_courtesy_ring_t *ring, const char *handle,
                                       const char *body);

/* Drain one entry. Returns false when the ring is empty. */
bool hu_imessage_courtesy_ring_drain_one(hu_imessage_courtesy_ring_t *ring,
                                         hu_imessage_courtesy_pending_t *out);

size_t hu_imessage_courtesy_ring_count(const hu_imessage_courtesy_ring_t *ring);
uint64_t hu_imessage_courtesy_ring_refused(const hu_imessage_courtesy_ring_t *ring);

#ifdef __cplusplus
}
#endif

#endif /* HU_CHANNELS_IMESSAGE_COURTESY_H */
