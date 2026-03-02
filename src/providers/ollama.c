#include "seaclaw/provider.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/core/http.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define SC_OLLAMA_DEFAULT_URL "http://localhost:11434/api/chat"

typedef struct sc_ollama_ctx {
    char *base_url;
    size_t base_url_len;
} sc_ollama_ctx_t;

static sc_error_t ollama_chat(void *ctx, sc_allocator_t *alloc,
    const sc_chat_request_t *request,
    const char *model, size_t model_len,
    double temperature,
    sc_chat_response_t *out);

static sc_error_t ollama_chat_with_system(void *ctx, sc_allocator_t *alloc,
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
        .messages = msgs,
        .messages_count = 2,
        .model = model,
        .model_len = model_len,
        .temperature = temperature,
        .max_tokens = 0,
        .tools = NULL,
        .tools_count = 0,
        .timeout_secs = 0,
        .reasoning_effort = NULL,
        .reasoning_effort_len = 0,
    };

    sc_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));
    sc_error_t err = ollama_chat(ctx, alloc, &req, model, model_len, temperature, &resp);
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

static sc_error_t ollama_chat(void *ctx, sc_allocator_t *alloc,
    const sc_chat_request_t *request,
    const char *model, size_t model_len,
    double temperature,
    sc_chat_response_t *out)
{
    sc_ollama_ctx_t *oc = (sc_ollama_ctx_t *)ctx;
    if (!oc || !request || !out) return SC_ERR_INVALID_ARGUMENT;
    (void)model;
    (void)model_len;
    (void)temperature;

#if SC_IS_TEST
    memset(out, 0, sizeof(*out));
    if (request->tools && request->tools_count > 0) {
        out->content = NULL;
        out->content_len = 0;
        sc_tool_call_t *tcs = (sc_tool_call_t *)alloc->alloc(alloc->ctx, sizeof(sc_tool_call_t));
        if (tcs) {
            memset(tcs, 0, sizeof(sc_tool_call_t));
            tcs[0].id = sc_strndup(alloc, "call_mock_ollama", 16);
            tcs[0].id_len = 16;
            tcs[0].name = sc_strndup(alloc, "shell", 5);
            tcs[0].name_len = 5;
            tcs[0].arguments = sc_strndup(alloc, "{\"command\":\"ls\"}", 16);
            tcs[0].arguments_len = 16;
            out->tool_calls = tcs;
            out->tool_calls_count = 1;
        }
        out->usage.prompt_tokens = 10;
        out->usage.completion_tokens = 8;
        out->usage.total_tokens = 18;
    } else {
        const char *content = "Hello from mock Ollama";
        size_t len = strlen(content);
        out->content = sc_strndup(alloc, content, len);
        out->content_len = len;
        out->usage.prompt_tokens = 10;
        out->usage.completion_tokens = 5;
        out->usage.total_tokens = 15;
    }
    return SC_OK;
#else
    sc_json_value_t *root = sc_json_object_new(alloc);
    if (!root) return SC_ERR_OUT_OF_MEMORY;

    sc_json_object_set(alloc, root, "model", sc_json_string_new(alloc, model, model_len));
    sc_json_object_set(alloc, root, "stream", sc_json_bool_new(alloc, false));
    sc_json_object_set(alloc, root, "temperature", sc_json_number_new(alloc, temperature));

    sc_json_value_t *msgs_arr = sc_json_array_new(alloc);
    if (!msgs_arr) { sc_json_free(alloc, root); return SC_ERR_OUT_OF_MEMORY; }
    sc_json_object_set(alloc, root, "messages", msgs_arr);

    for (size_t i = 0; i < request->messages_count; i++) {
        const sc_chat_message_t *m = &request->messages[i];
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) { sc_json_free(alloc, root); return SC_ERR_OUT_OF_MEMORY; }

        const char *role_str = "user";
        if (m->role == SC_ROLE_SYSTEM) role_str = "system";
        else if (m->role == SC_ROLE_ASSISTANT) role_str = "assistant";
        else if (m->role == SC_ROLE_TOOL) role_str = "tool";
        sc_json_object_set(alloc, obj, "role", sc_json_string_new(alloc, role_str, strlen(role_str)));
        if (m->content && m->content_len > 0) {
            sc_json_object_set(alloc, obj, "content", sc_json_string_new(alloc, m->content, m->content_len));
        }
        if (m->role == SC_ROLE_TOOL && m->tool_call_id) {
            sc_json_object_set(alloc, obj, "tool_call_id", sc_json_string_new(alloc, m->tool_call_id, m->tool_call_id_len));
        }
        if (m->role == SC_ROLE_TOOL && m->name) {
            sc_json_object_set(alloc, obj, "name", sc_json_string_new(alloc, m->name, m->name_len));
        }
        if (m->role == SC_ROLE_ASSISTANT && m->tool_calls && m->tool_calls_count > 0) {
            sc_json_value_t *tc_arr = sc_json_array_new(alloc);
            if (tc_arr) {
                for (size_t k = 0; k < m->tool_calls_count; k++) {
                    const sc_tool_call_t *tc = &m->tool_calls[k];
                    sc_json_value_t *tc_obj = sc_json_object_new(alloc);
                    if (!tc_obj) { sc_json_free(alloc, tc_arr); sc_json_free(alloc, root); return SC_ERR_OUT_OF_MEMORY; }
                    if (tc->id && tc->id_len > 0)
                        sc_json_object_set(alloc, tc_obj, "id", sc_json_string_new(alloc, tc->id, tc->id_len));
                    sc_json_object_set(alloc, tc_obj, "type", sc_json_string_new(alloc, "function", 8));
                    sc_json_value_t *fn_obj = sc_json_object_new(alloc);
                    if (!fn_obj) { sc_json_free(alloc, tc_obj); sc_json_free(alloc, tc_arr); sc_json_free(alloc, root); return SC_ERR_OUT_OF_MEMORY; }
                    if (tc->name && tc->name_len > 0)
                        sc_json_object_set(alloc, fn_obj, "name", sc_json_string_new(alloc, tc->name, tc->name_len));
                    if (tc->arguments && tc->arguments_len > 0)
                        sc_json_object_set(alloc, fn_obj, "arguments", sc_json_string_new(alloc, tc->arguments, tc->arguments_len));
                    sc_json_object_set(alloc, tc_obj, "function", fn_obj);
                    sc_json_array_push(alloc, tc_arr, tc_obj);
                }
                sc_json_object_set(alloc, obj, "tool_calls", tc_arr);
            }
        }
        sc_json_array_push(alloc, msgs_arr, obj);
    }
    if (request->tools && request->tools_count > 0) {
        sc_json_value_t *tools_arr = sc_json_array_new(alloc);
        if (tools_arr) {
            for (size_t i = 0; i < request->tools_count; i++) {
                sc_json_value_t *tool_obj = sc_json_object_new(alloc);
                if (!tool_obj) { sc_json_free(alloc, tools_arr); sc_json_free(alloc, root); return SC_ERR_OUT_OF_MEMORY; }
                sc_json_object_set(alloc, tool_obj, "type", sc_json_string_new(alloc, "function", 8));
                sc_json_value_t *fn_obj = sc_json_object_new(alloc);
                if (!fn_obj) { sc_json_free(alloc, tool_obj); sc_json_free(alloc, tools_arr); sc_json_free(alloc, root); return SC_ERR_OUT_OF_MEMORY; }
                sc_json_object_set(alloc, fn_obj, "name", sc_json_string_new(alloc, request->tools[i].name, request->tools[i].name_len));
                sc_json_object_set(alloc, fn_obj, "description", sc_json_string_new(alloc,
                    request->tools[i].description ? request->tools[i].description : "", request->tools[i].description_len));
                if (request->tools[i].parameters_json && request->tools[i].parameters_json_len > 0) {
                    sc_json_value_t *params = NULL;
                    if (sc_json_parse(alloc, request->tools[i].parameters_json, request->tools[i].parameters_json_len, &params) == SC_OK)
                        sc_json_object_set(alloc, fn_obj, "parameters", params);
                }
                sc_json_object_set(alloc, tool_obj, "function", fn_obj);
                sc_json_array_push(alloc, tools_arr, tool_obj);
            }
            sc_json_object_set(alloc, root, "tools", tools_arr);
        }
    }

    char *body = NULL;
    size_t body_len = 0;
    sc_error_t err = sc_json_stringify(alloc, root, &body, &body_len);
    sc_json_free(alloc, root);
    if (err != SC_OK) return err;

    const char *base = oc->base_url && oc->base_url_len > 0 ? oc->base_url : SC_OLLAMA_DEFAULT_URL;
    size_t base_len = oc->base_url_len ? oc->base_url_len : (sizeof(SC_OLLAMA_DEFAULT_URL) - 1);
    char url_buf[256];
    int n = snprintf(url_buf, sizeof(url_buf), "%.*s", (int)base_len, base);
    if (n <= 0 || (size_t)n >= sizeof(url_buf)) {
        alloc->free(alloc->ctx, body, body_len);
        return SC_ERR_INVALID_ARGUMENT;
    }
    if (base_len > 0 && base[base_len - 1] != '/') {
        size_t need = strlen(url_buf);
        if (need + 10 < sizeof(url_buf)) {
            if (strstr(url_buf, "/api/chat") == NULL)
                snprintf(url_buf, sizeof(url_buf), "%.*s/api/chat", (int)base_len, base);
        }
    } else if (strstr(url_buf, "/api/chat") == NULL) {
        strncat(url_buf, "api/chat", sizeof(url_buf) - strlen(url_buf) - 1);
    }

    sc_http_response_t hresp = {0};
    err = sc_http_post_json(alloc, url_buf, NULL, body, body_len, &hresp);
    alloc->free(alloc->ctx, body, body_len);
    if (err != SC_OK) return err;

    if (hresp.status_code < 200 || hresp.status_code >= 300) {
        sc_http_response_free(alloc, &hresp);
        return SC_ERR_PROVIDER_RESPONSE;
    }

    sc_json_value_t *parsed = NULL;
    err = sc_json_parse(alloc, hresp.body, hresp.body_len, &parsed);
    sc_http_response_free(alloc, &hresp);
    if (err != SC_OK) return err;

    memset(out, 0, sizeof(*out));
    sc_json_value_t *msg = sc_json_object_get(parsed, "message");
    if (msg && msg->type == SC_JSON_OBJECT) {
        const char *content = sc_json_get_string(msg, "content");
        if (content) {
            size_t clen = strlen(content);
            out->content = sc_strndup(alloc, content, clen);
            out->content_len = clen;
        }
        sc_json_value_t *tc_arr = sc_json_object_get(msg, "tool_calls");
        if (tc_arr && tc_arr->type == SC_JSON_ARRAY && tc_arr->data.array.len > 0) {
            size_t tc_count = tc_arr->data.array.len;
            sc_tool_call_t *tcs = (sc_tool_call_t *)alloc->alloc(alloc->ctx, tc_count * sizeof(sc_tool_call_t));
            if (tcs) {
                memset(tcs, 0, tc_count * sizeof(sc_tool_call_t));
                size_t valid = 0;
                for (size_t j = 0; j < tc_count; j++) {
                    sc_json_value_t *tc = tc_arr->data.array.items[j];
                    const char *tc_id = sc_json_get_string(tc, "id");
                    sc_json_value_t *fn = sc_json_object_get(tc, "function");
                    if (!fn || fn->type != SC_JSON_OBJECT) continue;
                    const char *fn_name = sc_json_get_string(fn, "name");
                    const char *fn_args = sc_json_get_string(fn, "arguments");
                    if (!fn_name) continue;
                    tcs[valid].id = tc_id ? sc_strndup(alloc, tc_id, strlen(tc_id)) : NULL;
                    tcs[valid].id_len = tc_id ? (size_t)strlen(tc_id) : 0;
                    tcs[valid].name = sc_strndup(alloc, fn_name, strlen(fn_name));
                    tcs[valid].name_len = (size_t)strlen(fn_name);
                    tcs[valid].arguments = fn_args ? sc_strndup(alloc, fn_args, strlen(fn_args)) : NULL;
                    tcs[valid].arguments_len = fn_args ? (size_t)strlen(fn_args) : 0;
                    valid++;
                }
                out->tool_calls = tcs;
                out->tool_calls_count = valid;
            }
        }
    }
    sc_json_free(alloc, parsed);
    return SC_OK;
