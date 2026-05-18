#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "human/moment.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "human/core/error.h"

/* Returns seconds elapsed since ts_s, clamped to 0 on clock skew.
   Returns -1 when ts_s is negative (timestamp not available). */
static int64_t compute_delta_s(int64_t ts_s, int64_t now_s) {
    if (ts_s < 0)
        return -1;
    int64_t d = now_s - ts_s;
    return d < 0 ? 0 : d;
}

/* Maps (hour, minute) in local time to a hu_moment_phase_t.
   Boundaries match the enum comments in moment.h. */
static hu_moment_phase_t hour_to_phase(int hour, int min) {
    int t = hour * 60 + min;
    if (t < 5 * 60 + 30)
        return HU_MOMENT_PHASE_DEEP_NIGHT;
    if (t < 7 * 60 + 30)
        return HU_MOMENT_PHASE_EARLY_MORNING;
    if (t < 11 * 60)
        return HU_MOMENT_PHASE_MORNING;
    if (t < 14 * 60)
        return HU_MOMENT_PHASE_MIDDAY;
    if (t < 17 * 60 + 30)
        return HU_MOMENT_PHASE_AFTERNOON;
    if (t < 21 * 60 + 30)
        return HU_MOMENT_PHASE_EVENING;
    /* 21:30–24:00 */ return HU_MOMENT_PHASE_NIGHT;
}

/* Converts a Unix timestamp to a phase using the given IANA timezone name.
 * When tz is NULL, the process-local timezone is used.
 *
 * THREAD SAFETY: This function temporarily modifies the TZ environment
 * variable when tz != NULL. It restores it before returning, but the
 * swap is NOT atomic. Callers must ensure single-threaded access (the
 * agent-turn caller is single-threaded). Do not call concurrently.
 *
 * BUFFER SIZE: saved_tz uses 256 bytes, safely larger than any real IANA
 * timezone name (longest known: ~40 chars). */
static hu_moment_phase_t phase_for_tz(int64_t now_s, const char *tz) {
    time_t t = (time_t)now_s;
    struct tm tm_out;

    if (tz != NULL) {
        const char *saved = getenv("TZ");
        char saved_copy[256];
        saved_copy[0] = '\0';
        if (saved)
            snprintf(saved_copy, sizeof saved_copy, "%s", saved);

        setenv("TZ", tz, 1);
        tzset();
        localtime_r(&t, &tm_out);

        /* Restore: if TZ was set before, restore it; otherwise remove it. */
        if (saved)
            setenv("TZ", saved_copy, 1);
        else
            unsetenv("TZ");
        tzset();
    } else {
        localtime_r(&t, &tm_out);
    }

    return hour_to_phase(tm_out.tm_hour, tm_out.tm_min);
}

hu_error_t hu_moment_compose_from_inputs(const struct hu_persona_t *persona,
                                         const struct hu_persona_overlay_t *overlay,
                                         const struct hu_conversation_history_t *history,
                                         int64_t last_their_ts_s, int64_t last_our_ts_s,
                                         const char *contact_tz, int64_t now_s, hu_moment_t *out) {
    (void)persona;
    (void)overlay;
    (void)history;
    if (out == NULL)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    /* enum zero-values are correct defaults; only the time deltas need -1 overrides. */
    out->time_since_their_last_msg_s = compute_delta_s(last_their_ts_s, now_s);
    out->time_since_our_last_msg_s = compute_delta_s(last_our_ts_s, now_s);
    out->composed_at_s = now_s;
    if (last_their_ts_s >= 0)
        out->source_flags |= HU_MOMENT_SRC_LAST_THEIR_TS;
    if (last_our_ts_s >= 0)
        out->source_flags |= HU_MOMENT_SRC_LAST_OUR_TS;

    /* Phase fields: local and contact's timezone. */
    out->phase_local = phase_for_tz(now_s, NULL);
    if (contact_tz != NULL && contact_tz[0] != '\0') {
        out->phase_theirs = phase_for_tz(now_s, contact_tz);
        out->source_flags |= HU_MOMENT_SRC_CONTACT_TZ;
    } else {
        out->phase_theirs = out->phase_local;
    }
    out->it_is_unusual_hour_for_them = (out->phase_theirs == HU_MOMENT_PHASE_DEEP_NIGHT);

    return HU_OK;
}
