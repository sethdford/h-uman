/* persona_fidelity_validator — stub for the M3 LoRA workstream.
 *
 * Always returns PASS. When the M3 persona LoRA ships, this validator
 * will host the on-device fidelity classifier: score each response
 * against the active persona's example bank and REJECT below threshold.
 *
 * Wired here as a no-op so the chain factory can include it now,
 * without changing the chain composition when M3 lands. */
#include "human/agent/output_validator.h"
#include "human/agent/validators/builtin.h"
#include <string.h>

static hu_error_t fidelity_validate(void *ctx, hu_allocator_t *alloc,
                                    const hu_validator_context_t *vctx, const char *response,
                                    size_t response_len, hu_validator_result_t *out) {
    (void)ctx;
    (void)alloc;
    (void)vctx;
    (void)response;
    (void)response_len;
    memset(out, 0, sizeof(*out));
    out->decision = HU_VALIDATOR_PASS;
    return HU_OK;
}

static const char *fidelity_name(void *ctx) {
    (void)ctx;
    return "persona_fidelity";
}

static const hu_output_validator_vtable_t fidelity_vtable = {
    .validate = fidelity_validate,
    .name = fidelity_name,
    .deinit = NULL,
};

hu_error_t hu_validator_persona_fidelity_create(hu_allocator_t *alloc, hu_output_validator_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    out->ctx = NULL;
    out->vtable = &fidelity_vtable;
    return HU_OK;
}