#endif
}

static bool ollama_supports_native_tools(void *ctx) { (void)ctx; return false; }
static const char *ollama_get_name(void *ctx) { (void)ctx; return "ollama"; }
static void ollama_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    sc_ollama_ctx_t *oc = (sc_ollama_ctx_t *)ctx;
    if (oc && oc->base_url) { free(oc->base_url); free(oc); }
}

static const sc_provider_vtable_t ollama_vtable = {
    .chat_with_system = ollama_chat_with_system,
    .chat = ollama_chat,
    .supports_native_tools = ollama_supports_native_tools,
    .get_name = ollama_get_name,
    .deinit = ollama_deinit,
    .warmup = NULL, .chat_with_tools = NULL,
    .supports_streaming = NULL, .supports_vision = NULL,
    .supports_vision_for_model = NULL, .stream_chat = NULL,
};

sc_error_t sc_ollama_create(sc_allocator_t *alloc,
    const char *api_key, size_t api_key_len,
    const char *base_url, size_t base_url_len,
    sc_provider_t *out)
{
    (void)alloc;
    (void)api_key;
    (void)api_key_len;
    sc_ollama_ctx_t *oc = (sc_ollama_ctx_t *)calloc(1, sizeof(*oc));
    if (!oc) return SC_ERR_OUT_OF_MEMORY;
    if (base_url && base_url_len > 0) {
        oc->base_url = (char *)malloc(base_url_len + 1);
        if (!oc->base_url) { free(oc); return SC_ERR_OUT_OF_MEMORY; }
        memcpy(oc->base_url, base_url, base_url_len);
        oc->base_url[base_url_len] = '\0';
        oc->base_url_len = base_url_len;
    } else {
        oc->base_url = (char *)malloc(sizeof(SC_OLLAMA_DEFAULT_URL));
        if (!oc->base_url) { free(oc); return SC_ERR_OUT_OF_MEMORY; }
        memcpy(oc->base_url, SC_OLLAMA_DEFAULT_URL, sizeof(SC_OLLAMA_DEFAULT_URL));
        oc->base_url_len = sizeof(SC_OLLAMA_DEFAULT_URL) - 1;
    }
    out->ctx = oc;
    out->vtable = &ollama_vtable;
    return SC_OK;
}
