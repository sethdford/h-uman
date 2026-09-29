#ifndef HU_DAEMON_CONSECUTIVE_LIMITER_H
#define HU_DAEMON_CONSECUTIVE_LIMITER_H
/* Per-contact consecutive-reply limiter (rewritten 2026-09-04).
 *
 * Counts replies the daemon has sent to one contact since the real user last
 * stepped in, and tells the reactive path to stay silent once that count
 * reaches behavior.max_consecutive_replies (0 = no cap; the hourly reply
 * budget is the runaway brake). A gap longer than
 * behavior.consecutive_reset_minutes since our last reply starts a NEW burst:
 * a contact coming back after lunch does not inherit the morning's silence.
 *
 * Why the rewrite: the previous limiter was four static arrays inside
 * hu_service_run with a hard-coded cap of 3 that only Seth typing could reset.
 * On 2026-09-04 a rental agent's four messages, two of them direct questions,
 * went unanswered because three earlier outputs (an empty reply, a tapback and
 * a song link) had spent the cap. The silenced question was logged at INFO.
 *
 * The state is a fixed table (HU_CONSEC_MAX_CONTACTS slots); a 33rd contact is
 * never limited, exactly as before. Pure with respect to time: the caller
 * passes `now_unix`, so every branch is testable without a clock. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_CONSEC_MAX_CONTACTS 32
#define HU_CONSEC_KEY_MAX      64

typedef struct hu_consec_slot {
    char key[HU_CONSEC_KEY_MAX];
    size_t key_len;
    uint32_t count;
    int64_t last_reply_unix;
} hu_consec_slot_t;

typedef struct hu_consec_limiter {
    hu_consec_slot_t slots[HU_CONSEC_MAX_CONTACTS];
    size_t used;
} hu_consec_limiter_t;

/* A zeroed struct is a valid, empty limiter; init is for stack instances. */
void hu_consec_limiter_init(hu_consec_limiter_t *l);

/* A reply went out to `key` at `now_unix`: count it and stamp the time.
 * Registers the contact on first use; silently ignored when the table is
 * full or the key does not fit. */
void hu_consec_limiter_note_reply(hu_consec_limiter_t *l, const char *key, size_t key_len,
                                  int64_t now_unix);

/* The real user stepped in for `key`: the count restarts at zero. */
void hu_consec_limiter_reset(hu_consec_limiter_t *l, const char *key, size_t key_len);

/* Should the daemon stay silent to `key` right now? Expires a stale burst
 * first (count → 0 when now − last_reply > reset_secs, reset_secs > 0), then
 * applies the cap (cap == 0 → never). `out_count` (optional) receives the
 * count after any expiry, for the log line. Unknown contacts are never silenced. */
bool hu_consec_limiter_should_silence(hu_consec_limiter_t *l, const char *key, size_t key_len,
                                      uint32_t cap, uint32_t reset_secs, int64_t now_unix,
                                      uint32_t *out_count);

/* Current count for `key` (0 when unknown). Read-only; no expiry applied. */
uint32_t hu_consec_limiter_count(const hu_consec_limiter_t *l, const char *key, size_t key_len);

#endif /* HU_DAEMON_CONSECUTIVE_LIMITER_H */
