/* validator_telemetry.h — thin emit helper for validator decision events.
 *
 * Call hu_observer_emit_validator_decision() immediately after every
 * hu_output_validator_chain_execute() at call sites that hold an observer.
 * PASS outcomes are suppressed (PO ruling: avoid per-token noise).
 * The helper is a no-op when obs has a NULL vtable (hu_observer_noop()).
 *
 * String lifetime guarantee: all const char * fields in hu_chain_result_t
 * and hu_validator_context_t are caller-owned and must remain valid through
 * this call — the event is dispatched synchronously inside record_event.
 */
#ifndef HU_OBSERVABILITY_VALIDATOR_TELEMETRY_H
#define HU_OBSERVABILITY_VALIDATOR_TELEMETRY_H

#include "human/agent/output_validator.h"
#include "human/agent/output_validator_chain.h"
#include "human/observer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Emit a HU_OBSERVER_EVENT_VALIDATOR_DECISION event for REJECT or REWRITE
 * outcomes. Does nothing for PASS.
 *
 * Matches the hu_log_error/hu_log_warn convention: obs is a nullable pointer.
 * Safe to call with obs == NULL (no-op) or obs->vtable == NULL (noop observer).
 *
 * @param obs          Nullable pointer to the observer (e.g. agent->observer).
 * @param cr           Chain result produced by hu_output_validator_chain_execute().
 * @param vctx         Validator context that was passed to the chain (may be NULL).
 * @param input_len    Length of the original input string (before any rewrite).
 */
void hu_observer_emit_validator_decision(hu_observer_t *obs, const hu_chain_result_t *cr,
                                         const hu_validator_context_t *vctx, size_t input_len);

#ifdef __cplusplus
}
#endif

#endif /* HU_OBSERVABILITY_VALIDATOR_TELEMETRY_H */
