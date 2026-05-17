#include "human/provider_test_seam.h"

#if HU_IS_TEST

#include "human/core/error.h"
#include <stdio.h>
#include <string.h>

typedef struct hu_provider_test_ctx {
    hu_allocator_t *alloc;
    char canned[256];
    int unload_count;
    int load_count;
} hu_provider_test_ctx_t;

static hu_error_t test_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                            const char *model, size_t model_len, double temperature,
                            hu_chat_response_t *out) {
    (void)model;
    (void)model_len;
    (void)temperature;
    (void)request;
    if (!ctx || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_provider_test_ctx_t *t = (hu_provider_test_ctx_t *)ctx;
    const char *text = t->canned;
    if (strncmp(text, "canned:", 7) == 0)
        text += 7;
    while (*text == ' ')
        text++;
    size_t len = strlen(text);
    char *copy = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!copy)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(copy, text, len);
    copy[len] = '\0';
    out->content = copy;
    out->content_len = len;
    return HU_OK;
}

static hu_error_t test_unload(void *ctx, const char *adapter_id, size_t adapter_id_len) {
    (void)adapter_id;
    (void)adapter_id_len;
    if (!ctx)
        return HU_ERR_INVALID_ARGUMENT;
    ((hu_provider_test_ctx_t *)ctx)->unload_count++;
    return HU_OK;
}

static hu_error_t test_load(void *ctx, hu_allocator_t *alloc, const char *adapter_path,
                            size_t adapter_path_len, const char *adapter_id,
                            size_t adapter_id_len) {
    (void)alloc;
    (void)adapter_path;
    (void)adapter_path_len;
    (void)adapter_id;
    (void)adapter_id_len;
    if (!ctx)
        return HU_ERR_INVALID_ARGUMENT;
    ((hu_provider_test_ctx_t *)ctx)->load_count++;
    return HU_OK;
}

static const char *test_name(void *ctx) {
    (void)ctx;
    return "provider_test";
}

static void test_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx || !alloc)
        return;
    alloc->free(alloc->ctx, ctx, sizeof(hu_provider_test_ctx_t));
}

static hu_error_t test_chat_with_system_full(void *ctx, hu_allocator_t *alloc,
                                              const char *system_prompt,
                                              size_t system_prompt_len, const char *message,
                                              size_t message_len, const char *model,
                                              size_t model_len, double temperature, char **out,
                                              size_t *out_len) {
    (void)system_prompt;
    (void)system_prompt_len;
    (void)message;
    (void)message_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    if (!ctx || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    hu_provider_test_ctx_t *t = (hu_provider_test_ctx_t *)ctx;
    const char *text = t->canned;
    if (strncmp(text, "canned:", 7) == 0)
        text += 7;
    while (*text == ' ')
        text++;
    size_t len = strlen(text);
    char *copy = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!copy)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(copy, text, len);
    copy[len] = '\0';
    *out = copy;
    *out_len = len;
    return HU_OK;
}

static const hu_provider_vtable_t TEST_VTABLE = {
    .chat_with_system = test_chat_with_system_full,
    .chat = test_chat,
    .supports_native_tools = NULL,
    .get_name = test_name,
    .deinit = test_deinit,
    .unload_adapter = test_unload,
    .load_adapter = test_load,
};

hu_error_t hu_provider_create_for_test_with_canned_response(hu_allocator_t *alloc,
                                                            const char *canned,
                                                            hu_provider_t **out) {
    if (!alloc || !canned || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_provider_test_ctx_t *ctx =
        (hu_provider_test_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*ctx));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = alloc;
    snprintf(ctx->canned, sizeof(ctx->canned), "%s", canned);

    hu_provider_t *p =
        (hu_provider_t *)alloc->alloc(alloc->ctx, sizeof(hu_provider_t));
    if (!p) {
        alloc->free(alloc->ctx, ctx, sizeof(*ctx));
        return HU_ERR_OUT_OF_MEMORY;
    }
    p->ctx = ctx;
    p->vtable = &TEST_VTABLE;
    *out = p;
    return HU_OK;
}

void hu_provider_destroy_for_test(hu_provider_t *provider, hu_allocator_t *alloc) {
    if (!provider || !alloc)
        return;
    if (provider->vtable && provider->vtable->deinit)
        provider->vtable->deinit(provider->ctx, alloc);
    alloc->free(alloc->ctx, provider, sizeof(hu_provider_t));
}

int hu_provider_unload_called_count_for_test(const hu_provider_t *provider) {
    if (!provider || !provider->ctx)
        return 0;
    return ((const hu_provider_test_ctx_t *)provider->ctx)->unload_count;
}

int hu_provider_load_adapter_called_count_for_test(const hu_provider_t *provider) {
    if (!provider || !provider->ctx)
        return 0;
    return ((const hu_provider_test_ctx_t *)provider->ctx)->load_count;
}

#endif /* HU_IS_TEST */
