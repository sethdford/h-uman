/* WASM-compatible OpenAI-style provider using sc_host_http_fetch. */
#ifdef __wasi__

#include "seaclaw/provider.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/wasm/host_imports.h"
#include <string.h>
#include <stdlib.h>

#define SC_OPENAI_URL "https://api.openai.com/v1/chat/completions"
#define SC_OPENAI_URL_LEN (sizeof(SC_OPENAI_URL) - 1)
#define SC_HTTP_RESPONSE_MAX 262144  /* 256K */

typedef struct sc_wasm_provider_ctx {
    sc_allocator_t alloc;
    char *api_key;
    size_t api_key_len;
    char *base_url;
    size_t base_url_len;
} sc_wasm_provider_ctx_t;

static sc_error_t wasm_http_post(sc_allocator_t *alloc,
    const char *url, size_t url_len,
    const char *auth_header, size_t auth_len,
    const char *body, size_t body_len,
    char **response_out, size_t *response_len_out)
{
    (void)auth_header;
    (void)auth_len;
    char *resp_buf = (char *)alloc->alloc(alloc->ctx, SC_HTTP_RESPONSE_MAX);
    if (!resp_buf) return SC_ERR_OUT_OF_MEMORY;

    const char *method = "POST";
    int n = sc_host_http_fetch(
        url, (int)url_len,
        method, 4,
        body, (int)body_len,
        resp_buf, SC_HTTP_RESPONSE_MAX);
    if (n < 0) {
        alloc->free(alloc->ctx, resp_buf, SC_HTTP_RESPONSE_MAX);
        return SC_ERR_PROVIDER_RESPONSE;
    }
    *response_out = resp_buf;
    *response_len_out = (size_t)n;
    return SC_OK;
}

static sc_error_t wasm_chat(void *ctx, sc_allocator_t *alloc,
    const sc_chat_request_t *request,
    const char *model, size_t model_len,
    double temperature,
    sc_chat_response_t *out);

static sc_error_t wasm_chat_with_system(void *ctx, sc_allocator_t *alloc,
    const char *system_prompt, size_t system_prompt_len,
    const char *message, size_t message_len,
    const char *model, size_t model_len,
    double temperature,
    char **out, size_t *out_len)
{
    sc_chat_message_t msgs[2] = {
        { .role = SC_ROLE_SYSTEM, .content = system_prompt, .content_len = system_prompt_len,
          .name = NULL, .name_len = 0, .tool_call_id = NULL, .tool_call_id_len = 0,
          .content_parts = NULL, .content_parts_count = 0 },
        { .role = SC_ROLE_USER, .content = message, .content_len = message_len,
          .name = NULL, .name_len = 0, .tool_call_id = NULL, .tool_call_id_len = 0,
          .content_parts = NULL, .content_parts_count = 0 },
    };
    sc_chat_request_t req = {
        .messages = msgs, .messages_count = 2,
        .model = model, .model_len = model_len,
        .temperature = temperature, .max_tokens = 0,
        .tools = NULL, .tools_count = 0, .timeout_secs = 0,
        .reasoning_effort = NULL, .reasoning_effort_len = 0,
    };
    sc_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));
    sc_error_t err = wasm_chat(ctx, alloc, &req, model, model_len, temperature, &resp);
    if (err != SC_OK) return err;
    if (resp.content && resp.content_len > 0) {
        *out = sc_strndup(alloc, resp.content, resp.content_len);
        if (!*out) return SC_ERR_OUT_OF_MEMORY;
        *out_len = resp.content_len;
    } else {
        *out = NULL;
        *out_len = 0;
    }
    return SC_OK;
}

