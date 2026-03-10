/*
 * Template: Custom AI Provider
 *
 * Implements hu_provider_t vtable. Required methods:
 *   - chat_with_system: simple system+user -> text
 *   - chat: full multi-message chat with optional tool calls
 *   - supports_native_tools
 *   - get_name
 *   - deinit
 *
 * Optional (may be NULL): warmup, chat_with_tools, supports_streaming,
 *   supports_vision, stream_chat
 */
#include "my_provider.h"
#include "human/provider.h"
#include "human/core/json.h"
#include "human/core/http.h"
#include "human/core/string.h"
#include <string.h>
#include <stdlib.h>

typedef struct hu_my_provider_ctx {
    char *api_key;
    size_t api_key_len;
    char *base_url;
    size_t base_url_len;
} hu_my_provider_ctx_t;

/* chat_with_system: single system prompt + user message -> text output */
static hu_error_t my_provider_chat_with_system(void *ctx, hu_allocator_t *alloc,
    const char *system_prompt, size_t system_prompt_len,
    const char *message, size_t message_len,
    const char *model, size_t model_len,
    double temperature,
    char **out, size_t *out_len)
{
    (void)alloc;
    (void)system_prompt;
    (void)system_prompt_len;
    (void)message;
    (void)message_len;
    (void)model;
    (void)model_len;
    (void)temperature;

#if HU_IS_TEST
    *out = hu_strndup(alloc, "(my_provider stub)", 18);
    if (!*out) return HU_ERR_OUT_OF_MEMORY;
    *out_len = 18;
    return HU_OK;
#else
    hu_my_provider_ctx_t *c = (hu_my_provider_ctx_t *)ctx;
    if (!c || !out || !out_len) return HU_ERR_INVALID_ARGUMENT;

    /* TODO: Build JSON request, call HTTP API, parse response.
     * Use hu_http_post_json or hu_http_post_json_ex. */
    (void)c;
    *out = NULL;
    *out_len = 0;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

/* chat: full chat request with optional tool calls */
static hu_error_t my_provider_chat(void *ctx, hu_allocator_t *alloc,
    const hu_chat_request_t *request,
    const char *model, size_t model_len,
    double temperature,
    hu_chat_response_t *out)
{
    (void)ctx;
    (void)alloc;
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    (void)out;

#if HU_IS_TEST
    memset(out, 0, sizeof(*out));
    out->content = hu_strndup(alloc, "(my_provider chat stub)", 24);
    if (out->content) out->content_len = 24;
    return HU_OK;
#else
    return HU_ERR_NOT_SUPPORTED;
#endif
}

static bool my_provider_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}

static const char *my_provider_get_name(void *ctx) {
    (void)ctx;
    return "my_provider";
}

static void my_provider_deinit(void *ctx, hu_allocator_t *alloc) {
    hu_my_provider_ctx_t *c = (hu_my_provider_ctx_t *)ctx;
    if (!c) return;
    if (c->api_key) alloc->free(alloc->ctx, c->api_key, c->api_key_len + 1);
    if (c->base_url) alloc->free(alloc->ctx, c->base_url, c->base_url_len + 1);
    alloc->free(alloc->ctx, c, sizeof(*c));
}

static const hu_provider_vtable_t my_provider_vtable = {
    .chat_with_system = my_provider_chat_with_system,
    .chat = my_provider_chat,
    .supports_native_tools = my_provider_supports_native_tools,
    .get_name = my_provider_get_name,
    .deinit = my_provider_deinit,
    .warmup = NULL,
    .chat_with_tools = NULL,
    .supports_streaming = NULL,
    .supports_vision = NULL,
    .supports_vision_for_model = NULL,
    .stream_chat = NULL,
};

hu_error_t hu_my_provider_create(hu_allocator_t *alloc,
    const char *api_key, size_t api_key_len,
    const char *base_url, size_t base_url_len,
    hu_provider_t *out)
{
    if (!alloc || !out) return HU_ERR_INVALID_ARGUMENT;

    hu_my_provider_ctx_t *c = (hu_my_provider_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c) return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));

    if (api_key && api_key_len > 0) {
        c->api_key = hu_strndup(alloc, api_key, api_key_len);
        if (!c->api_key) { alloc->free(alloc->ctx, c, sizeof(*c)); return HU_ERR_OUT_OF_MEMORY; }
        c->api_key_len = api_key_len;
    }
    if (base_url && base_url_len > 0) {
        c->base_url = hu_strndup(alloc, base_url, base_url_len);
        if (!c->base_url) {
            if (c->api_key) alloc->free(alloc->ctx, c->api_key, c->api_key_len + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        c->base_url_len = base_url_len;
    }

    out->ctx = c;
    out->vtable = &my_provider_vtable;
    return HU_OK;
}

void hu_my_provider_destroy(hu_provider_t *prov, hu_allocator_t *alloc) {
    if (prov && prov->vtable && prov->vtable->deinit && alloc) {
        prov->vtable->deinit(prov->ctx, alloc);
        prov->ctx = NULL;
        prov->vtable = NULL;
    }
}
