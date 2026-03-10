#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/provider.h"
#include "human/providers/provider_http.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct hu_compatible_ctx {
    char *api_key;
    size_t api_key_len;
    char *base_url;
    size_t base_url_len;
} hu_compatible_ctx_t;

static hu_error_t compatible_chat(void *ctx, hu_allocator_t *alloc,
                                  const hu_chat_request_t *request, const char *model,
                                  size_t model_len, double temperature, hu_chat_response_t *out);

static hu_error_t compatible_chat_with_system(void *ctx, hu_allocator_t *alloc,
                                              const char *system_prompt, size_t system_prompt_len,
                                              const char *message, size_t message_len,
                                              const char *model, size_t model_len,
                                              double temperature, char **out, size_t *out_len) {
    hu_chat_message_t msgs[2];
    msgs[0].role = HU_ROLE_SYSTEM;
    msgs[0].content = system_prompt;
    msgs[0].content_len = system_prompt_len;
    msgs[0].name = NULL;
    msgs[0].name_len = 0;
    msgs[0].tool_call_id = NULL;
    msgs[0].tool_call_id_len = 0;
    msgs[0].content_parts = NULL;
    msgs[0].content_parts_count = 0;

    msgs[1].role = HU_ROLE_USER;
    msgs[1].content = message;
    msgs[1].content_len = message_len;
    msgs[1].name = NULL;
    msgs[1].name_len = 0;
    msgs[1].tool_call_id = NULL;
    msgs[1].tool_call_id_len = 0;
    msgs[1].content_parts = NULL;
    msgs[1].content_parts_count = 0;

    hu_chat_request_t req = {
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

    hu_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));
    hu_error_t err = compatible_chat(ctx, alloc, &req, model, model_len, temperature, &resp);
    if (err != HU_OK)
        return err;

    if (resp.content && resp.content_len > 0) {
        *out = hu_strndup(alloc, resp.content, resp.content_len);
        if (!*out) {
            hu_chat_response_free(alloc, &resp);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out_len = resp.content_len;
    } else {
        *out = NULL;
        *out_len = 0;
    }
    hu_chat_response_free(alloc, &resp);
    return HU_OK;
}

