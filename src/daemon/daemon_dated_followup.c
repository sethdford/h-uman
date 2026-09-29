/* Dated-moment check-ins. Contract: include/human/daemon/dated_followup.h. */
#include "human/core/log.h"
#include "human/daemon/dated_followup.h"
#include "human/memory/superhuman.h"
#include <stdbool.h>

hu_dated_followup_outcome_t hu_daemon_dated_followup_apply(void *memory, hu_allocator_t *alloc,
                                                           hu_contextual_proactive_mode_t mode,
                                                           const char *contact, size_t contact_len,
                                                           const char *frame, size_t frame_len,
                                                           int64_t send_at_s, int64_t now_s) {
    if (mode == HU_CONTEXTUAL_PROACTIVE_OFF || !contact || contact_len == 0 || !frame ||
        frame_len == 0 || send_at_s <= 0)
        return HU_DATED_FOLLOWUP_NONE;
    long long in_min = (long long)((send_at_s - now_s) / 60);
    if (mode == HU_CONTEXTUAL_PROACTIVE_SHADOW) {
        /* Length and delay only: shadow telemetry never carries text or who. */
        hu_log_info("dated_followup", NULL,
                    "shadow: would schedule a check-in in %lld min (situation %zu B)", in_min,
                    frame_len);
        return HU_DATED_FOLLOWUP_WOULD_SCHEDULE;
    }
    if (!memory)
        return HU_DATED_FOLLOWUP_NONE;
    bool pending = false;
    if (hu_superhuman_delayed_followup_pending_exists(memory, contact, contact_len, frame,
                                                      frame_len, &pending) != HU_OK)
        return HU_DATED_FOLLOWUP_NONE;
    if (pending)
        return HU_DATED_FOLLOWUP_ALREADY_PENDING;
    if (hu_superhuman_delayed_followup_schedule(memory, alloc, contact, contact_len, frame,
                                                frame_len, send_at_s) != HU_OK) {
        hu_log_warn("dated_followup", NULL,
                    "could not queue a check-in (situation %zu B); it will not be sent", frame_len);
        return HU_DATED_FOLLOWUP_NONE;
    }
    hu_log_info("dated_followup", NULL, "scheduled a check-in in %lld min (situation %zu B)",
                in_min, frame_len);
    return HU_DATED_FOLLOWUP_SCHEDULED;
}
