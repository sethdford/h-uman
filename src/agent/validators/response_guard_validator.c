#include "human/agent/output_validator.h"
#include "human/agent/response_guard.h"
#include "human/agent/validators/builtin.h"
#include <string.h>

static hu_error_t response_guard_validate(void *ctx, hu_allocator_t *alloc,
                                          const hu_validator_context_t *vctx, const char *response,
                                          size_t response_len, hu_validator_result_t *out) {
    (void)ctx;
    (void)vctx;
    memset(out, 0, sizeof(*out));

    char *new_text = NULL;
    size_t new_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));

    hu_error_t err = hu_response_guard_check(alloc, response, response_len, &new_text, &new_len,
                                             &outcome, &report);
    if (err != HU_OK)
        return err;

    switch (outcome) {
    case HU_GUARD_OK:
        out->decision = HU_VALIDATOR_PASS;
        return HU_OK;
    case HU_GUARD_REWROTE:
        out->decision = HU_VALIDATOR_REWRITE;
        out->text = new_text;
        out->text_len = new_len;
        out->text_owned = true;
        return HU_OK;
    case HU_GUARD_REJECT: {
        if (new_text)
            alloc->free(alloc->ctx, new_text, new_len + 1);
        const char *msg = "response_guard rejected response (harmony/thinking/degen/bullet leak)";
        size_t mlen = strlen(msg);
        char *reason = (char *)alloc->alloc(alloc->ctx, mlen + 1);
        if (!reason)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(reason, msg, mlen + 1);
        out->decision = HU_VALIDATOR_REJECT;
        out->reason = reason;
        out->reason_len = mlen;
        out->reason_owned = true;
        return HU_OK;
    }
    }
    return HU_OK;
}

static const char *response_guard_name(void *ctx) {
    (void)ctx;
    return "response_guard";
}

static const hu_output_validator_vtable_t response_guard_vtable = {
    .validate = response_guard_validate,
    .name = response_guard_name,
    .deinit = NULL,
};

hu_error_t hu_validator_response_guard_create(hu_allocator_t *alloc, hu_output_validator_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    out->ctx = NULL;
    out->vtable = &response_guard_vtable;
    return HU_OK;
}
