#ifndef HU_DAEMON_DATED_FOLLOWUP_H
#define HU_DAEMON_DATED_FOLLOWUP_H

/*
 * Dated-moment check-ins: when a contact mentions something with a date
 * ("interview Thursday", "mom's surgery on the 3rd"), queue ONE check-in for
 * after the event, carrying what the event was, so the proactive proposer
 * asks about the thing instead of sending a generic "hey".
 *
 * Why (2026-09-25 audit): hu_contextual_proactive_decide already parses
 * weekdays/dates and picks a send time (7pm after the event), but its caller
 * only logged "nothing enqueued". The older temporal_events trigger fired
 * BEFORE the event and consumed it without ever putting it in the prompt.
 *
 * The queue is the existing delayed_followups table, which the proactive
 * proposer already lists as due_followups context and marks sent only on a
 * confirmed delivery. Gated by HU_PROACTIVE_CONTEXTUAL (off|shadow|on,
 * default off) per .claude/rules/feature-gate-requires-measurement.md.
 */

#include "human/agent/contextual_proactive.h"
#include "human/core/allocator.h"
#include <stddef.h>
#include <stdint.h>

typedef enum hu_dated_followup_outcome {
    HU_DATED_FOLLOWUP_NONE = 0,        /* OFF, bad input, or the store is unavailable */
    HU_DATED_FOLLOWUP_WOULD_SCHEDULE,  /* SHADOW: logged, nothing queued */
    HU_DATED_FOLLOWUP_SCHEDULED,       /* ON: queued for send_at */
    HU_DATED_FOLLOWUP_ALREADY_PENDING, /* ON: the same event is already queued */
} hu_dated_followup_outcome_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Apply the gate to one detected situation. `memory` is the agent's
 * hu_memory_t (the delayed_followups store), `frame` the situation text from
 * hu_contextual_proactive_situation_frame, `send_at_s` its send time (epoch
 * seconds), `now_s` the current time for logging. Logs lengths and delays,
 * never the frame text or the contact. */
hu_dated_followup_outcome_t hu_daemon_dated_followup_apply(void *memory, hu_allocator_t *alloc,
                                                           hu_contextual_proactive_mode_t mode,
                                                           const char *contact, size_t contact_len,
                                                           const char *frame, size_t frame_len,
                                                           int64_t send_at_s, int64_t now_s);

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_DATED_FOLLOWUP_H */
