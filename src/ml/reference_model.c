/* src/ml/reference_model.c
 *
 * Phase 2 Task 3: implementation of hu_reference_model_create_from (see
 * include/human/ml/reference_model.h). Verbatim from the canonical plan
 * (lines 829-870) with the include-path correction documented in the
 * header (none needed in this .c file — it only includes the local
 * header, which transitively pulls in human/core/allocator.h etc.).
 */
#include "human/ml/reference_model.h"
#include <string.h>

hu_error_t hu_reference_model_create_from(hu_allocator_t *alloc,
                                           hu_model_t *base,
                                           const hu_gpt_config_t *config,
                                           hu_model_t *out) {
    if (!alloc || !base || !base->vtable || !base->vtable->get_params
        || !config || !out) return HU_ERR_INVALID_ARGUMENT;
    hu_error_t err = hu_gpt_create(alloc, config, out);
    if (err != HU_OK) return err;

    /* get_params returns a pointer-to-array owned by the model
     * (per include/human/ml/model.h:32, see src/ml/checkpoint.c:27 usage). */
    hu_ml_tensor_t *base_params = NULL;
    hu_ml_tensor_t *ref_params = NULL;
    size_t n_base = 0, n_ref = 0;
    err = base->vtable->get_params(base->ctx, &base_params, &n_base);
    if (err != HU_OK) {
        out->vtable->deinit(out->ctx, alloc);
        return err;
    }
    err = out->vtable->get_params(out->ctx, &ref_params, &n_ref);
    if (err != HU_OK) {
        out->vtable->deinit(out->ctx, alloc);
        return err;
    }
    if (n_base != n_ref) {
        out->vtable->deinit(out->ctx, alloc);
        return HU_ERR_PROVIDER_RESPONSE;  /* shape mismatch */
    }
    for (size_t i = 0; i < n_base; i++) {
        if (base_params[i].size_bytes != ref_params[i].size_bytes
            || base_params[i].dtype != ref_params[i].dtype) {
            out->vtable->deinit(out->ctx, alloc);
            return HU_ERR_PROVIDER_RESPONSE;
        }
        memcpy(ref_params[i].data, base_params[i].data, base_params[i].size_bytes);
    }
    return HU_OK;
}