static sc_error_t wasm_chat(void *ctx, sc_allocator_t *alloc,
    const sc_chat_request_t *request,
    const char *model, size_t model_len,
    double temperature,
    sc_chat_response_t *out)
{
    sc_wasm_provider_ctx_t *wc = (sc_wasm_provider_ctx_t *)ctx;
    if (!wc || !request || !out) return SC_ERR_INVALID_ARGUMENT;

    sc_json_value_t *root = sc_json_object_new(alloc);
    if (!root) return SC_ERR_OUT_OF_MEMORY;
    sc_json_value_t *msgs_arr = sc_json_array_new(alloc);
    if (!msgs_arr) { sc_json_free(alloc, root); return SC_ERR_OUT_OF_MEMORY; }
    (void)sc_json_object_set(alloc, root, "messages", msgs_arr);

    for (size_t i = 0; i < request->messages_count; i++) {
        const sc_chat_message_t *m = &request->messages[i];
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) { sc_json_free(alloc, root); return SC_ERR_OUT_OF_MEMORY; }
        const char *role_str = "user";
        if (m->role == SC_ROLE_SYSTEM) role_str = "system";
        else if (m->role == SC_ROLE_ASSISTANT) role_str = "assistant";
        else if (m->role == SC_ROLE_TOOL) role_str = "tool";
        sc_json_value_t *role_val = sc_json_string_new(alloc, role_str, strlen(role_str));
        sc_json_object_set(alloc, obj, "role", role_val);
        if (m->content && m->content_len > 0) {
            sc_json_value_t *cval = sc_json_string_new(alloc, m->content, m->content_len);
            sc_json_object_set(alloc, obj, "content", cval);
        }
        if (m->role == SC_ROLE_TOOL && m->tool_call_id) {
            sc_json_value_t *id_val = sc_json_string_new(alloc, m->tool_call_id, m->tool_call_id_len);
            sc_json_object_set(alloc, obj, "tool_call_id", id_val);
        }
        if (m->role == SC_ROLE_TOOL && m->name) {
            sc_json_value_t *nval = sc_json_string_new(alloc, m->name, m->name_len);
            sc_json_object_set(alloc, obj, "name", nval);
        }
        sc_json_array_push(alloc, msgs_arr, obj);
    }

    sc_json_value_t *model_val = sc_json_string_new(alloc, model, model_len);
    sc_json_object_set(alloc, root, "model", model_val);
    sc_json_value_t *temp_val = sc_json_number_new(alloc, temperature);
    sc_json_object_set(alloc, root, "temperature", temp_val);

    char *body = NULL;
    size_t body_len = 0;
    sc_error_t err = sc_json_stringify(alloc, root, &body, &body_len);
    sc_json_free(alloc, root);
    if (err != SC_OK) return err;

    const char *url = wc->base_url ? wc->base_url : SC_OPENAI_URL;
    size_t url_len = wc->base_url_len ? wc->base_url_len : SC_OPENAI_URL_LEN;

    char *resp_body = NULL;
    size_t resp_len = 0;
    err = wasm_http_post(alloc, url, url_len, NULL, 0, body, body_len, &resp_body, &resp_len);
    alloc->free(alloc->ctx, body, body_len);
    if (err != SC_OK) return err;

    sc_json_value_t *parsed = NULL;
    err = sc_json_parse(alloc, resp_body, resp_len, &parsed);
    alloc->free(alloc->ctx, resp_body, resp_len);
    if (err != SC_OK) return err;

    memset(out, 0, sizeof(*out));
    sc_json_value_t *choices = sc_json_object_get(parsed, "choices");
    if (choices && choices->type == SC_JSON_ARRAY && choices->data.array.len > 0) {
        sc_json_value_t *first = choices->data.array.items[0];
        sc_json_value_t *msg = sc_json_object_get(first, "message");
        if (msg && msg->type == SC_JSON_OBJECT) {
            const char *content = sc_json_get_string(msg, "content");
            if (content) {
                size_t clen = strlen(content);
                out->content = sc_strndup(alloc, content, clen);
                out->content_len = clen;
            }
        }
    }
    sc_json_value_t *usage = sc_json_object_get(parsed, "usage");
    if (usage && usage->type == SC_JSON_OBJECT) {
        out->usage.prompt_tokens = (uint32_t)sc_json_get_number(usage, "prompt_tokens", 0);
        out->usage.completion_tokens = (uint32_t)sc_json_get_number(usage, "completion_tokens", 0);
        out->usage.total_tokens = (uint32_t)sc_json_get_number(usage, "total_tokens", 0);
    }
    const char *model_res = sc_json_get_string(parsed, "model");
    if (model_res) {
        out->model = sc_strndup(alloc, model_res, strlen(model_res));
        out->model_len = strlen(model_res);
    }
    sc_json_free(alloc, parsed);
    return SC_OK;
}

static bool wasm_supports_native_tools(void *ctx) { (void)ctx; return true; }
static const char *wasm_get_name(void *ctx) { (void)ctx; return "wasm_openai"; }

static void wasm_deinit(void *ctx, sc_allocator_t *alloc) {
    sc_wasm_provider_ctx_t *wc = (sc_wasm_provider_ctx_t *)ctx;
    if (!wc || !alloc) return;
    if (wc->api_key) alloc->free(alloc->ctx, wc->api_key, wc->api_key_len + 1);
    if (wc->base_url) alloc->free(alloc->ctx, wc->base_url, wc->base_url_len + 1);
    alloc->free(alloc->ctx, wc, sizeof(*wc));
}

static const sc_provider_vtable_t wasm_provider_vtable = {
    .chat_with_system = wasm_chat_with_system,
    .chat = wasm_chat,
    .supports_native_tools = wasm_supports_native_tools,
    .get_name = wasm_get_name,
    .deinit = wasm_deinit,
    .warmup = NULL, .chat_with_tools = NULL,
    .supports_streaming = NULL, .supports_vision = NULL,
    .supports_vision_for_model = NULL, .stream_chat = NULL,
};

sc_error_t sc_wasm_provider_create(sc_allocator_t *alloc,
    const char *api_key, size_t api_key_len,
    const char *base_url, size_t base_url_len,
    sc_provider_t *out)
{
    sc_wasm_provider_ctx_t *wc = (sc_wasm_provider_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*wc));
    if (!wc) return SC_ERR_OUT_OF_MEMORY;
    memset(wc, 0, sizeof(*wc));
    wc->alloc = *alloc;

    if (api_key && api_key_len > 0) {
        wc->api_key = (char *)alloc->alloc(alloc->ctx, api_key_len + 1);
        if (!wc->api_key) { alloc->free(alloc->ctx, wc, sizeof(*wc)); return SC_ERR_OUT_OF_MEMORY; }
        memcpy(wc->api_key, api_key, api_key_len);
        wc->api_key[api_key_len] = '\0';
        wc->api_key_len = api_key_len;
    }
    if (base_url && base_url_len > 0) {
        wc->base_url = (char *)alloc->alloc(alloc->ctx, base_url_len + 1);
        if (!wc->base_url) {
            if (wc->api_key) alloc->free(alloc->ctx, wc->api_key, wc->api_key_len + 1);
            alloc->free(alloc->ctx, wc, sizeof(*wc));
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(wc->base_url, base_url, base_url_len);
        wc->base_url[base_url_len] = '\0';
        wc->base_url_len = base_url_len;
    }

    out->ctx = wc;
    out->vtable = &wasm_provider_vtable;
    return SC_OK;
}

#endif /* __wasi__ */
