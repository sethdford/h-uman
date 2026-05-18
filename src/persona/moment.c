#include "human/moment.h"

#include <string.h>

#include "human/core/error.h"

/* Returns seconds elapsed since ts_s, clamped to 0 on clock skew.
   Returns -1 when ts_s is negative (timestamp not available). */
static int64_t compute_delta_s(int64_t ts_s, int64_t now_s) {
    if (ts_s < 0)
        return -1;
    int64_t d = now_s - ts_s;
    return d < 0 ? 0 : d;
}

hu_error_t hu_moment_compose_from_inputs(const struct hu_persona_t *persona,
                                         const struct hu_persona_overlay_t *overlay,
                                         const struct hu_conversation_history_t *history,
                                         int64_t last_their_ts_s, int64_t last_our_ts_s,
                                         const char *contact_tz, int64_t now_s, hu_moment_t *out) {
    (void)persona;
    (void)overlay;
    (void)history;
    (void)contact_tz;
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
    return HU_OK;
}
