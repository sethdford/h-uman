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

/* ──────────────────────────────────────────────────────────────────────────
 * Warmth + template + decide
 *
 * Composite layer on top of hu_followup_compute_send_time. Maps persona's
 * free-form warmth_level string ("close", "high", "friend", ...) to the
 * tier enum, selects a template text for the tier, and bundles the policy
 * decision (should_schedule + send_at_ms + template_text) into one struct.
 *
 * Default templates (2026-05-17) — short, low-stakes, always-appropriate:
 *   CLOSE  → "hey, just bumping this"
 *   FRIEND → "any thoughts on this?"
 *   NONE   → NULL (no follow-up)
 *
 * Templates are static strings; do NOT free. They flow through the daemon's
 * existing validator-chain + complexity-vary + casing post-processing in
 * the scheduled-message dispatch loop, so they don't feel robotic.
 * ────────────────────────────────────────────────────────────────────────── */

/* Map a persona's free-form warmth_level string ("close friend", "high",
 * "warm", "friend", ...) to a tier enum. Case-insensitive substring match.
 * NULL / empty / unknown strings map to HU_FOLLOWUP_WARMTH_NONE. */
hu_followup_warmth_t hu_followup_warmth_from_string(const char *warmth_level);

/* Static template text for a warmth tier. NULL for NONE (and any future
 * enum value not handled here). Pointer is stable; do NOT free. */
const char *hu_followup_template_for_warmth(hu_followup_warmth_t warmth);

typedef struct hu_followup_decision {
    bool should_schedule;      /* false when send_at_ms == 0 or template missing */
    uint64_t send_at_ms;       /* wall-clock ms; 0 when should_schedule is false */
    const char *template_text; /* static; do not free; valid iff should_schedule */
} hu_followup_decision_t;

/* Composite predicate: compute_send_time + template_for_warmth, bundled.
 * Returns {should_schedule=false} when either piece refuses. Pure. */
hu_followup_decision_t hu_followup_decide(const hu_followup_input_t *in);

/* ──────────────────────────────────────────────────────────────────────────
 * Idempotency: small ring of recently-scheduled message_ids
 *
 * The daemon's read-receipt watcher polls every loop iteration; without
 * dedup it would re-schedule a follow-up for the same outbound message
 * on every tick until either the contact replies or the schedule fires.
 *
 * This ring is intentionally small (32 entries) and in-memory only — a
 * daemon restart wipes it. The downside is one duplicate follow-up across
 * a restart in the worst case; acceptable given the watcher itself only
 * fires for messages already in the chat.db "read but unreplied" set.
 * Persistence can be added later if duplicates become observable.
 *
 * Pinned by tests/test_follow_up.c (dedup_*).
 * ────────────────────────────────────────────────────────────────────────── */

#define HU_FOLLOWUP_DEDUP_SIZE 32

typedef struct hu_followup_dedup {
    int64_t recent_msg_ids[HU_FOLLOWUP_DEDUP_SIZE];
    size_t next_slot;
} hu_followup_dedup_t;

/* Zero the ring. Safe to call on the same instance multiple times. */
void hu_followup_dedup_init(hu_followup_dedup_t *d);

/* True if msg_id is in the ring. Returns false for NULL d, msg_id <= 0,
 * or empty ring. Pure. */
bool hu_followup_dedup_seen(const hu_followup_dedup_t *d, int64_t msg_id);

/* Record msg_id in the ring, overwriting the oldest slot. No-op on
 * NULL d or msg_id <= 0. */
void hu_followup_dedup_record(hu_followup_dedup_t *d, int64_t msg_id);

/* ──────────────────────────────────────────────────────────────────────────
 * Per-contact follow-up cooldown ledger.
 *
 * The msg-id dedup ring above cannot bound follow-up FREQUENCY: each bump the
 * daemon sends becomes a new read-but-unreplied message in chat.db, which then
 * triggers the next bump — the bump bumps itself. On 2026-07-14 this sent the
 * identical "hey, just bumping this" 5x in one day to one contact.
 *
 * The ledger caps scheduling per CONTACT: at most one follow-up per contact
 * per HU_FOLLOWUP_PER_CONTACT_COOLDOWN_MS, regardless of msg-id churn.
 * In-memory like the dedup ring (worst case across a daemon restart: one
 * extra follow-up). Pinned by tests/test_follow_up.c (followup_ledger_*).
 * ────────────────────────────────────────────────────────────────────────── */

#define HU_FOLLOWUP_CONTACT_LEDGER_SIZE 16
#define HU_FOLLOWUP_CONTACT_ID_MAX      64
/* 48h: humans bump a silent thread after a day or two, never twice a day. */
#define HU_FOLLOWUP_PER_CONTACT_COOLDOWN_MS (48ULL * 3600ULL * 1000ULL)

typedef struct hu_followup_contact_ledger {
    char contact_ids[HU_FOLLOWUP_CONTACT_LEDGER_SIZE][HU_FOLLOWUP_CONTACT_ID_MAX];
    uint64_t scheduled_at_ms[HU_FOLLOWUP_CONTACT_LEDGER_SIZE];
    size_t next_slot;
} hu_followup_contact_ledger_t;

/* Zero the ledger. Safe to call repeatedly. */
void hu_followup_contact_ledger_init(hu_followup_contact_ledger_t *l);

/* True if a follow-up was recorded for contact_id within cooldown_ms of
 * now_ms — i.e. scheduling another one now would over-bump. Pure; NULL/empty
 * inputs return false (unknown contact is never "recent"). */
bool hu_followup_contact_recent(const hu_followup_contact_ledger_t *l, const char *contact_id,
                                uint64_t now_ms, uint64_t cooldown_ms);

/* Record that a follow-up was scheduled for contact_id at now_ms. Re-recording
 * an existing contact refreshes its slot; otherwise the oldest slot is
 * evicted. No-op on NULL/empty inputs. */
void hu_followup_contact_record(hu_followup_contact_ledger_t *l, const char *contact_id,
                                uint64_t now_ms);

/* ──────────────────────────────────────────────────────────────────────────
 * Send-now predicate — extracts the daemon's send decision into a pure
 * predicate per .claude/rules/security-predicate-extraction.md.
 *
 * Returns true if the follow-up should be delivered immediately (synchronously
 * or with minimal delay). Returns false if:
 *   - throttle has reached per_contact_daily_max for today
 *   - (future extension) rate limiter indicates backoff
 *
 * This is the gate the daemon's flush function checks BEFORE calling
 * autoresponder and iMessage send.
 *
 * Thread-safe on separate throttle instances; shares throttle state across
 * calls (tests must reset between cases).
 * ────────────────────────────────────────────────────────────────────────── */

struct hu_proactive_throttle; /* forward decl */

bool hu_follow_up_should_send_now(const char *contact_handle, uint64_t now_ms,
                                  struct hu_proactive_throttle *throttle);

#endif /* HU_FOLLOW_UP_H */
