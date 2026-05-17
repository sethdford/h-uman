/* src/ml/policy_logprobs.c
 *
 * Phase 2 Task 2: implementation of hu_policy_logprobs (see
 * include/human/ml/policy_logprobs.h). Verbatim from the canonical plan
 * (lines 629-690) with the include-path correction documented in the
 * header.
 */
#include "human/ml/policy_logprobs.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

hu_error_t hu_policy_logprobs(hu_allocator_t *alloc, hu_model_t *model,
                               const int32_t *prompt, size_t prompt_len,
                               const int32_t *response, size_t response_len,
                               double *out_logprob) {
    if (!alloc || !model || !model->vtable || !model->vtable->forward
        || !prompt || !response || !out_logprob || prompt_len == 0
        || response_len == 0) return HU_ERR_INVALID_ARGUMENT;

    size_t total = prompt_len + response_len;
    int32_t *ids = (int32_t *)alloc->alloc(alloc->ctx, total * sizeof(int32_t));
    if (!ids) return HU_ERR_OUT_OF_MEMORY;
    memcpy(ids, prompt, prompt_len * sizeof(int32_t));
    memcpy(ids + prompt_len, response, response_len * sizeof(int32_t));

    /* hu_ml_tensor_t per include/human/ml/model.h:13-19 — shape[4] (size_t),
     * size_bytes (NOT .n). Forward output ownership: caller frees via project
     * allocator (matches src/ml/train.c:171 pattern). */
    hu_ml_tensor_t input = {
        .data = ids,
        .shape = {1, total, 0, 0},
        .ndim = 2,
        .dtype = HU_ML_DTYPE_I32,
        .size_bytes = total * sizeof(int32_t),
    };
    hu_ml_tensor_t output = {0};
    hu_error_t err = model->vtable->forward(model->ctx, &input, &output);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, ids, total * sizeof(int32_t));
        return err;
    }
    /* output.data: float logits, shape [1, total, V]. We need positions
     * (prompt_len - 1) .. (total - 2) predicting response tokens at
     * indices [prompt_len, total - 1]. */
    float *logits = (float *)output.data;
    size_t V = output.shape[2];  /* size_t, no cast */
    double sum = 0.0;
    for (size_t i = 0; i < response_len; i++) {
        size_t pred_pos = prompt_len - 1 + i;  /* position predicting response[i] */
        const float *li = logits + pred_pos * V;
        /* log-softmax */
        float mx = li[0];
        for (size_t j = 1; j < V; j++) if (li[j] > mx) mx = li[j];
        double s = 0.0;
        for (size_t j = 0; j < V; j++) s += exp((double)(li[j] - mx));
        double log_z = (double)mx + log(s);
        sum += (double)li[response[i]] - log_z;
    }
    *out_logprob = sum;

    /* Forward output owned by caller; free via project allocator
     * (matches src/ml/train.c:171). output.size_bytes is set by forward. */
    alloc->free(alloc->ctx, output.data, output.size_bytes);
    alloc->free(alloc->ctx, ids, total * sizeof(int32_t));
    return HU_OK;
}
