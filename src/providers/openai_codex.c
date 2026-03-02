/* OpenAI Codex provider - ChatGPT Codex API.
 * Uses OAuth in production; in test mode returns mock. */

#include "seaclaw/providers/openai_codex.h"
#include "seaclaw/provider.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/core/http.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define SC_OPENAI_CODEX_URL_LEN (sizeof(SC_OPENAI_CODEX_URL) - 1)

/* Strip openai-codex/ prefix from model name */
static void normalize_model(const char *model, size_t model_len, char *out, size_t out_cap, size_t *out_len) {
    size_t skip = 0;
    if (model_len >= sizeof(SC_OPENAI_CODEX_PREFIX) - 1 &&
        memcmp(model, SC_OPENAI_CODEX_PREFIX, sizeof(SC_OPENAI_CODEX_PREFIX) - 1) == 0) {
        skip = sizeof(SC_OPENAI_CODEX_PREFIX) - 1;
    }
    size_t n = model_len - skip;
    if (n >= out_cap) n = out_cap - 1;
    memcpy(out, model + skip, n);
    out[n] = '\0';
    *out_len = n;
}

typedef struct sc_openai_codex_ctx {
    char *api_key;
    size_t api_key_len;
    char *base_url;
    size_t base_url_len;
} sc_openai_codex_ctx_t;

static sc_error_t codex_http_post(sc_allocator_t *alloc,
    const char *url, size_t url_len,
    const char *auth, size_t auth_len,
    const char *body, size_t body_len,
    char **resp_out, size_t *resp_len_out)
{
    *resp_out = NULL;
    *resp_len_out = 0;

#if SC_IS_TEST
    (void)alloc;
    (void)url;
    (void)url_len;
    (void)auth;
    (void)auth_len;
    (void)body;
    (void)body_len;
    const char *mock = "{\"response\":{\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"Hello from mock Codex\"}]}]}}";
    size_t mock_len = strlen(mock);
    char *buf = (char *)alloc->alloc(alloc->ctx, mock_len + 1);
    if (!buf) return SC_ERR_OUT_OF_MEMORY;
    memcpy(buf, mock, mock_len + 1);
    *resp_out = buf;
    *resp_len_out = mock_len;
    return SC_OK;
#else
    char url_buf[512];
    if (url_len >= sizeof(url_buf)) return SC_ERR_INVALID_ARGUMENT;
    memcpy(url_buf, url, url_len);
    url_buf[url_len] = '\0';

    const char *auth_str = NULL;
    char auth_buf[1024];
    if (auth_len > 0 && auth_len < sizeof(auth_buf)) {
        memcpy(auth_buf, auth, auth_len);
        auth_buf[auth_len] = '\0';
        auth_str = auth_buf;
    }

    sc_http_response_t hresp = {0};
    sc_error_t err = sc_http_post_json(alloc, url_buf, auth_str, body, body_len, &hresp);
    if (err != SC_OK) return err;
    if (hresp.status_code < 200 || hresp.status_code >= 300) {
        sc_http_response_free(alloc, &hresp);
        if (hresp.status_code == 401) return SC_ERR_PROVIDER_AUTH;
        if (hresp.status_code == 429) return SC_ERR_PROVIDER_RATE_LIMITED;
        return SC_ERR_PROVIDER_RESPONSE;
    }
    *resp_out = hresp.body;
    *resp_len_out = hresp.body_len;
    hresp.owned = false;
    return SC_OK;
#endif
}

static sc_error_t openai_codex_chat(void *ctx, sc_allocator_t *alloc,
    const sc_chat_request_t *request,
    const char *model, size_t model_len,
    double temperature,
    sc_chat_response_t *out);

