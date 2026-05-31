#include "human/follow_up.h"
#include "human/core/string.h"

#include <ctype.h>
#include <string.h>
#include <time.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Follow-up delay computation + warmth-tier + template selection.
 * See include/human/follow_up.h for the full contract.
 *
 * Implementation notes:
 *   - hu_chronotype_is_active_hour() (from circadian.h) is the authority on
 *     "is this hour quiet for this chronotype?" — we don't re-implement it.
 *   - Snap loop is bounded at 36 hops (38 h) so it cannot run away under any
 *     pathological chronotype band; if we don't find an active hour in that
 *     window something's deeply wrong and we drop the follow-up.
 *   - All math is uint64_t to avoid 32-bit overflow on read_at_ms (which is
 *     epoch ms ≈ 1.7e12).
 *   - The warmth-string-to-enum and template-selector pieces are kept as
 *     separate small functions so each is independently testable; the
 *     composite hu_followup_decide() bundles them.
 * ────────────────────────────────────────────────────────────────────────── */

#define MS_PER_MIN   ((uint64_t)60ULL * 1000ULL)
#define MS_PER_HOUR  ((uint64_t)60ULL * MS_PER_MIN)
#define MAX_DELAY_MS (24ULL * MS_PER_HOUR)
#define SNAP_CAP     36 /* max hours we'll advance to find an active hour */

/* Convert a wall-clock ms timestamp + UTC offset to a local hour-of-day. */
static uint8_t local_hour_from_ms(uint64_t ms, int tz_offset_seconds) {
    int64_t adjusted = (int64_t)(ms / 1000ULL) + (int64_t)tz_offset_seconds;
    /* Wrap into [0, 86400) seconds-of-day */
    int64_t day_sec = adjusted % 86400;
    if (day_sec < 0)
        day_sec += 86400;
    return (uint8_t)(day_sec / 3600);
}

uint64_t hu_followup_compute_send_time(const hu_followup_input_t *in) {
    if (!in)
        return 0;
    if (in->warmth == HU_FOLLOWUP_WARMTH_NONE)
        return 0;
    if (in->read_at_ms == 0)
        return 0;

    /* Base delay by warmth tier (minutes). */
    uint32_t base_min;
    switch (in->warmth) {
    case HU_FOLLOWUP_WARMTH_CLOSE:
        base_min = 60;
        break;
    case HU_FOLLOWUP_WARMTH_FRIEND:
        base_min = 120;
        break;
    default:
        return 0; /* defensive — covered by NONE check above */
    }

    /* Seeded jitter: [70%, 130%] of base, so close-friend lands 42–78 min,
     * friend lands 84–156 min. Uniform over 61 integer percentage points. */
    uint32_t s = in->seed * 1103515245u + 12345u;
    uint32_t jitter_pct = 70u + ((s >> 16) % 61u);
    uint64_t delay_ms = (uint64_t)base_min * (uint64_t)jitter_pct * MS_PER_MIN / 100ULL;

    uint64_t candidate = in->read_at_ms + delay_ms;

    /* Snap forward to the contact's chronotype-aware active hours. The loop
     * advances hour-by-hour until we hit an active hour OR exhaust SNAP_CAP
     * iterations (safety against a misconfigured chronotype band with no
     * active hours). */
    uint8_t hour = local_hour_from_ms(candidate, in->local_tz_offset_seconds);
    int snapped = 0;
    while (snapped < SNAP_CAP && !hu_chronotype_is_active_hour(in->contact_chronotype, hour)) {
        candidate += MS_PER_HOUR;
        hour = (uint8_t)((hour + 1u) % 24u);
        snapped++;
    }
    if (snapped >= SNAP_CAP)
        return 0; /* couldn't find an active hour — bail */

    /* Drop the follow-up if snapping pushed us beyond the 24 h staleness
     * cutoff. A bumping message that lands more than a day after read is
     * less "checking in" and more "stalking". */
    if (candidate - in->read_at_ms > MAX_DELAY_MS)
        return 0;

    return candidate;
}

/* ── Warmth-string → tier enum ──────────────────────────────────────────── */