static hu_error_t compatible_chat(void *ctx, hu_allocator_t *alloc,
                                  const hu_chat_request_t *request, const char *model,
                                  size_t model_len, double temperature, hu_chat_response_t *out) {
    hu_compatible_ctx_t *cc = (hu_compatible_ctx_t *)ctx;
    if (!cc || !request || !out)
        return HU_ERR_INVALID_ARGUMENT;
    (void)model;
    (void)model_len;
    (void)temperature;

#if HU_IS_TEST
    memset(out, 0, sizeof(*out));
    if (request->tools && request->tools_count > 0) {
        out->content = NULL;
        out->content_len = 0;
        hu_tool_call_t *tcs = (hu_tool_call_t *)alloc->alloc(alloc->ctx, sizeof(hu_tool_call_t));
        if (tcs) {
            memset(tcs, 0, sizeof(hu_tool_call_t));
            tcs[0].id = hu_strndup(alloc, "call_mock_comp", 14);
            tcs[0].id_len = 14;
            tcs[0].name = hu_strndup(alloc, "shell", 5);
            tcs[0].name_len = 5;
            tcs[0].arguments = hu_strndup(alloc, "{\"command\":\"ls\"}", 16);
            tcs[0].arguments_len = 16;
            out->tool_calls = tcs;
            out->tool_calls_count = 1;
        }
        out->usage.prompt_tokens = 10;
        out->usage.completion_tokens = 8;
        out->usage.total_tokens = 18;
    } else {
        const char *content = "Hello from mock OpenAI-compatible";
        size_t len = strlen(content);
        out->content = hu_strndup(alloc, content, len);
        out->content_len = len;
        out->usage.prompt_tokens = 10;
        out->usage.completion_tokens = 5;
        out->usage.total_tokens = 15;
    }
    return HU_OK;
#else
    if (!cc->base_url || cc->base_url_len == 0)
        return HU_ERR_CONFIG_INVALID;

    hu_json_value_t *root = hu_json_object_new(alloc);
    if (!root)
        return HU_ERR_OUT_OF_MEMORY;

    hu_json_value_t *msgs_arr = hu_json_array_new(alloc);
    if (!msgs_arr) {
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    hu_json_object_set(alloc, root, "messages", msgs_arr);

    for (size_t i = 0; i < request->messages_count; i++) {
        const hu_chat_message_t *m = &request->messages[i];
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj) {
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }

        const char *role_str = "user";
        if (m->role == HU_ROLE_SYSTEM)
            role_str = "system";
        else if (m->role == HU_ROLE_ASSISTANT)
            role_str = "assistant";
        else if (m->role == HU_ROLE_TOOL)
            role_str = "tool";
        hu_json_object_set(alloc, obj, "role",
                           hu_json_string_new(alloc, role_str, strlen(role_str)));
        if (m->content && m->content_len > 0) {
            hu_json_object_set(alloc, obj, "content",
                               hu_json_string_new(alloc, m->content, m->content_len));
        }
        if (m->role == HU_ROLE_TOOL && m->tool_call_id) {
            hu_json_object_set(alloc, obj, "tool_call_id",
                               hu_json_string_new(alloc, m->tool_call_id, m->tool_call_id_len));
        }
        if (m->role == HU_ROLE_TOOL && m->name) {
            hu_json_object_set(alloc, obj, "name", hu_json_string_new(alloc, m->name, m->name_len));
        }
        if (m->role == HU_ROLE_ASSISTANT && m->tool_calls && m->tool_calls_count > 0) {
            hu_json_value_t *tc_arr = hu_json_array_new(alloc);
            if (tc_arr) {
                for (size_t k = 0; k < m->tool_calls_count; k++) {
                    const hu_tool_call_t *tc = &m->tool_calls[k];
                    hu_json_value_t *tc_obj = hu_json_object_new(alloc);
                    if (!tc_obj) {
                        hu_json_free(alloc, tc_arr);
                        hu_json_free(alloc, root);
                        return HU_ERR_OUT_OF_MEMORY;
                    }
                    if (tc->id && tc->id_len > 0)
                        hu_json_object_set(alloc, tc_obj, "id",
                                           hu_json_string_new(alloc, tc->id, tc->id_len));
                    hu_json_object_set(alloc, tc_obj, "type",
                                       hu_json_string_new(alloc, "function", 8));
                    hu_json_value_t *fn_obj = hu_json_object_new(alloc);
                    if (!fn_obj) {
                        hu_json_free(alloc, tc_obj);
                        hu_json_free(alloc, tc_arr);
                        hu_json_free(alloc, root);
                        return HU_ERR_OUT_OF_MEMORY;
                    }
                    if (tc->name && tc->name_len > 0)
                        hu_json_object_set(alloc, fn_obj, "name",
                                           hu_json_string_new(alloc, tc->name, tc->name_len));
                    if (tc->arguments && tc->arguments_len > 0)
                        hu_json_object_set(
                            alloc, fn_obj, "arguments",
                            hu_json_string_new(alloc, tc->arguments, tc->arguments_len));
                    hu_json_object_set(alloc, tc_obj, "function", fn_obj);
                    hu_json_array_push(alloc, tc_arr, tc_obj);
                }
                hu_json_object_set(alloc, obj, "tool_calls", tc_arr);
            }
        }
        hu_json_array_push(alloc, msgs_arr, obj);
    }
    if (request->tools && request->tools_count > 0) {
        hu_json_value_t *tools_arr = hu_json_array_new(alloc);
        if (tools_arr) {
            for (size_t i = 0; i < request->tools_count; i++) {
                hu_json_value_t *tool_obj = hu_json_object_new(alloc);
                if (!tool_obj) {
                    hu_json_free(alloc, tools_arr);
                    hu_json_free(alloc, root);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                hu_json_object_set(alloc, tool_obj, "type",
                                   hu_json_string_new(alloc, "function", 8));
                hu_json_value_t *fn_obj = hu_json_object_new(alloc);
                if (!fn_obj) {
                    hu_json_free(alloc, tool_obj);
                    hu_json_free(alloc, tools_arr);
                    hu_json_free(alloc, root);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                hu_json_object_set(
                    alloc, fn_obj, "name",
                    hu_json_string_new(alloc, request->tools[i].name, request->tools[i].name_len));
                hu_json_object_set(
                    alloc, fn_obj, "description",
                    hu_json_string_new(
                        alloc, request->tools[i].description ? request->tools[i].description : "",
                        request->tools[i].description_len));
                if (request->tools[i].parameters_json &&
                    request->tools[i].parameters_json_len > 0) {
                    hu_json_value_t *params = NULL;
                    if (hu_json_parse(alloc, request->tools[i].parameters_json,
                                      request->tools[i].parameters_json_len, &params) == HU_OK)
                        hu_json_object_set(alloc, fn_obj, "parameters", params);
                }
                hu_json_object_set(alloc, tool_obj, "function", fn_obj);
                hu_json_array_push(alloc, tools_arr, tool_obj);
            }
            hu_json_object_set(alloc, root, "tools", tools_arr);
        }
    }

    hu_json_object_set(alloc, root, "model", hu_json_string_new(alloc, model, model_len));
    hu_json_object_set(alloc, root, "temperature", hu_json_number_new(alloc, temperature));
    if (request->max_tokens > 0) {
        hu_json_object_set(alloc, root, "max_tokens",
                           hu_json_number_new(alloc, (double)request->max_tokens));
    }

    if (request->response_format && request->response_format_len > 0) {
        hu_json_value_t *rf_obj = hu_json_object_new(alloc);
        if (rf_obj) {
            hu_json_value_t *rf_type =
                hu_json_string_new(alloc, request->response_format, request->response_format_len);
            hu_json_object_set(alloc, rf_obj, "type", rf_type);
            hu_json_object_set(alloc, root, "response_format", rf_obj);
        }
    }

    char *body = NULL;
    size_t body_len = 0;
    hu_error_t err = hu_json_stringify(alloc, root, &body, &body_len);
    hu_json_free(alloc, root);
    if (err != HU_OK)
        return err;

    size_t base_len = cc->base_url_len;
    char url_buf[512];
    int n;
    if (base_len >= 17 && strncmp(cc->base_url + base_len - 17, "/chat/completions", 17) == 0) {
        n = snprintf(url_buf, sizeof(url_buf), "%.*s", (int)base_len, cc->base_url);
    } else {
        char *sep = (base_len > 0 && cc->base_url[base_len - 1] == '/') ? "" : "/";
        n = snprintf(url_buf, sizeof(url_buf), "%.*s%schat/completions", (int)base_len,
                     cc->base_url, sep);
    }
    if (n <= 0 || (size_t)n >= sizeof(url_buf)) {
        alloc->free(alloc->ctx, body, body_len);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *auth = NULL;
    char auth_buf[512];
    if (cc->api_key && cc->api_key_len > 0) {
        n = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)cc->api_key_len, cc->api_key);
        if (n > 0 && (size_t)n < sizeof(auth_buf))
            auth = auth_buf;
    }

    hu_json_value_t *parsed = NULL;
    err = hu_provider_http_post_json(alloc, url_buf, auth, NULL, body, body_len, &parsed);
    alloc->free(alloc->ctx, body, body_len);
    if (err != HU_OK)
        return err;

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
            hu_json_value_t *tc_arr = hu_json_object_get(msg, "tool_calls");
            if (tc_arr && tc_arr->type == HU_JSON_ARRAY && tc_arr->data.array.len > 0) {
                size_t tc_count = tc_arr->data.array.len;
                hu_tool_call_t *tcs =
                    (hu_tool_call_t *)alloc->alloc(alloc->ctx, tc_count * sizeof(hu_tool_call_t));
                if (tcs) {
                    memset(tcs, 0, tc_count * sizeof(hu_tool_call_t));
                    size_t valid = 0;
                    for (size_t j = 0; j < tc_count; j++) {
                        hu_json_value_t *tc = tc_arr->data.array.items[j];
                        const char *tc_id = hu_json_get_string(tc, "id");
                        hu_json_value_t *fn = hu_json_object_get(tc, "function");
                        if (!fn || fn->type != HU_JSON_OBJECT)
                            continue;
                        const char *fn_name = hu_json_get_string(fn, "name");
                        const char *fn_args = hu_json_get_string(fn, "arguments");
                        if (!fn_name)
                            continue;
                        tcs[valid].id = tc_id ? hu_strndup(alloc, tc_id, strlen(tc_id)) : NULL;
                        tcs[valid].id_len = tc_id ? (size_t)strlen(tc_id) : 0;
                        tcs[valid].name = hu_strndup(alloc, fn_name, strlen(fn_name));
                        tcs[valid].name_len = (size_t)strlen(fn_name);
                        tcs[valid].arguments =
                            fn_args ? hu_strndup(alloc, fn_args, strlen(fn_args)) : NULL;
                        tcs[valid].arguments_len = fn_args ? (size_t)strlen(fn_args) : 0;
                        valid++;
                    }
                    out->tool_calls = tcs;
                    out->tool_calls_count = valid;
                }
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
#endif
}

static bool compatible_supports_native_tools(void *ctx) {
    (void)ctx;
    return true;
}
static const char *compatible_get_name(void *ctx) {
    (void)ctx;
    return "compatible";
}
static void compatible_deinit(void *ctx, hu_allocator_t *alloc) {
    hu_compatible_ctx_t *cc = (hu_compatible_ctx_t *)ctx;
    if (!cc)
        return;
    if (cc->api_key)
        free(cc->api_key);
    if (cc->base_url)
        free(cc->base_url);
    alloc->free(alloc->ctx, cc, sizeof(*cc));
}

static const hu_provider_vtable_t compatible_vtable;

static bool compatible_supports_streaming(void *ctx) {
    (void)ctx;
    return true;
}

static hu_error_t compatible_stream_chat(void *ctx, hu_allocator_t *alloc,
                                         const hu_chat_request_t *request, const char *model,
                                         size_t model_len, double temperature,
                                         hu_stream_callback_t callback, void *callback_ctx,
                                         hu_stream_chat_result_t *out) {
#if HU_IS_TEST
    (void)ctx;
    (void)alloc;
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    (void)callback_ctx;
    hu_stream_chunk_t chunk;
    memset(&chunk, 0, sizeof(chunk));
    chunk.delta = "test";
    chunk.delta_len = 4;
    chunk.is_final = false;
    if (callback)
        callback(callback_ctx, &chunk);
    memset(&chunk, 0, sizeof(chunk));
    chunk.is_final = true;
    if (callback)
        callback(callback_ctx, &chunk);
    if (out) {
        out->content = hu_strndup(alloc, "test", 4);
        out->content_len = 4;
    }
    return HU_OK;
#else
    hu_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));
    hu_error_t err =
        compatible_vtable.chat(ctx, alloc, request, model, model_len, temperature, &resp);
    if (err != HU_OK)
        return err;
    if (resp.content && resp.content_len > 0) {
        hu_stream_chunk_t chunk;
        memset(&chunk, 0, sizeof(chunk));
        chunk.delta = resp.content;
        chunk.delta_len = resp.content_len;
        chunk.is_final = false;
        if (callback)
            callback(callback_ctx, &chunk);
        memset(&chunk, 0, sizeof(chunk));
        chunk.is_final = true;
        if (callback)
            callback(callback_ctx, &chunk);
    }
    if (out) {
        out->content = resp.content;
        out->content_len = resp.content_len;
    } else if (resp.content) {
        alloc->free(alloc->ctx, (void *)resp.content, resp.content_len + 1);
    }
    return HU_OK;
#endif
}

