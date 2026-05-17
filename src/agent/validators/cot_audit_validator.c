#include "human/agent/output_validator.h"
#include "human/agent/validators/builtin.h"
#include "human/security/cot_audit.h"
#include <string.h>

static hu_error_t cot_audit_validate(void *ctx, hu_allocator_t *alloc,
                                     const hu_validator_context_t *vctx, const char *response,
                                     size_t response_len, hu_validator_result_t *out) {
    (void)ctx;
    (void)vctx;
    memset(out, 0, sizeof(*out));

    hu_cot_audit_result_t aud;
    memset(&aud, 0, sizeof(aud));

    hu_error_t err = hu_cot_audit(alloc, response, response_len, &aud);
    if (err != HU_OK)
        return err;

    if (aud.verdict == HU_COT_SAFE || aud.verdict == HU_COT_SUSPICIOUS) {
        hu_cot_audit_result_free(alloc, &aud);
        out->decision = HU_VALIDATOR_PASS;
        return HU_OK;
    }

    /* HU_COT_BLOCKED — transfer the reason to the result. */
    out->decision = HU_VALIDATOR_REJECT;
    out->reason = aud.reason;
    out->reason_len = aud.reason_len;
    out->reason_owned = true;
    aud.reason = NULL; /* prevent double-free in result_free */
    aud.reason_len = 0;
    hu_cot_audit_result_free(alloc, &aud);
    return HU_OK;
}

static const char *cot_audit_name(void *ctx) {
    (void)ctx;
    return "cot_audit";
}

static const hu_output_validator_vtable_t cot_audit_vtable = {
    .validate = cot_audit_validate,
    .name = cot_audit_name,
    .deinit = NULL,
};

hu_error_t hu_validator_cot_audit_create(hu_allocator_t *alloc, hu_output_validator_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    out->ctx = NULL;
    out->vtable = &cot_audit_vtable;
    return HU_OK;
}
