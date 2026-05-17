#include "human/agent/output_validator.h"
#include "human/core/allocator.h"
#include <stddef.h>

void hu_validator_result_free(hu_allocator_t *alloc, hu_validator_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->text && result->text_owned) {
        alloc->free(alloc->ctx, (void *)result->text, result->text_len + 1);
    }
    if (result->reason && result->reason_owned) {
        alloc->free(alloc->ctx, (void *)result->reason, result->reason_len + 1);
    }
    result->text = NULL;
    result->text_len = 0;
    result->text_owned = false;
    result->reason = NULL;
    result->reason_len = 0;
    result->reason_owned = false;
}

void hu_output_validator_deinit(hu_output_validator_t *v, hu_allocator_t *alloc) {
    if (!v || !v->vtable)
        return;
    if (v->vtable->deinit) {
        v->vtable->deinit(v->ctx, alloc);
    }
    v->ctx = NULL;
    v->vtable = NULL;
}
