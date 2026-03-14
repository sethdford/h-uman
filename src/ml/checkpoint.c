/* Checkpoint save/load for ML models — binary format (HUML magic, version 1). */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/checkpoint.h"
#include "human/ml/model.h"
#include "human/ml/optimizer.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define HUML_MAGIC 0x48554D4C
#define HUML_VERSION 1

hu_error_t hu_ml_checkpoint_save(hu_allocator_t *alloc, const char *path,
                                 hu_model_t *model, hu_ml_optimizer_t *opt)
{
    (void)alloc;
    (void)opt;
    if (!alloc || !path || !model || !opt)
        return HU_ERR_INVALID_ARGUMENT;
    if (!model->vtable || !model->vtable->get_params)
        return HU_ERR_INVALID_ARGUMENT;

    hu_ml_tensor_t *params = NULL;
    size_t count = 0;
    hu_error_t err = model->vtable->get_params(model->ctx, &params, &count);
    if (err != HU_OK)
        return err;

    FILE *f = fopen(path, "wb");
    if (!f)
        return HU_ERR_IO;
    uint32_t magic = (uint32_t)HUML_MAGIC;
    uint32_t version = (uint32_t)HUML_VERSION;
    if (fwrite(&magic, sizeof(magic), 1, f) != 1 ||
        fwrite(&version, sizeof(version), 1, f) != 1 ||
        fwrite(&count, sizeof(count), 1, f) != 1) {
        fclose(f);
        return HU_ERR_IO;
    }
    for (size_t i = 0; i < count; i++) {
        size_t sz = params[i].size_bytes;
        if (fwrite(&sz, sizeof(sz), 1, f) != 1)
            goto fail;
        if (sz > 0 && (!params[i].data || fwrite(params[i].data, 1, sz, f) != sz))
            goto fail;
    }
    fclose(f);
    return HU_OK;
fail:
    fclose(f);
    return HU_ERR_IO;
}

hu_error_t hu_ml_checkpoint_load(hu_allocator_t *alloc, const char *path,
                                 hu_model_t *model, hu_ml_optimizer_t *opt)
{
    (void)alloc;
    (void)opt;
    if (!alloc || !path || !model || !opt)
        return HU_ERR_INVALID_ARGUMENT;
    if (!model->vtable || !model->vtable->get_params)
        return HU_ERR_INVALID_ARGUMENT;

    hu_ml_tensor_t *params = NULL;
    size_t count = 0;
    hu_error_t err = model->vtable->get_params(model->ctx, &params, &count);
    if (err != HU_OK)
        return err;

    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_IO;
    uint32_t magic = 0, version = 0;
    size_t file_count = 0;
    if (fread(&magic, sizeof(magic), 1, f) != 1 ||
        fread(&version, sizeof(version), 1, f) != 1 ||
        fread(&file_count, sizeof(file_count), 1, f) != 1 ||
        magic != (uint32_t)HUML_MAGIC || version != (uint32_t)HUML_VERSION ||
        file_count != count) {
        fclose(f);
        return HU_ERR_IO;
    }
    for (size_t i = 0; i < count; i++) {
        size_t sz = 0;
        if (fread(&sz, sizeof(sz), 1, f) != 1 || sz != params[i].size_bytes)
            goto fail_load;
        if (sz > 0 && (!params[i].data || fread(params[i].data, 1, sz, f) != sz))
            goto fail_load;
    }
    fclose(f);
    return HU_OK;
fail_load:
    fclose(f);
    return HU_ERR_IO;
}
