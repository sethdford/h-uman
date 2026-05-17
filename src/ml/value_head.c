/* src/ml/value_head.c — Phase 3 Task 1
 *
 * Linear value head used as the trainable surface on top of frozen
 * backbones in Phase 3's reward-model composition. See
 * include/human/ml/value_head.h for the full forward/backward contract
 * and save-format spec.
 *
 * Style mirrors src/ml/dpo_real_huml.c and src/ml/lora.c:
 *   - hu_allocator_t with 3-arg free (ctx, ptr, size) — see
 *     include/human/core/allocator.h:11 and the file-header note on
 *     dpo_real_huml.c lines 73-77.
 *   - Save/load via fopen("wb") / fopen("rb") + raw magic header,
 *     matching hu_lora_save's "LORA" convention (src/ml/lora.c:285-389).
 *   - Double accumulators in the inner product to avoid the
 *     summation-order drift that hits float32 dot products for
 *     hidden_dim ≳ 1024 (the frontier-backbone hidden width).
 */
#include "human/ml/value_head.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* M_PI is not in C11/C17 (it's a POSIX/BSD extension). Apple Clang's
 * math.h provides it unconditionally, but providing a fallback keeps
 * the file portable under the project's -std=c11 -Wpedantic -Werror
 * (CMakeLists.txt:3206) compile contract. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Xavier-Glorot stddev = sqrt(2.0 / fan_in). Fan-in for a [hidden_dim, 1]
 * projection is hidden_dim; fan-out is 1, so the He-style 2/fan_in form
 * is equivalent to Xavier-uniform's effective scale here and gives a
 * well-conditioned init regardless of hidden width. Mirrors the
 * 2.0/fan_in initialization used in src/ml/gpt.c for the LM head. */
static float xavier_sample(double stddev) {
    /* Box-Muller transform on rand() — deterministic per process so the
     * test suite is reproducible without seeding from /dev/urandom. We
     * intentionally do not seed; callers that need a specific init can
     * overwrite vh->W in place (the forward/backward test does exactly
     * this — see tests/test_value_head.c). */
    double u1 = ((double)rand() + 1.0) / ((double)RAND_MAX + 2.0);
    double u2 = ((double)rand() + 1.0) / ((double)RAND_MAX + 2.0);
    double z = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
    return (float)(z * stddev);
}

hu_error_t hu_value_head_create(hu_allocator_t *alloc, size_t hidden_dim,
                                hu_value_head_t *out) {
    if (!alloc || !alloc->alloc || hidden_dim == 0 || !out) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    out->W = NULL;
    out->b = 0.0f;
    out->hidden_dim = 0;

    float *W = (float *)alloc->alloc(alloc->ctx, hidden_dim * sizeof(float));
    if (!W) {
        return HU_ERR_OUT_OF_MEMORY;
    }
    const double stddev = sqrt(2.0 / (double)hidden_dim);
    for (size_t i = 0; i < hidden_dim; i++) {
        W[i] = xavier_sample(stddev);
    }
    out->W = W;
    out->b = 0.0f;
    out->hidden_dim = hidden_dim;
    return HU_OK;
}

hu_error_t hu_value_head_forward(const hu_value_head_t *vh, const float *h,
                                 double *out_score) {
    if (!vh || !vh->W || !h || !out_score) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    double s = (double)vh->b;
    for (size_t i = 0; i < vh->hidden_dim; i++) {
        s += (double)vh->W[i] * (double)h[i];
    }
    *out_score = s;
    return HU_OK;
}

hu_error_t hu_value_head_backward(const hu_value_head_t *vh, const float *h,
                                  double dL_dscore,
                                  float *out_dW, float *out_db, float *out_dh) {
    if (!vh || !vh->W || !h) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (out_dW) {
        for (size_t i = 0; i < vh->hidden_dim; i++) {
            out_dW[i] = (float)((double)h[i] * dL_dscore);
        }
    }
    if (out_db) {
        *out_db = (float)dL_dscore;
    }
    if (out_dh) {
        for (size_t i = 0; i < vh->hidden_dim; i++) {
            out_dh[i] = (float)((double)vh->W[i] * dL_dscore);
        }
    }
    return HU_OK;
}

hu_error_t hu_value_head_save(const hu_value_head_t *vh, const char *path) {
    if (!vh || !vh->W || !path || vh->hidden_dim == 0) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (vh->hidden_dim > UINT32_MAX) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        return HU_ERR_IO;
    }
    const char magic[4] = {'V', 'H', 'E', 'D'};
    if (fwrite(magic, 1, 4, f) != 4) goto fail;
    uint32_t hd = (uint32_t)vh->hidden_dim;
    if (fwrite(&hd, sizeof(hd), 1, f) != 1) goto fail;
    size_t W_bytes = vh->hidden_dim * sizeof(float);
    if (fwrite(vh->W, 1, W_bytes, f) != W_bytes) goto fail;
    if (fwrite(&vh->b, sizeof(float), 1, f) != 1) goto fail;
    fclose(f);
    return HU_OK;
fail:
    fclose(f);
    return HU_ERR_IO;
}

hu_error_t hu_value_head_load(hu_allocator_t *alloc, const char *path,
                              hu_value_head_t *out) {
    if (!alloc || !alloc->alloc || !path || !out) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    out->W = NULL;
    out->b = 0.0f;
    out->hidden_dim = 0;

    FILE *f = fopen(path, "rb");
    if (!f) {
        return HU_ERR_IO;
    }
    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "VHED", 4) != 0) {
        fclose(f);
        return HU_ERR_PARSE;
    }
    uint32_t hd = 0;
    if (fread(&hd, sizeof(hd), 1, f) != 1 || hd == 0) {
        fclose(f);
        return HU_ERR_PARSE;
    }
    float *W = (float *)alloc->alloc(alloc->ctx, (size_t)hd * sizeof(float));
    if (!W) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t W_bytes = (size_t)hd * sizeof(float);
    if (fread(W, 1, W_bytes, f) != W_bytes) {
        alloc->free(alloc->ctx, W, W_bytes);
        fclose(f);
        return HU_ERR_IO;
    }
    float b = 0.0f;
    if (fread(&b, sizeof(float), 1, f) != 1) {
        alloc->free(alloc->ctx, W, W_bytes);
        fclose(f);
        return HU_ERR_IO;
    }
    fclose(f);
    out->W = W;
    out->b = b;
    out->hidden_dim = (size_t)hd;
    return HU_OK;
}

void hu_value_head_deinit(hu_value_head_t *vh, hu_allocator_t *alloc) {
    if (!vh) return;
    if (vh->W && alloc && alloc->free) {
        alloc->free(alloc->ctx, vh->W, vh->hidden_dim * sizeof(float));
    }
    vh->W = NULL;
    vh->b = 0.0f;
    vh->hidden_dim = 0;
}
