#include "human/moment.h"

#include <string.h>

#include "human/core/error.h"

hu_error_t hu_moment_compose_from_inputs(const struct hu_persona_t *persona,
                                         const struct hu_persona_overlay_t *overlay,
                                         const struct hu_conversation_history_t *history,
                                         int64_t last_their_ts_s, int64_t last_our_ts_s,
                                         const char *contact_tz, int64_t now_s, hu_moment_t *out) {
    (void)persona;
    (void)overlay;
    (void)history;
    (void)contact_tz;
    (void)last_their_ts_s;
    (void)last_our_ts_s;
    if (out == NULL)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    out->time_since_their_last_msg_s = -1;
    out->time_since_our_last_msg_s = -1;
    out->suggested_open = HU_MOMENT_OPEN_NONE;
    out->suggested_brevity = HU_MOMENT_BREVITY_MIRROR;
    out->composed_at_s = now_s;
    out->source_flags = 0u;
    return HU_OK;
}
