/* WASM-compatible OpenAI-style provider using hu_host_http_fetch. */
#ifdef __wasi__

#include "human/provider.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/wasm/host_imports.h"
#include <string.h>
#include <stdlib.h>

#define HU_OPENAI_URL "https://api.openai.com/v1/chat/completions"
#define HU_OPENAI_URL_LEN (sizeof(HU_OPENAI_URL) - 1)
#define HU_HTTP_RESPONSE_MAX 262144  /* 256K */

typedef struct hu_wasm_provider_ctx {
    hu_allocator_t alloc;
    char *api_key;
    size_t api_key_len;
    char *base_url;
    size_t base_url_len;
} hu_wasm_provider_ctx_t;

static hu_error_t wasm_http_post(hu_allocator_t *alloc,
    const char *url, size_t url_len,
    const char *auth_header, size_t auth_len,
    const char *body, size_t body_len,
    char **response_out, size_t *response_len_out)
{
    (void)auth_header;
    (void)auth_len;
    char *resp_buf = (char *)alloc->alloc(alloc->ctx, HU_HTTP_RESPONSE_MAX);
    if (!resp_buf) return HU_ERR_OUT_OF_MEMORY;

    const char *method = "POST";
    int n = hu_host_http_fetch(
        url, (int)url_len,
        method, 4,
        body, (int)body_len,
        resp_buf, HU_HTTP_RESPONSE_MAX);
    if (n < 0) {
        alloc->free(alloc->ctx, resp_buf, HU_HTTP_RESPONSE_MAX);
        return HU_ERR_PROVIDER_RESPONSE;
    }
    *response_out = resp_buf;
    *response_len_out = (size_t)n;
    return HU_OK;
}

static hu_error_t wasm_chat(void *ctx, hu_allocator_t *alloc,
    const hu_chat_request_t *request,
    const char *model, size_t model_len,
    double temperature,
    hu_chat_response_t *out);

static hu_error_t wasm_chat_with_system(void *ctx, hu_allocator_t *alloc,
    const char *system_prompt, size_t system_prompt_len,
    const char *message, size_t message_len,
    const char *model, size_t model_len,
    double temperature,
    char **out, size_t *out_len)
{
    hu_chat_message_t msgs[2] = {
        { .role = HU_ROLE_SYSTEM, .content = system_prompt, .content_len = system_prompt_len,
          .name = NULL, .name_len = 0, .tool_call_id = NULL, .tool_call_id_len = 0,
          .content_parts = NULL, .content_parts_count = 0 },
        { .role = HU_ROLE_USER, .content = message, .content_len = message_len,
          .name = NULL, .name_len = 0, .tool_call_id = NULL, .tool_call_id_len = 0,
          .content_parts = NULL, .content_parts_count = 0 },
    };
    hu_chat_request_t req = {
        .messages = msgs, .messages_count = 2,
        .model = model, .model_len = model_len,
        .temperature = temperature, .max_tokens = 0,
        .tools = NULL, .tools_count = 0, .timeout_secs = 0,
        .reasoning_effort = NULL, .reasoning_effort_len = 0,
    };
    hu_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));
    hu_error_t err = wasm_chat(ctx, alloc, &req, model, model_len, temperature, &resp);
    if (err != HU_OK) return err;
    if (resp.content && resp.content_len > 0) {
        *out = hu_strndup(alloc, resp.content, resp.content_len);
        if (!*out) return HU_ERR_OUT_OF_MEMORY;
        *out_len = resp.content_len;
    } else {
        *out = NULL;
        *out_len = 0;
    }
    return HU_OK;
}