static const hu_provider_vtable_t compatible_vtable = {
    .chat_with_system = compatible_chat_with_system,
    .chat = compatible_chat,
    .supports_native_tools = compatible_supports_native_tools,
    .get_name = compatible_get_name,
    .deinit = compatible_deinit,
    .warmup = NULL,
    .chat_with_tools = NULL,
    .supports_streaming = compatible_supports_streaming,
    .supports_vision = NULL,
    .supports_vision_for_model = NULL,
    .stream_chat = compatible_stream_chat,
};

hu_error_t hu_compatible_create(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                                const char *base_url, size_t base_url_len, hu_provider_t *out) {
    hu_compatible_ctx_t *cc = (hu_compatible_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*cc));
    if (!cc)
        return HU_ERR_OUT_OF_MEMORY;
    memset(cc, 0, sizeof(*cc));
    if (api_key && api_key_len > 0) {
        cc->api_key = (char *)malloc(api_key_len + 1);
        if (!cc->api_key) {
            alloc->free(alloc->ctx, cc, sizeof(*cc));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(cc->api_key, api_key, api_key_len);
        cc->api_key[api_key_len] = '\0';
        cc->api_key_len = api_key_len;
    }
    if (base_url && base_url_len > 0) {
        cc->base_url = (char *)malloc(base_url_len + 1);
        if (!cc->base_url) {
            if (cc->api_key)
                free(cc->api_key);
            alloc->free(alloc->ctx, cc, sizeof(*cc));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(cc->base_url, base_url, base_url_len);
        cc->base_url[base_url_len] = '\0';
        cc->base_url_len = base_url_len;
    }
    out->ctx = cc;
    out->vtable = &compatible_vtable;
    return HU_OK;
}
