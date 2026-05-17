#ifndef HU_FOLLOW_UP_H
#define HU_FOLLOW_UP_H

#include "human/persona/circadian.h" /* hu_chronotype_t + hu_chronotype_is_active_hour */

#include <stdbool.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Follow-up scheduler — circadian-aware delay computation
 *
 * Computes when (in wall-clock ms) to send a follow-up to a contact who
 * read our message but hasn't replied. The daemon's read-receipt watcher
 * calls this predicate after detecting a read-but-unreplied outbound, and
 * schedules the returned timestamp via hu_conversation_schedule_message_on.
 *
 * This is a PURE predicate — no I/O, no allocations, no globals — so the
 * policy decision is independently testable. The wiring (read-receipt
 * detection, follow-up text generation, idempotency tracking) lives at the
 * daemon layer in a follow-up commit.
 *
 * Policy (per user direction 2026-05-17):
 *   1. Base delay by relationship warmth tier:
 *        CLOSE  →  60 min (you check in fast with close friends)
 *        FRIEND → 120 min (medium delay for regular friends)
 *        NONE   → no follow-up at all (acquaintance / unknown)
 *   2. Apply ±30% seeded jitter so deliveries don't bunch at exact intervals
 *   3. Snap into the contact's chronotype-aware active hours via
 *      hu_chronotype_is_active_hour. A 9:30 pm read for an OWL contact can
 *      still follow up at ~11 pm; the same read for a LARK contact snaps
 *      forward to the next morning's active band (~6 am).
 *   4. Drop the follow-up (return 0) if the snapped send time would land
 *      more than 24 h after the read — too stale to be useful.
 *
 * Returns 0 when no follow-up should fire (warmth NONE, stale-after-snap,
 * invalid input). Otherwise returns the wall-clock ms at which the
 * follow-up should be delivered.
 *
 * Pinned by tests/test_follow_up.c.
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_followup_warmth {
    HU_FOLLOWUP_WARMTH_NONE = 0, /* no follow-up — default for acquaintance / unknown */
    HU_FOLLOWUP_WARMTH_FRIEND,   /* medium delay, ~120 min base */
    HU_FOLLOWUP_WARMTH_CLOSE,    /* short delay, ~60 min base */
} hu_followup_warmth_t;

typedef struct hu_followup_input {
    uint64_t read_at_ms;                /* when contact read our message (wall clock ms) */
    hu_followup_warmth_t warmth;        /* relationship tier — drives base delay */
    hu_chronotype_t contact_chronotype; /* contact's chronotype; UNKNOWN → INTERMEDIATE band */
    int local_tz_offset_seconds;        /* seconds offset from UTC; e.g. PST = -28800 */
    uint32_t seed;                      /* deterministic jitter seed */
} hu_followup_input_t;

/* Returns 0 if no follow-up should be scheduled, else wall-clock ms of send. */
uint64_t hu_followup_compute_send_time(const hu_followup_input_t *in);

#endif /* HU_FOLLOW_UP_H */
