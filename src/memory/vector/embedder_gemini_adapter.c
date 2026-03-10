#include "human/memory/vector/embedder_gemini_adapter.h"
#include <string.h>

typedef struct gemini_adapter_ctx {
    hu_embedding_provider_t provider;
} gemini_adapter_ctx_t;

static hu_error_t embed_impl(void *ctx, hu_allocator_t *alloc, const char *text, size_t text_len,
                             hu_embedding_t *out) {
    gemini_adapter_ctx_t *adapter = (gemini_adapter_ctx_t *)ctx;
    if (!adapter || !adapter->provider.vtable || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    if (text_len > 0 && !text)
        return HU_ERR_INVALID_ARGUMENT;

    hu_embedding_provider_result_t result = {0};
    hu_error_t err =
        adapter->provider.vtable->embed(adapter->provider.ctx, alloc, text, text_len, &result);
    if (err != HU_OK)
        return err;

    if (result.dimensions == 0) {
        hu_embedding_provider_free(alloc, &result);
        out->values = NULL;
        out->dim = 0;
        return HU_OK;
    }

    out->values = (float *)alloc->alloc(alloc->ctx, result.dimensions * sizeof(float));
    if (!out->values) {
        hu_embedding_provider_free(alloc, &result);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(out->values, result.values, result.dimensions * sizeof(float));
    out->dim = result.dimensions;
    hu_embedding_provider_free(alloc, &result);
    return HU_OK;
}

static hu_error_t embed_batch_impl(void *ctx, hu_allocator_t *alloc, const char **texts,
                                   const size_t *text_lens, size_t count, hu_embedding_t *out) {
    if (count > 0 && (!texts || !text_lens || !out))
        return HU_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < count; i++) {
        hu_error_t err = embed_impl(ctx, alloc, texts[i], text_lens[i], &out[i]);
        if (err != HU_OK) {
            for (size_t j = 0; j < i; j++)
                hu_embedding_free(alloc, &out[j]);
            return err;
        }
    }
    return HU_OK;
}

static size_t dimensions_impl(void *ctx) {
    gemini_adapter_ctx_t *adapter = (gemini_adapter_ctx_t *)ctx;
    if (!adapter || !adapter->provider.vtable)
        return 0;
    return adapter->provider.vtable->dimensions(adapter->provider.ctx);
}

static void deinit_impl(void *ctx, hu_allocator_t *alloc) {
    if (!ctx || !alloc)
        return;
    gemini_adapter_ctx_t *adapter = (gemini_adapter_ctx_t *)ctx;
    if (adapter->provider.vtable && adapter->provider.ctx)
        adapter->provider.vtable->deinit(adapter->provider.ctx, alloc);
    alloc->free(alloc->ctx, ctx, sizeof(gemini_adapter_ctx_t));
}

static const hu_embedder_vtable_t gemini_adapter_vtable = {
    .embed = embed_impl,
    .embed_batch = embed_batch_impl,
    .dimensions = dimensions_impl,
    .deinit = deinit_impl,
};

hu_embedder_t hu_embedder_gemini_adapter_create(hu_allocator_t *alloc,
                                                hu_embedding_provider_t provider) {
    hu_embedder_t emb = {.ctx = NULL, .vtable = &gemini_adapter_vtable};
    if (!alloc || !provider.vtable)
        return emb;

    gemini_adapter_ctx_t *ctx =
        (gemini_adapter_ctx_t *)alloc->alloc(alloc->ctx, sizeof(gemini_adapter_ctx_t));
    if (!ctx) {
        if (provider.vtable && provider.vtable->deinit)
            provider.vtable->deinit(provider.ctx, alloc);
        return emb;
    }
    ctx->provider = provider;

    emb.ctx = ctx;
    return emb;
}