static sc_error_t openai_codex_chat_with_system(void *ctx, sc_allocator_t *alloc,
    const char *system_prompt, size_t system_prompt_len,
    const char *message, size_t message_len,
    const char *model, size_t model_len,
    double temperature,
    char **out, size_t *out_len)
{
    sc_chat_message_t msgs[2];
    msgs[0].role = SC_ROLE_SYSTEM;
    msgs[0].content = system_prompt;
    msgs[0].content_len = system_prompt_len;
    msgs[0].name = NULL;
    msgs[0].name_len = 0;
    msgs[0].tool_call_id = NULL;
    msgs[0].tool_call_id_len = 0;
    msgs[0].content_parts = NULL;
    msgs[0].content_parts_count = 0;

    msgs[1].role = SC_ROLE_USER;
    msgs[1].content = message;
    msgs[1].content_len = message_len;
    msgs[1].name = NULL;
    msgs[1].name_len = 0;
    msgs[1].tool_call_id = NULL;
    msgs[1].tool_call_id_len = 0;
    msgs[1].content_parts = NULL;
    msgs[1].content_parts_count = 0;

    sc_chat_request_t req = {
        .messages = msgs, .messages_count = 2,
        .model = model, .model_len = model_len, .temperature = temperature,
        .max_tokens = 0, .tools = NULL, .tools_count = 0,
        .timeout_secs = 0, .reasoning_effort = NULL, .reasoning_effort_len = 0,
    };

    sc_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));
    sc_error_t err = openai_codex_chat(ctx, alloc, &req, model, model_len, temperature, &resp);
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

static sc_error_t openai_codex_chat(void *ctx, sc_allocator_t *alloc,
    const sc_chat_request_t *request,
    const char *model, size_t model_len,
    double temperature,
    sc_chat_response_t *out)
{
    sc_openai_codex_ctx_t *oc = (sc_openai_codex_ctx_t *)ctx;
    if (!oc || !request || !out) return SC_ERR_INVALID_ARGUMENT;
    (void)temperature;
#if !SC_IS_TEST
    if (!oc->api_key || oc->api_key_len == 0) return SC_ERR_PROVIDER_AUTH;
#endif

    char model_norm[128];
    size_t model_norm_len;
    normalize_model(model, model_len, model_norm, sizeof(model_norm), &model_norm_len);

    /* Build Codex-style body: instructions, input array */
    sc_json_value_t *root = sc_json_object_new(alloc);
    if (!root) return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *model_val = sc_json_string_new(alloc, model_norm, model_norm_len);
    sc_json_object_set(alloc, root, "model", model_val);

    const char *instructions = "You are a helpful assistant.";
    size_t inst_len = strlen(instructions);
    for (size_t i = 0; i < request->messages_count; i++) {
        if (request->messages[i].role == SC_ROLE_SYSTEM && request->messages[i].content) {
            instructions = request->messages[i].content;
            inst_len = request->messages[i].content_len;
            break;
        }
    }
    sc_json_value_t *inst_val = sc_json_string_new(alloc, instructions, inst_len);
    sc_json_object_set(alloc, root, "instructions", inst_val);

    sc_json_value_t *input_arr = sc_json_array_new(alloc);
    if (!input_arr) { sc_json_free(alloc, root); return SC_ERR_OUT_OF_MEMORY; }
    for (size_t i = 0; i < request->messages_count; i++) {
        const sc_chat_message_t *m = &request->messages[i];
        if (m->role == SC_ROLE_SYSTEM) continue;
        sc_json_value_t *item = sc_json_object_new(alloc);
        if (!item) { sc_json_free(alloc, root); return SC_ERR_OUT_OF_MEMORY; }
        sc_json_value_t *type_val = sc_json_string_new(alloc, "message", 7);
        sc_json_object_set(alloc, item, "type", type_val);
        const char *role_str = m->role == SC_ROLE_USER ? "user" : "assistant";
        sc_json_value_t *role_val = sc_json_string_new(alloc, role_str, strlen(role_str));
        sc_json_object_set(alloc, item, "role", role_val);
        if (m->content) {
            sc_json_value_t *content_val = sc_json_string_new(alloc, m->content, m->content_len);
            sc_json_object_set(alloc, item, "content", content_val);
        }
        sc_json_array_push(alloc, input_arr, item);
    }
    sc_json_object_set(alloc, root, "input", input_arr);

    sc_json_value_t *stream_val = sc_json_bool_new(alloc, false); /* non-stream for simple impl */
    sc_json_object_set(alloc, root, "stream", stream_val);

    char *body = NULL;
    size_t body_len = 0;
    sc_error_t err = sc_json_stringify(alloc, root, &body, &body_len);
    sc_json_free(alloc, root);
    if (err != SC_OK) return err;

    char auth_buf[1024];
    size_t auth_len = 0;
    if (oc->api_key && oc->api_key_len > 0) {
        int n = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)oc->api_key_len, oc->api_key);
        if (n > 0 && (size_t)n < sizeof(auth_buf)) auth_len = (size_t)n;
    }

    const char *url = oc->base_url && oc->base_url_len > 0 ? oc->base_url : SC_OPENAI_CODEX_URL;
    size_t url_len = oc->base_url_len > 0 ? oc->base_url_len : SC_OPENAI_CODEX_URL_LEN;

    char *resp_body = NULL;
    size_t resp_len = 0;
    err = codex_http_post(alloc, url, url_len, auth_buf, auth_len, body, body_len, &resp_body, &resp_len);
    alloc->free(alloc->ctx, body, body_len);
    if (err != SC_OK) return err;

    sc_json_value_t *parsed = NULL;
    err = sc_json_parse(alloc, resp_body, resp_len, &parsed);
    alloc->free(alloc->ctx, resp_body, resp_len);
    if (err != SC_OK) return err;

    memset(out, 0, sizeof(*out));
    sc_json_value_t *response = sc_json_object_get(parsed, "response");
    if (response && response->type == SC_JSON_OBJECT) {
        sc_json_value_t *output = sc_json_object_get(response, "output");
        if (output && output->type == SC_JSON_ARRAY && output->data.array.len > 0) {
            sc_json_value_t *first = output->data.array.items[0];
            sc_json_value_t *content = sc_json_object_get(first, "content");
            if (content && content->type == SC_JSON_ARRAY && content->data.array.len > 0) {
                sc_json_value_t *part = content->data.array.items[0];
                const char *text = sc_json_get_string(part, "text");
                if (text) {
                    size_t tlen = strlen(text);
                    out->content = sc_strndup(alloc, text, tlen);
                    out->content_len = tlen;
                }
            }
        }
    }
    out->model = sc_strndup(alloc, model_norm, model_norm_len);
    out->model_len = model_norm_len;
    sc_json_free(alloc, parsed);
    return SC_OK;
}