/* Uses hu_str_contains_word_ci (human/core/string.h) — word-boundary CI
 * matching that avoids the substring-overlap hazard of the warmth classifier:
 *   "lukewarm"       contains "warm"   — but means cool, not close
 *   "unfriendly"     contains "friend" — but means distant, not friendly
 *   "highly distant" contains "high"   — explicit disclaimer of closeness
 * Pinned by tests/test_follow_up.c (warmth-string ⊃ keyword cases).
 * See ~/.claude/rules/substring-classifier-pitfalls.md for the pattern. */
hu_followup_warmth_t hu_followup_warmth_from_string(const char *warmth_level) {
    if (!warmth_level || !warmth_level[0])
        return HU_FOLLOWUP_WARMTH_NONE;

    /* CLOSE keywords first — "close friend" must map to CLOSE, not FRIEND.
     * The persona conventions in src/context/conversation.c already use
     * "high" / "warm" for close relationships; we accept both that and
     * the more explicit "close". Word-boundary matching avoids the
     * "lukewarm" / "highly distant" / "unfriendly" mis-classifications. */
    if (hu_str_contains_word_ci(warmth_level, "close") ||
        hu_str_contains_word_ci(warmth_level, "high") ||
        hu_str_contains_word_ci(warmth_level, "warm"))
        return HU_FOLLOWUP_WARMTH_CLOSE;

    if (hu_str_contains_word_ci(warmth_level, "friend"))
        return HU_FOLLOWUP_WARMTH_FRIEND;

    /* Acquaintance / unknown / anything else: no follow-up. */
    return HU_FOLLOWUP_WARMTH_NONE;
}

/* ── Template selector ──────────────────────────────────────────────────── */

const char *hu_followup_template_for_warmth(hu_followup_warmth_t warmth) {
    switch (warmth) {
    case HU_FOLLOWUP_WARMTH_CLOSE:
        return "hey, just bumping this";
    case HU_FOLLOWUP_WARMTH_FRIEND:
        return "any thoughts on this?";
    case HU_FOLLOWUP_WARMTH_NONE:
    default:
        return NULL;
    }
}

/* ── Composite decision ─────────────────────────────────────────────────── */

hu_followup_decision_t hu_followup_decide(const hu_followup_input_t *in) {
    hu_followup_decision_t out = {0};
    if (!in)
        return out;

    uint64_t send_at = hu_followup_compute_send_time(in);
    if (send_at == 0)
        return out;

    const char *tmpl = hu_followup_template_for_warmth(in->warmth);
    if (!tmpl)
        return out;

    out.should_schedule = true;
    out.send_at_ms = send_at;
    out.template_text = tmpl;
    return out;
}

/* ── Idempotency ring ───────────────────────────────────────────────────── */

void hu_followup_dedup_init(hu_followup_dedup_t *d) {
    if (!d)
        return;
    for (size_t i = 0; i < HU_FOLLOWUP_DEDUP_SIZE; i++)
        d->recent_msg_ids[i] = 0;
    d->next_slot = 0;
}

bool hu_followup_dedup_seen(const hu_followup_dedup_t *d, int64_t msg_id) {
    if (!d || msg_id <= 0)
        return false;
    for (size_t i = 0; i < HU_FOLLOWUP_DEDUP_SIZE; i++) {
        if (d->recent_msg_ids[i] == msg_id)
            return true;
    }
    return false;
}

void hu_followup_dedup_record(hu_followup_dedup_t *d, int64_t msg_id) {
    if (!d || msg_id <= 0)
        return;
    d->recent_msg_ids[d->next_slot] = msg_id;
    d->next_slot = (d->next_slot + 1) % HU_FOLLOWUP_DEDUP_SIZE;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Send-now predicate — daemon gate for follow-up throttle
 * ────────────────────────────────────────────────────────────────────────── */

#include "human/agent/proactive_throttle.h"

bool hu_follow_up_should_send_now(const char *contact_handle, uint64_t now_ms,
                                  struct hu_proactive_throttle *throttle) {
    if (!contact_handle || !contact_handle[0] || !throttle)
        return false;

    /* Check per-contact daily cap via throttle subsystem. The throttle module
     * handles YMD computation and per-contact daily max enforcement. */
    if (!hu_proactive_throttle_record_send(throttle, contact_handle, "follow_up", now_ms))
        return false; /* throttle rejected the send */

    return true;
}
