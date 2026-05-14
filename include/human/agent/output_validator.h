/* hu_output_validator — composable post-generation output check.
 *
 * Each validator inspects a model response (or the partial state from
 * a previous validator's rewrite) and returns one of:
 *   PASS     — output is acceptable; chain continues with same text
 *   REWRITE  — output was modified; chain continues with new text
 *   REJECT   — output is unsendable; chain short-circuits, caller
 *              must either retry (via response_guard_retry_slim) or
 *              suppress the message
 *
 * Validators are stateless per-call. They receive an allocator for any
 * rewrite buffer they need and are responsible for diagnostic strings.
 * The chain (output_validator_chain.h) owns lifecycle; this header
 * defines the single-validator contract. */
#ifndef HU_AGENT_OUTPUT_VALIDATOR_H
#define HU_AGENT_OUTPUT_VALIDATOR_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum hu_validator_decision {
    HU_VALIDATOR_PASS = 0,
    HU_VALIDATOR_REWRITE = 1,
    HU_VALIDATOR_REJECT = 2,
} hu_validator_decision_t;

/* Per-call result. If decision == REWRITE, `text` is the new output and
 * `text_owned` indicates whether the caller must free it via `alloc`. If
 * decision == REJECT, `text` is NULL and `reason` explains why. */
typedef struct hu_validator_result {
    hu_validator_decision_t decision;
    const char *text;
    size_t text_len;
    bool text_owned;    /* if true, caller frees via alloc->free(.., text, text_len + 1) */
    const char *reason; /* allocator-owned; may be NULL on PASS/REWRITE */
    size_t reason_len;
} hu_validator_result_t;

void hu_validator_result_free(hu_allocator_t *alloc, hu_validator_result_t *result);

/* Per-call context. The chain passes this to each validator so it can
 * make decisions based on which channel/persona/provider produced the
 * response. NULL fields are permitted (the validator must tolerate them). */
typedef struct hu_validator_context {
    const char *channel_id; /* "imessage", "slack", ...; NULL = unknown */
    size_t channel_id_len;
    const char *persona_name; /* active persona display name; NULL = none */
    size_t persona_name_len;
    const char *provider_name; /* "anthropic", "gemini", ...; NULL = unknown */
    size_t provider_name_len;
} hu_validator_context_t;

typedef struct hu_output_validator_vtable {
    /* Run the validator. MUST populate *out with a valid result. */
    hu_error_t (*validate)(void *ctx, hu_allocator_t *alloc, const hu_validator_context_t *vctx,
                           const char *response, size_t response_len, hu_validator_result_t *out);
    /* Stable identifier for logs + telemetry. Must not return NULL. */
    const char *(*name)(void *ctx);
    /* Optional: free implementation-owned state. May be NULL. */
    void (*deinit)(void *ctx, hu_allocator_t *alloc);
} hu_output_validator_vtable_t;

typedef struct hu_output_validator {
    void *ctx;
    const hu_output_validator_vtable_t *vtable;
} hu_output_validator_t;

/* Free a validator's owned state. Safe on zero-initialized structs. */
void hu_output_validator_deinit(hu_output_validator_t *v, hu_allocator_t *alloc);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_OUTPUT_VALIDATOR_H */