static hu_error_t wasm_chat(void *ctx, hu_allocator_t *alloc,
    const hu_chat_request_t *request,
    const char *model, size_t model_len,
    double temperature,
    hu_chat_response_t *out)
{
    hu_wasm_provider_ctx_t *wc = (hu_wasm_provider_ctx_t *)ctx;
    if (!wc || !request || !out) return HU_ERR_INVALID_ARGUMENT;

    hu_json_value_t *root = hu_json_object_new(alloc);
    if (!root) return HU_ERR_OUT_OF_MEMORY;
    hu_json_value_t *msgs_arr = hu_json_array_new(alloc);
    if (!msgs_arr) { hu_json_free(alloc, root); return HU_ERR_OUT_OF_MEMORY; }
    (void)hu_json_object_set(alloc, root, "messages", msgs_arr);

    for (size_t i = 0; i < request->messages_count; i++) {
        const hu_chat_message_t *m = &request->messages[i];
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj) { hu_json_free(alloc, root); return HU_ERR_OUT_OF_MEMORY; }
        const char *role_str = "user";
        if (m->role == HU_ROLE_SYSTEM) role_str = "system";
        else if (m->role == HU_ROLE_ASSISTANT) role_str = "assistant";
        else if (m->role == HU_ROLE_TOOL) role_str = "tool";
        hu_json_value_t *role_val = hu_json_string_new(alloc, role_str, strlen(role_str));
        hu_json_object_set(alloc, obj, "role", role_val);
        if (m->content && m->content_len > 0) {
            hu_json_value_t *cval = hu_json_string_new(alloc, m->content, m->content_len);
            hu_json_object_set(alloc, obj, "content", cval);
        }
        if (m->role == HU_ROLE_TOOL && m->tool_call_id) {
            hu_json_value_t *id_val = hu_json_string_new(alloc, m->tool_call_id, m->tool_call_id_len);
            hu_json_object_set(alloc, obj, "tool_call_id", id_val);
        }
        if (m->role == HU_ROLE_TOOL && m->name) {
            hu_json_value_t *nval = hu_json_string_new(alloc, m->name, m->name_len);
            hu_json_object_set(alloc, obj, "name", nval);
        }
        hu_json_array_push(alloc, msgs_arr, obj);
    }

    hu_json_value_t *model_val = hu_json_string_new(alloc, model, model_len);
    hu_json_object_set(alloc, root, "model", model_val);
    hu_json_value_t *temp_val = hu_json_number_new(alloc, temperature);
    hu_json_object_set(alloc, root, "temperature", temp_val);

    char *body = NULL;
    size_t body_len = 0;
    hu_error_t err = hu_json_stringify(alloc, root, &body, &body_len);
    hu_json_free(alloc, root);
    if (err != HU_OK) return err;

    const char *url = wc->base_url ? wc->base_url : HU_OPENAI_URL;
    size_t url_len = wc->base_url_len ? wc->base_url_len : HU_OPENAI_URL_LEN;

    char *resp_body = NULL;
    size_t resp_len = 0;
    err = wasm_http_post(alloc, url, url_len, NULL, 0, body, body_len, &resp_body, &resp_len);
    alloc->free(alloc->ctx, body, body_len);
    if (err != HU_OK) return err;

    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(alloc, resp_body, resp_len, &parsed);
    alloc->free(alloc->ctx, resp_body, resp_len);
    if (err != HU_OK) return err;

    memset(out, 0, sizeof(*out));
    hu_json_value_t *choices = hu_json_object_get(parsed, "choices");
    if (choices && choices->type == HU_JSON_ARRAY && choices->data.array.len > 0) {
        hu_json_value_t *first = choices->data.array.items[0];
        hu_json_value_t *msg = hu_json_object_get(first, "message");
        if (msg && msg->type == HU_JSON_OBJECT) {
            const char *content = hu_json_get_string(msg, "content");
            if (content) {
                size_t clen = strlen(content);
                out->content = hu_strndup(alloc, content, clen);
                out->content_len = clen;
            }
        }
    }
    hu_json_value_t *usage = hu_json_object_get(parsed, "usage");
    if (usage && usage->type == HU_JSON_OBJECT) {
        out->usage.prompt_tokens = (uint32_t)hu_json_get_number(usage, "prompt_tokens", 0);
        out->usage.completion_tokens = (uint32_t)hu_json_get_number(usage, "completion_tokens", 0);
        out->usage.total_tokens = (uint32_t)hu_json_get_number(usage, "total_tokens", 0);
    }
    const char *model_res = hu_json_get_string(parsed, "model");
    if (model_res) {
        out->model = hu_strndup(alloc, model_res, strlen(model_res));
        out->model_len = strlen(model_res);
    }
    hu_json_free(alloc, parsed);
    return HU_OK;
}

static bool wasm_supports_native_tools(void *ctx) { (void)ctx; return true; }
static const char *wasm_get_name(void *ctx) { (void)ctx; return "wasm_openai"; }

static void wasm_deinit(void *ctx, hu_allocator_t *alloc) {
    hu_wasm_provider_ctx_t *wc = (hu_wasm_provider_ctx_t *)ctx;
    if (!wc || !alloc) return;
    if (wc->api_key) alloc->free(alloc->ctx, wc->api_key, wc->api_key_len + 1);
    if (wc->base_url) alloc->free(alloc->ctx, wc->base_url, wc->base_url_len + 1);
    alloc->free(alloc->ctx, wc, sizeof(*wc));
}

static const hu_provider_vtable_t wasm_provider_vtable = {
    .chat_with_system = wasm_chat_with_system,
    .chat = wasm_chat,
    .supports_native_tools = wasm_supports_native_tools,
    .get_name = wasm_get_name,
    .deinit = wasm_deinit,
    .warmup = NULL, .chat_with_tools = NULL,
    .supports_streaming = NULL, .supports_vision = NULL,
    .supports_vision_for_model = NULL, .stream_chat = NULL,
};

hu_error_t hu_wasm_provider_create(hu_allocator_t *alloc,
    const char *api_key, size_t api_key_len,
    const char *base_url, size_t base_url_len,
    hu_provider_t *out)
{
    hu_wasm_provider_ctx_t *wc = (hu_wasm_provider_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*wc));
    if (!wc) return HU_ERR_OUT_OF_MEMORY;
    memset(wc, 0, sizeof(*wc));
    wc->alloc = *alloc;

    if (api_key && api_key_len > 0) {
        wc->api_key = (char *)alloc->alloc(alloc->ctx, api_key_len + 1);
        if (!wc->api_key) { alloc->free(alloc->ctx, wc, sizeof(*wc)); return HU_ERR_OUT_OF_MEMORY; }
        memcpy(wc->api_key, api_key, api_key_len);
        wc->api_key[api_key_len] = '\0';
        wc->api_key_len = api_key_len;
    }
    if (base_url && base_url_len > 0) {
        wc->base_url = (char *)alloc->alloc(alloc->ctx, base_url_len + 1);
        if (!wc->base_url) {
            if (wc->api_key) alloc->free(alloc->ctx, wc->api_key, wc->api_key_len + 1);
            alloc->free(alloc->ctx, wc, sizeof(*wc));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(wc->base_url, base_url, base_url_len);
        wc->base_url[base_url_len] = '\0';
        wc->base_url_len = base_url_len;
    }

    out->ctx = wc;
    out->vtable = &wasm_provider_vtable;
    return HU_OK;
}

#endif /* __wasi__ */