static bool openai_codex_supports_native_tools(void *ctx) { (void)ctx; return false; }
static const char *openai_codex_get_name(void *ctx) { (void)ctx; return "openai-codex"; }
static void openai_codex_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    sc_openai_codex_ctx_t *oc = (sc_openai_codex_ctx_t *)ctx;
    if (!oc) return;
    if (oc->api_key) free(oc->api_key);
    if (oc->base_url) free(oc->base_url);
    free(oc);
}

static const sc_provider_vtable_t openai_codex_vtable = {
    .chat_with_system = openai_codex_chat_with_system,
    .chat = openai_codex_chat,
    .supports_native_tools = openai_codex_supports_native_tools,
    .get_name = openai_codex_get_name,
    .deinit = openai_codex_deinit,
    .warmup = NULL, .chat_with_tools = NULL,
    .supports_streaming = NULL, .supports_vision = NULL,
    .supports_vision_for_model = NULL, .stream_chat = NULL,
};

sc_error_t sc_openai_codex_create(sc_allocator_t *alloc,
    const char *api_key, size_t api_key_len,
    const char *base_url, size_t base_url_len,
    sc_provider_t *out)
{
    (void)alloc;
    sc_openai_codex_ctx_t *oc = (sc_openai_codex_ctx_t *)calloc(1, sizeof(*oc));
    if (!oc) return SC_ERR_OUT_OF_MEMORY;
    if (api_key && api_key_len > 0) {
        oc->api_key = (char *)malloc(api_key_len + 1);
        if (!oc->api_key) { free(oc); return SC_ERR_OUT_OF_MEMORY; }
        memcpy(oc->api_key, api_key, api_key_len);
        oc->api_key[api_key_len] = '\0';
        oc->api_key_len = api_key_len;
    }
    if (base_url && base_url_len > 0) {
        oc->base_url = (char *)malloc(base_url_len + 1);
        if (!oc->base_url) {
            if (oc->api_key) free(oc->api_key);
            free(oc);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(oc->base_url, base_url, base_url_len);
        oc->base_url[base_url_len] = '\0';
        oc->base_url_len = base_url_len;
    }
    out->ctx = oc;
    out->vtable = &openai_codex_vtable;
    return SC_OK;
}
