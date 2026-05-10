/* LoRA (Low-Rank Adaptation) adapter for fine-tuning. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/lora.h"
#include "human/ml/optimizer.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct hu_lora_adapter {
    hu_allocator_t *alloc;
    hu_lora_config_t config;
    hu_lora_layer_t *layers;
    size_t n_layers;
    size_t in_dim;
    size_t out_dim;
};

/* PRNG matching gpt.c: xorshift-style LCG */
static float prng_next(uint64_t *seed) {
    *seed = *seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return ((float)(*seed >> 33) / (float)(1ULL << 31)) - 0.5f;
}

hu_error_t hu_lora_create(hu_allocator_t *alloc, const hu_lora_config_t *config,
                           size_t in_dim, size_t out_dim, size_t n_layers,
                           hu_lora_adapter_t **out) {
    if (!alloc || !config || !out || n_layers == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (config->rank == 0 || in_dim == 0 || out_dim == 0)
        return HU_ERR_INVALID_ARGUMENT;

    hu_lora_adapter_t *a =
        (hu_lora_adapter_t *)alloc->alloc(alloc->ctx, sizeof(hu_lora_adapter_t));
    if (!a)
        return HU_ERR_OUT_OF_MEMORY;

    a->alloc = alloc;
    a->config = *config;
    a->n_layers = n_layers;
    a->in_dim = in_dim;
    a->out_dim = out_dim;

    a->layers =
        (hu_lora_layer_t *)alloc->alloc(alloc->ctx, n_layers * sizeof(hu_lora_layer_t));
    if (!a->layers) {
        alloc->free(alloc->ctx, a, sizeof(hu_lora_adapter_t));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(a->layers, 0, n_layers * sizeof(hu_lora_layer_t));

    uint64_t seed = 12345;
    float scale = (config->alpha > 0.0f && config->rank > 0)
                      ? (config->alpha / (float)config->rank)
                      : 1.0f;

    for (size_t i = 0; i < n_layers; i++) {
        hu_lora_layer_t *layer = &a->layers[i];
        size_t rank = config->rank;

        layer->in_dim = in_dim;
        layer->out_dim = out_dim;
        layer->rank = rank;
        layer->scale = scale;

        layer->A = (float *)alloc->alloc(alloc->ctx, rank * in_dim * sizeof(float));
        layer->B = (float *)alloc->alloc(alloc->ctx, out_dim * rank * sizeof(float));
        layer->grad_A = (float *)alloc->alloc(alloc->ctx, rank * in_dim * sizeof(float));
        layer->grad_B = (float *)alloc->alloc(alloc->ctx, out_dim * rank * sizeof(float));

        if (!layer->A || !layer->B || !layer->grad_A || !layer->grad_B) {
            if (layer->A)
                alloc->free(alloc->ctx, layer->A, rank * in_dim * sizeof(float));
            if (layer->B)
                alloc->free(alloc->ctx, layer->B, out_dim * rank * sizeof(float));
            if (layer->grad_A)
                alloc->free(alloc->ctx, layer->grad_A, rank * in_dim * sizeof(float));
            if (layer->grad_B)
                alloc->free(alloc->ctx, layer->grad_B, out_dim * rank * sizeof(float));
            for (size_t j = 0; j < i; j++) {
                hu_lora_layer_t *l = &a->layers[j];
                alloc->free(alloc->ctx, l->A, l->rank * l->in_dim * sizeof(float));
                alloc->free(alloc->ctx, l->B, l->out_dim * l->rank * sizeof(float));
                alloc->free(alloc->ctx, l->grad_A, l->rank * l->in_dim * sizeof(float));
                alloc->free(alloc->ctx, l->grad_B, l->out_dim * l->rank * sizeof(float));
            }
            alloc->free(alloc->ctx, a->layers, n_layers * sizeof(hu_lora_layer_t));
            alloc->free(alloc->ctx, a, sizeof(hu_lora_adapter_t));
            return HU_ERR_OUT_OF_MEMORY;
        }

        /* A: small random, B: zeros (initial delta = 0) */
        for (size_t k = 0; k < rank * in_dim; k++)
            layer->A[k] = 0.01f * prng_next(&seed);
        memset(layer->B, 0, out_dim * rank * sizeof(float));
        memset(layer->grad_A, 0, rank * in_dim * sizeof(float));
        memset(layer->grad_B, 0, out_dim * rank * sizeof(float));
    }

    *out = a;
    return HU_OK;
}

void hu_lora_destroy(hu_allocator_t *alloc, hu_lora_adapter_t *adapter) {
    if (!alloc || !adapter)
        return;
    for (size_t i = 0; i < adapter->n_layers; i++) {
        hu_lora_layer_t *l = &adapter->layers[i];
        if (l->A)
            alloc->free(alloc->ctx, l->A, l->rank * l->in_dim * sizeof(float));
        if (l->B)
            alloc->free(alloc->ctx, l->B, l->out_dim * l->rank * sizeof(float));
        if (l->grad_A)
            alloc->free(alloc->ctx, l->grad_A, l->rank * l->in_dim * sizeof(float));
        if (l->grad_B)
            alloc->free(alloc->ctx, l->grad_B, l->out_dim * l->rank * sizeof(float));
    }
    alloc->free(alloc->ctx, adapter->layers,
                adapter->n_layers * sizeof(hu_lora_layer_t));
    alloc->free(alloc->ctx, adapter, sizeof(hu_lora_adapter_t));
}

hu_error_t hu_lora_apply(const hu_lora_adapter_t *adapter, size_t layer_idx,
                          const float *input, size_t batch_tokens, float *output) {
    if (!adapter || !input || !output)
        return HU_ERR_INVALID_ARGUMENT;
    if (layer_idx >= adapter->n_layers)
        return HU_ERR_INVALID_ARGUMENT;

    const hu_lora_layer_t *layer = &adapter->layers[layer_idx];
    size_t in_dim = layer->in_dim;
    size_t out_dim = layer->out_dim;
    size_t rank = layer->rank;
    float scale = layer->scale;

    float *temp =
        (float *)adapter->alloc->alloc(adapter->alloc->ctx,
                                       batch_tokens * rank * sizeof(float));
    if (!temp)
        return HU_ERR_OUT_OF_MEMORY;

    /* temp = input @ A^T  [batch_tokens × rank] */
    for (size_t i = 0; i < batch_tokens; i++)
        for (size_t j = 0; j < rank; j++) {
            float sum = 0.0f;
            for (size_t k = 0; k < in_dim; k++)
                sum += input[i * in_dim + k] * layer->A[j * in_dim + k];
            temp[i * rank + j] = sum;
        }

    /* output += temp @ B^T * scale  [batch_tokens × out_dim] */
    for (size_t i = 0; i < batch_tokens; i++)
        for (size_t j = 0; j < out_dim; j++) {
            float sum = 0.0f;
            for (size_t k = 0; k < rank; k++)
                sum += temp[i * rank + k] * layer->B[j * rank + k];
            output[i * out_dim + j] += sum * scale;
        }

    adapter->alloc->free(adapter->alloc->ctx, temp,
                         batch_tokens * rank * sizeof(float));
    return HU_OK;
}

hu_error_t hu_lora_backward(hu_lora_adapter_t *adapter, size_t layer_idx,
                             const float *input, const float *grad_output,
                             size_t batch_tokens, float *grad_input) {
    if (!adapter || !input || !grad_output)
        return HU_ERR_INVALID_ARGUMENT;
    if (layer_idx >= adapter->n_layers)
        return HU_ERR_INVALID_ARGUMENT;

    hu_lora_layer_t *layer = &adapter->layers[layer_idx];
    size_t in_dim = layer->in_dim;
    size_t out_dim = layer->out_dim;
    size_t rank = layer->rank;
    float scale = layer->scale;
    size_t bt_rank = batch_tokens * rank;

    /* temp = input @ A^T  [BT × rank] */
    float *temp =
        (float *)adapter->alloc->alloc(adapter->alloc->ctx, bt_rank * sizeof(float));
    if (!temp)
        return HU_ERR_OUT_OF_MEMORY;

    for (size_t i = 0; i < batch_tokens; i++)
        for (size_t j = 0; j < rank; j++) {
            float sum = 0.0f;
            for (size_t k = 0; k < in_dim; k++)
                sum += input[i * in_dim + k] * layer->A[j * in_dim + k];
            temp[i * rank + j] = sum;
        }

    /* grad_B += grad_output^T @ temp * scale  [out_dim × rank] */
    for (size_t j = 0; j < out_dim; j++)
        for (size_t k = 0; k < rank; k++) {
            float sum = 0.0f;
            for (size_t i = 0; i < batch_tokens; i++)
                sum += grad_output[i * out_dim + j] * temp[i * rank + k];
            layer->grad_B[j * rank + k] += sum * scale;
        }

    /* gob = grad_output @ B  [BT × rank] — reuse temp buffer */
    float *gob =
        (float *)adapter->alloc->alloc(adapter->alloc->ctx, bt_rank * sizeof(float));
    if (!gob) {
        adapter->alloc->free(adapter->alloc->ctx, temp, bt_rank * sizeof(float));
        return HU_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < batch_tokens; i++)
        for (size_t j = 0; j < rank; j++) {
            float sum = 0.0f;
            for (size_t m = 0; m < out_dim; m++)
                sum += grad_output[i * out_dim + m] * layer->B[m * rank + j];
            gob[i * rank + j] = sum;
        }

    /* grad_A += gob^T @ input * scale  [rank × in_dim] */
    for (size_t j = 0; j < rank; j++)
        for (size_t k = 0; k < in_dim; k++) {
            float sum = 0.0f;
            for (size_t i = 0; i < batch_tokens; i++)
                sum += gob[i * rank + j] * input[i * in_dim + k];
            layer->grad_A[j * in_dim + k] += sum * scale;
        }

    /* grad_input += gob @ A * scale  [BT × in_dim] */
    if (grad_input) {
        for (size_t i = 0; i < batch_tokens; i++)
            for (size_t k = 0; k < in_dim; k++) {
                float sum = 0.0f;
                for (size_t j = 0; j < rank; j++)
                    sum += gob[i * rank + j] * layer->A[j * in_dim + k];
                grad_input[i * in_dim + k] += sum * scale;
            }
    }

    adapter->alloc->free(adapter->alloc->ctx, gob, bt_rank * sizeof(float));
    adapter->alloc->free(adapter->alloc->ctx, temp, bt_rank * sizeof(float));
    return HU_OK;
}

hu_error_t hu_lora_register_params(hu_lora_adapter_t *adapter, hu_ml_optimizer_t *opt) {
    if (!adapter || !opt)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < adapter->n_layers; i++) {
        hu_lora_layer_t *layer = &adapter->layers[i];
        hu_error_t err =
            hu_muon_adamw_add_param(opt, layer->A, layer->grad_A, layer->rank,
                                    layer->in_dim, HU_PARAM_MATRIX);
        if (err != HU_OK)
            return err;
        err = hu_muon_adamw_add_param(opt, layer->B, layer->grad_B, layer->out_dim,
                                       layer->rank, HU_PARAM_MATRIX);
        if (err != HU_OK)
            return err;
    }
    return HU_OK;
}

void hu_lora_get_dims(const hu_lora_adapter_t *adapter, size_t *out_in_dim,
                      size_t *out_out_dim, size_t *out_n_layers) {
    if (out_in_dim)
        *out_in_dim = adapter ? adapter->in_dim : 0;
    if (out_out_dim)
        *out_out_dim = adapter ? adapter->out_dim : 0;
    if (out_n_layers)
        *out_n_layers = adapter ? adapter->n_layers : 0;
}

size_t hu_lora_num_params(const hu_lora_adapter_t *adapter) {
    if (!adapter)
        return 0;
    size_t n = 0;
    for (size_t i = 0; i < adapter->n_layers; i++) {
        const hu_lora_layer_t *l = &adapter->layers[i];
        n += l->rank * l->in_dim + l->out_dim * l->rank;
    }
    return n;
}

hu_error_t hu_lora_save(const hu_lora_adapter_t *adapter, const char *path) {
    if (!adapter || !path)
        return HU_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(path, "wb");
    if (!f)
        return HU_ERR_IO;

    const char magic[] = "LORA";
    if (fwrite(magic, 1, 4, f) != 4)
        goto fail;
    uint32_t version = 1;
    if (fwrite(&version, sizeof(version), 1, f) != 1)
        goto fail;
    if (fwrite(&adapter->config, sizeof(adapter->config), 1, f) != 1)
        goto fail;
    uint32_t n_layers = (uint32_t)adapter->n_layers;
    uint32_t in_dim = (uint32_t)adapter->in_dim;
    uint32_t out_dim = (uint32_t)adapter->out_dim;
    if (fwrite(&n_layers, sizeof(n_layers), 1, f) != 1)
        goto fail;
    if (fwrite(&in_dim, sizeof(in_dim), 1, f) != 1)
        goto fail;
    if (fwrite(&out_dim, sizeof(out_dim), 1, f) != 1)
        goto fail;

    for (size_t i = 0; i < adapter->n_layers; i++) {
        const hu_lora_layer_t *l = &adapter->layers[i];
        size_t a_size = l->rank * l->in_dim * sizeof(float);
        size_t b_size = l->out_dim * l->rank * sizeof(float);
        if (fwrite(l->A, 1, a_size, f) != a_size)
            goto fail;
        if (fwrite(l->B, 1, b_size, f) != b_size)
            goto fail;
    }

    fclose(f);
    return HU_OK;
fail:
    fclose(f);
    return HU_ERR_IO;
}

hu_error_t hu_lora_set_layer_weights(hu_lora_adapter_t *adapter, size_t layer_idx,
                                     const float *A, const float *B) {
    if (!adapter || layer_idx >= adapter->n_layers)
        return HU_ERR_INVALID_ARGUMENT;
    hu_lora_layer_t *layer = &adapter->layers[layer_idx];
    if (A)
        memcpy(layer->A, A, layer->rank * layer->in_dim * sizeof(float));
    if (B)
        memcpy(layer->B, B, layer->out_dim * layer->rank * sizeof(float));
    return HU_OK;
}

hu_error_t hu_lora_load(hu_allocator_t *alloc, const char *path,
                        hu_lora_adapter_t **out) {
    if (!alloc || !path || !out)
        return HU_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_IO;

    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "LORA", 4) != 0)
        goto fail;
    uint32_t version = 0;
    if (fread(&version, sizeof(version), 1, f) != 1 || version != 1)
        goto fail;

    hu_lora_config_t config;
    if (fread(&config, sizeof(config), 1, f) != 1)
        goto fail;
    uint32_t n_layers, in_dim, out_dim;
    if (fread(&n_layers, sizeof(n_layers), 1, f) != 1)
        goto fail;
    if (fread(&in_dim, sizeof(in_dim), 1, f) != 1)
        goto fail;
    if (fread(&out_dim, sizeof(out_dim), 1, f) != 1)
        goto fail;

    hu_error_t err = hu_lora_create(alloc, &config, in_dim, out_dim, n_layers, out);
    if (err != HU_OK)
        goto fail;

    for (size_t i = 0; i < (*out)->n_layers; i++) {
        hu_lora_layer_t *l = &(*out)->layers[i];
        size_t a_size = l->rank * l->in_dim * sizeof(float);
        size_t b_size = l->out_dim * l->rank * sizeof(float);
        if (fread(l->A, 1, a_size, f) != a_size)
            goto fail_load;
        if (fread(l->B, 1, b_size, f) != b_size)
            goto fail_load;
    }

    fclose(f);
    return HU_OK;
fail_load:
    hu_lora_destroy(alloc, *out);
    *out = NULL;
fail:
    fclose(f);
    return HU_ERR_IO;
}
