#include "human/follow_up.h"

/* ──────────────────────────────────────────────────────────────────────────
 * Follow-up delay computation. See include/human/follow_up.h for contract.
 *
 * Implementation notes:
 *   - hu_chronotype_is_active_hour() (from circadian.h) is the authority on
 *     "is this hour quiet for this chronotype?" — we don't re-implement it.
 *   - Snap loop is bounded at 36 hops (38 h) so it cannot run away under any
 *     pathological chronotype band; if we don't find an active hour in that
 *     window something's deeply wrong and we drop the follow-up.
 *   - All math is uint64_t to avoid 32-bit overflow on read_at_ms (which is
 *     epoch ms ≈ 1.7e12).
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
