/* validator_telemetry.c — emit helper for validator decision observer events.
 *
 * Design: chain stays pure (no observer); telemetry lives at call sites.
 * This file centralises the event construction so each call site is a
 * single-line call rather than 20 lines of event wiring.
 *
 * String ownership: all const char * pointers are non-owning. Callers must
 * guarantee the strings remain valid for the duration of this call.
 * hu_observer_record_event is synchronous, so no async lifetime hazard.
 */

#include "human/observability/validator_telemetry.h"

#include <stddef.h>
#include <string.h>

void hu_observer_emit_validator_decision(hu_observer_t *obs, const hu_chain_result_t *cr,
                                         const hu_validator_context_t *vctx, size_t input_len) {
    /* Fast-path: nothing to emit. */
    if (!cr)
        return;
    if (cr->final_decision == HU_VALIDATOR_PASS)
        return;
    /* No-op when observer pointer is NULL or has no vtable. Matches hu_log_impl_ convention. */
    if (!obs || !obs->vtable || !obs->vtable->record_event)
        return;

    /* Decide string labels. */
    const char *decision_str = (cr->final_decision == HU_VALIDATOR_REJECT) ? "reject" : "rewrite";

    /* Use deciding_validator_name from the chain result; fall back to
     * "<chain>" when unavailable (empty chain / internal error path). */
    const char *validator_name =
        (cr->deciding_validator_name && cr->deciding_validator_name[0] != '\0')
            ? cr->deciding_validator_name
            : "<chain>";

    /* Context strings — vctx may be NULL (daemon stream path passes NULL). */
    const char *channel_id = (vctx && vctx->channel_id) ? vctx->channel_id : NULL;
    const char *persona_name = (vctx && vctx->persona_name) ? vctx->persona_name : NULL;

    /* bytes_stripped: difference between input and final text for REWRITE;
     * 0 for REJECT (nothing produced). */
    size_t bytes_stripped = 0;
    if (cr->final_decision == HU_VALIDATOR_REWRITE && cr->final_text_len < input_len)
        bytes_stripped = input_len - cr->final_text_len;

    hu_observer_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.tag = HU_OBSERVER_EVENT_VALIDATOR_DECISION;
    ev.trace_id = NULL; /* call sites may set this if available */
    ev.data.validator_decision.decision = decision_str;
    ev.data.validator_decision.validator_name = validator_name;
    ev.data.validator_decision.channel_id = channel_id;
    ev.data.validator_decision.persona_name = persona_name;
    ev.data.validator_decision.response_len = input_len;
    ev.data.validator_decision.bytes_stripped = bytes_stripped;

    hu_observer_record_event(*obs, &ev);
}
