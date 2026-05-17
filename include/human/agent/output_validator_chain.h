/* hu_output_validator_chain — composable pipeline of output validators.
 *
 * Conceptually identical to hu_hook_pipeline (include/human/hook_pipeline.h):
 *   - A chain is an ordered list of validators.
 *   - Execution walks the list in registration order.
 *   - PASS  -> continue with same text.
 *   - REWRITE -> continue with new text (current_text is replaced).
 *   - REJECT -> short-circuit; chain reports the failing validator's
 *               name + reason and returns.
 *
 * Memory:
 *   - The chain owns its validators (validators registered via _add take
 *     ownership semantics; the chain calls hu_output_validator_deinit on
 *     each at chain destruction).
 *   - Intermediate rewrite buffers are tracked and freed before chain
 *     execution returns, except for the FINAL output buffer (when the
 *     chain ends with PASS/REWRITE), which is transferred to the caller
 *     via the result struct.
 */
#ifndef HU_AGENT_OUTPUT_VALIDATOR_CHAIN_H
#define HU_AGENT_OUTPUT_VALIDATOR_CHAIN_H

#include "human/agent/output_validator.h"
#include "human/core/allocator.h"
#include "human/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_output_validator_chain hu_output_validator_chain_t;

hu_error_t hu_output_validator_chain_create(hu_allocator_t *alloc,
                                            hu_output_validator_chain_t **out);
void hu_output_validator_chain_destroy(hu_output_validator_chain_t *chain);

/* Append a validator. The chain takes ownership of `v` (its deinit will
 * be called at chain destruction). On error the validator is NOT owned. */
hu_error_t hu_output_validator_chain_add(hu_output_validator_chain_t *chain,
                                         hu_output_validator_t v);

size_t hu_output_validator_chain_len(const hu_output_validator_chain_t *chain);

/* Per-execution result. Holds the final text + which validator (if any)
 * rejected, and a list of which validators rewrote or rejected for logs. */
typedef struct hu_chain_result {
    hu_validator_decision_t final_decision; /* PASS / REWRITE / REJECT */
    const char *final_text;                 /* NULL on REJECT */
    size_t final_text_len;
    bool final_text_owned; /* free with alloc if true */
    /* Index of the validator that produced final_decision; SIZE_MAX if
     * empty chain or no decision changed the text. */
    size_t deciding_validator;
    /* Name of deciding validator (borrowed pointer, valid for chain lifetime). */
    const char *deciding_validator_name;
    /* On REJECT, the reason returned by that validator (allocator-owned). */
    const char *reject_reason;
    size_t reject_reason_len;
    bool reject_reason_owned;
    /* Counts for telemetry. */
    size_t rewrite_count;
    size_t reject_count; /* always 0 or 1 (chain short-circuits) */
} hu_chain_result_t;

void hu_chain_result_free(hu_allocator_t *alloc, hu_chain_result_t *result);

hu_error_t hu_output_validator_chain_execute(const hu_output_validator_chain_t *chain,
                                             hu_allocator_t *alloc,
                                             const hu_validator_context_t *vctx,
                                             const char *response, size_t response_len,
                                             hu_chain_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_OUTPUT_VALIDATOR_CHAIN_H */
