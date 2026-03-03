#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/provider.h"
#include "seaclaw/providers/sse.h"
#include "seaclaw/websocket/websocket.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_OPENAI_URL     "https://api.openai.com/v1/chat/completions"
#define SC_OPENAI_URL_LEN (sizeof(SC_OPENAI_URL) - 1)

typedef struct sc_openai_ctx {
    char *api_key; /* owned */
    size_t api_key_len;
    char *base_url; /* owned, optional override */
    size_t base_url_len;
    bool ws_streaming; /* prefer WebSocket over SSE for streaming */
} sc_openai_ctx_t;

/* Perform HTTP POST. In test mode returns mock response. */
static sc_error_t sc_openai_http_post(sc_allocator_t *alloc, const char *url, size_t url_len,
                                      const char *auth_header, size_t auth_len, const char *body,
                                      size_t body_len, char **response_out,
                                      size_t *response_len_out) {
    (void)url;
    (void)url_len;
    (void)auth_header;
    (void)auth_len;
    *response_out = NULL;
    *response_len_out = 0;

#if SC_IS_TEST
    /* When request body contains "tools" but NOT "tool_call_id", return tool_calls (first round).
       When "tool_call_id" is present, we're in follow-up after tool execution — return text. */
    const char *mock;
    if (body && body_len > 0) {
        bool has_tools = false;
        bool has_tool_call_id = false;
        for (size_t i = 0; i + 14 <= body_len; i++) {
            if (memcmp(body + i, "\"tools\"", 7) == 0)
                has_tools = true;
            if (memcmp(body + i, "\"tool_call_id\"", 14) == 0)
                has_tool_call_id = true;
        }
        if (has_tools && !has_tool_call_id) {
            mock = "{\"choices\":[{\"message\":{\"content\":null,\"tool_calls\":[{\"id\":\"call_"
                   "mock1\","
                   "\"type\":\"function\",\"function\":{\"name\":\"shell\",\"arguments\":\"{"
                   "\\\"command\\\":\\\"ls\\\"}\"}}]}}],"
                   "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":8,\"total_tokens\":18},"
                   "\"model\":\"gpt-4\"}";
        } else {
            mock = "{\"choices\":[{\"message\":{\"content\":\"Hello from mock OpenAI\"}}],"
                   "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15},"
                   "\"model\":\"gpt-4\"}";
        }
    } else {
        mock = "{\"choices\":[{\"message\":{\"content\":\"Hello from mock OpenAI\"}}],"
               "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15},"
               "\"model\":\"gpt-4\"}";
    }
    size_t mock_len = strlen(mock);
    char *buf = (char *)alloc->alloc(alloc->ctx, mock_len + 1);
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;
    memcpy(buf, mock, mock_len + 1);
    *response_out = buf;
    *response_len_out = mock_len;
    return SC_OK;
#else
    {
        char url_buf[512];
        if (url_len >= sizeof(url_buf))
            return SC_ERR_INVALID_ARGUMENT;
        memcpy(url_buf, url, url_len);
        url_buf[url_len] = '\0';

        const char *auth = NULL;
        char auth_buf[512];
        if (auth_len > 0 && auth_len < sizeof(auth_buf)) {
            memcpy(auth_buf, auth_header, auth_len);
            auth_buf[auth_len] = '\0';
            auth = auth_buf;
        }

        sc_http_response_t hresp = {0};
        sc_error_t err = sc_http_post_json(alloc, url_buf, auth, body, body_len, &hresp);
        if (err != SC_OK)
            return err;

        if (hresp.status_code < 200 || hresp.status_code >= 300) {
            sc_http_response_free(alloc, &hresp);
            if (hresp.status_code == 401)
                return SC_ERR_PROVIDER_AUTH;
            if (hresp.status_code == 429)
                return SC_ERR_PROVIDER_RATE_LIMITED;
            return SC_ERR_PROVIDER_RESPONSE;
        }

        *response_out = hresp.body;
        *response_len_out = hresp.body_len;
        hresp.owned = false;
        return SC_OK;
    }
#endif
}

static sc_error_t openai_chat(void *ctx, sc_allocator_t *alloc, const sc_chat_request_t *request,
                              const char *model, size_t model_len, double temperature,
                              sc_chat_response_t *out);

static sc_error_t openai_chat_with_system(void *ctx, sc_allocator_t *alloc,
                                          const char *system_prompt, size_t system_prompt_len,
                                          const char *message, size_t message_len,
                                          const char *model, size_t model_len, double temperature,
                                          char **out, size_t *out_len) {
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
    sc_error_t err = openai_chat(ctx, alloc, &req, model, model_len, temperature, &resp);
    if (err != SC_OK)
        return err;

    if (resp.content && resp.content_len > 0) {
        *out = sc_strndup(alloc, resp.content, resp.content_len);
        if (!*out)
            return SC_ERR_OUT_OF_MEMORY;
        *out_len = resp.content_len;
    } else {
        *out = NULL;
        *out_len = 0;
    }
    return SC_OK;
}

static sc_error_t openai_chat(void *ctx, sc_allocator_t *alloc, const sc_chat_request_t *request,
                              const char *model, size_t model_len, double temperature,
                              sc_chat_response_t *out) {
    sc_openai_ctx_t *oc = (sc_openai_ctx_t *)ctx;
    if (!oc || !request || !out)
        return SC_ERR_INVALID_ARGUMENT;

    sc_json_value_t *root = sc_json_object_new(alloc);
    if (!root)
        return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *msgs_arr = sc_json_array_new(alloc);
    if (!msgs_arr) {
        sc_json_free(alloc, root);
        return SC_ERR_OUT_OF_MEMORY;
    }
    (void)sc_json_object_set(alloc, root, "messages", msgs_arr);

    for (size_t i = 0; i < request->messages_count; i++) {
        const sc_chat_message_t *m = &request->messages[i];
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) {
            sc_json_free(alloc, root);
            return SC_ERR_OUT_OF_MEMORY;
        }

        const char *role_str = "user";
        if (m->role == SC_ROLE_SYSTEM)
            role_str = "system";
        else if (m->role == SC_ROLE_ASSISTANT)
            role_str = "assistant";
        else if (m->role == SC_ROLE_TOOL)
            role_str = "tool";

        sc_json_value_t *role_val = sc_json_string_new(alloc, role_str, strlen(role_str));
        sc_json_object_set(alloc, obj, "role", role_val);

        if (m->content && m->content_len > 0) {
            sc_json_value_t *content_val = sc_json_string_new(alloc, m->content, m->content_len);
            sc_json_object_set(alloc, obj, "content", content_val);
        }
        if (m->role == SC_ROLE_TOOL && m->tool_call_id) {
            sc_json_value_t *id_val =
                sc_json_string_new(alloc, m->tool_call_id, m->tool_call_id_len);
            sc_json_object_set(alloc, obj, "tool_call_id", id_val);
        }
        if (m->role == SC_ROLE_TOOL && m->name) {
            sc_json_value_t *name_val = sc_json_string_new(alloc, m->name, m->name_len);
            sc_json_object_set(alloc, obj, "name", name_val);
        }
        if (m->role == SC_ROLE_ASSISTANT && m->tool_calls && m->tool_calls_count > 0) {
            sc_json_value_t *tc_arr = sc_json_array_new(alloc);
            if (tc_arr) {
                for (size_t k = 0; k < m->tool_calls_count; k++) {
                    const sc_tool_call_t *tc = &m->tool_calls[k];
                    sc_json_value_t *tc_obj = sc_json_object_new(alloc);
                    if (!tc_obj) {
                        sc_json_free(alloc, tc_arr);
                        sc_json_free(alloc, root);
                        return SC_ERR_OUT_OF_MEMORY;
                    }
                    if (tc->id && tc->id_len > 0)
                        sc_json_object_set(alloc, tc_obj, "id",
                                           sc_json_string_new(alloc, tc->id, tc->id_len));
                    sc_json_object_set(alloc, tc_obj, "type",
                                       sc_json_string_new(alloc, "function", 8));
                    sc_json_value_t *fn_obj = sc_json_object_new(alloc);
                    if (!fn_obj) {
                        sc_json_free(alloc, tc_obj);
                        sc_json_free(alloc, tc_arr);
                        sc_json_free(alloc, root);
                        return SC_ERR_OUT_OF_MEMORY;
                    }
                    if (tc->name && tc->name_len > 0)
                        sc_json_object_set(alloc, fn_obj, "name",
                                           sc_json_string_new(alloc, tc->name, tc->name_len));
                    if (tc->arguments && tc->arguments_len > 0)
                        sc_json_object_set(
                            alloc, fn_obj, "arguments",
                            sc_json_string_new(alloc, tc->arguments, tc->arguments_len));
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
        if (!tools_arr) {
            sc_json_free(alloc, root);
            return SC_ERR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < request->tools_count; i++) {
            sc_json_value_t *tool_obj = sc_json_object_new(alloc);
            if (!tool_obj) {
                sc_json_free(alloc, tools_arr);
                sc_json_free(alloc, root);
                return SC_ERR_OUT_OF_MEMORY;
            }
            sc_json_object_set(alloc, tool_obj, "type", sc_json_string_new(alloc, "function", 8));
            sc_json_value_t *fn_obj = sc_json_object_new(alloc);
            if (!fn_obj) {
                sc_json_free(alloc, tool_obj);
                sc_json_free(alloc, tools_arr);
                sc_json_free(alloc, root);
                return SC_ERR_OUT_OF_MEMORY;
            }
            sc_json_object_set(
                alloc, fn_obj, "name",
                sc_json_string_new(alloc, request->tools[i].name, request->tools[i].name_len));
            sc_json_object_set(
                alloc, fn_obj, "description",
                sc_json_string_new(
                    alloc, request->tools[i].description ? request->tools[i].description : "",
                    request->tools[i].description_len));
            if (request->tools[i].parameters_json && request->tools[i].parameters_json_len > 0) {
                sc_json_value_t *params = NULL;
                if (sc_json_parse(alloc, request->tools[i].parameters_json,
                                  request->tools[i].parameters_json_len, &params) == SC_OK) {
                    sc_json_object_set(alloc, fn_obj, "parameters", params);
                }
            }
            sc_json_object_set(alloc, tool_obj, "function", fn_obj);
            sc_json_array_push(alloc, tools_arr, tool_obj);
        }
        sc_json_object_set(alloc, root, "tools", tools_arr);
    }

    sc_json_value_t *model_val = sc_json_string_new(alloc, model, model_len);
    sc_json_object_set(alloc, root, "model", model_val);

    sc_json_value_t *temp_val = sc_json_number_new(alloc, temperature);
    sc_json_object_set(alloc, root, "temperature", temp_val);

    char *body = NULL;
    size_t body_len = 0;
    sc_error_t err = sc_json_stringify(alloc, root, &body, &body_len);
    sc_json_free(alloc, root);
    if (err != SC_OK)
        return err;

    const char *url = oc->base_url ? oc->base_url : SC_OPENAI_URL;
    size_t url_len = oc->base_url_len ? oc->base_url_len : SC_OPENAI_URL_LEN;

    char auth_buf[256];
    size_t auth_len = 0;
    if (oc->api_key && oc->api_key_len > 0) {
        int n =
            snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)oc->api_key_len, oc->api_key);
        if (n > 0 && (size_t)n < sizeof(auth_buf))
            auth_len = (size_t)n;
    }

    char *resp_body = NULL;
    size_t resp_len = 0;
    err = sc_openai_http_post(alloc, url, url_len, auth_buf, auth_len, body, body_len, &resp_body,
                              &resp_len);
    alloc->free(alloc->ctx, body, body_len);
    if (err != SC_OK)
        return err;

    sc_json_value_t *parsed = NULL;
    err = sc_json_parse(alloc, resp_body, resp_len, &parsed);
    alloc->free(alloc->ctx, resp_body, resp_len);
    if (err != SC_OK)
        return err;

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
            sc_json_value_t *tc_arr = sc_json_object_get(msg, "tool_calls");
            if (tc_arr && tc_arr->type == SC_JSON_ARRAY && tc_arr->data.array.len > 0) {
                size_t tc_count = tc_arr->data.array.len;
                sc_tool_call_t *tcs =
                    (sc_tool_call_t *)alloc->alloc(alloc->ctx, tc_count * sizeof(sc_tool_call_t));
                if (tcs) {
                    memset(tcs, 0, tc_count * sizeof(sc_tool_call_t));
                    size_t valid = 0;
                    for (size_t j = 0; j < tc_count; j++) {
                        sc_json_value_t *tc = tc_arr->data.array.items[j];
                        const char *tc_id = sc_json_get_string(tc, "id");
                        sc_json_value_t *fn = sc_json_object_get(tc, "function");
                        if (!fn || fn->type != SC_JSON_OBJECT)
                            continue;
                        const char *fn_name = sc_json_get_string(fn, "name");
                        const char *fn_args = sc_json_get_string(fn, "arguments");
                        if (!fn_name)
                            continue;
                        tcs[valid].id = tc_id ? sc_strndup(alloc, tc_id, strlen(tc_id)) : NULL;
                        tcs[valid].id_len = tc_id ? (size_t)strlen(tc_id) : 0;
                        tcs[valid].name = sc_strndup(alloc, fn_name, strlen(fn_name));
                        tcs[valid].name_len = (size_t)strlen(fn_name);
                        tcs[valid].arguments =
                            fn_args ? sc_strndup(alloc, fn_args, strlen(fn_args)) : NULL;
                        tcs[valid].arguments_len = fn_args ? (size_t)strlen(fn_args) : 0;
                        valid++;
                    }
                    out->tool_calls = tcs;
                    out->tool_calls_count = valid;
                }
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

static bool openai_supports_native_tools(void *ctx) {
    (void)ctx;
    return true;
}

static bool openai_supports_streaming(void *ctx) {
    (void)ctx;
    return true;
}

#if !SC_IS_TEST
typedef struct openai_stream_ctx {
    sc_allocator_t *alloc;
    sc_stream_callback_t callback;
    void *callback_ctx;
    sc_sse_parser_t parser;
    sc_error_t last_error;
    char *content_buf;
    size_t content_len;
    size_t content_cap;
} openai_stream_ctx_t;

static void append_content(openai_stream_ctx_t *s, const char *delta, size_t delta_len);

static bool is_done_signal(const char *data, size_t data_len) {
    if (!data)
        return false;
    while (data_len > 0 && (*data == ' ' || *data == '\t')) {
        data++;
        data_len--;
    }
    if (data_len == 6 && memcmp(data, "[DONE]", 6) == 0)
        return true;
    return false;
}

static void extract_openai_delta(openai_stream_ctx_t *s, const char *json_str, size_t json_len) {
    sc_json_value_t *parsed = NULL;
    if (sc_json_parse(s->alloc, json_str, json_len, &parsed) != SC_OK)
        return;
    sc_json_value_t *choices = sc_json_object_get(parsed, "choices");
    if (!choices || choices->type != SC_JSON_ARRAY || choices->data.array.len == 0) {
        sc_json_free(s->alloc, parsed);
        return;
    }
    sc_json_value_t *first = choices->data.array.items[0];
    sc_json_value_t *delta = sc_json_object_get(first, "delta");
    if (!delta || delta->type != SC_JSON_OBJECT) {
        sc_json_free(s->alloc, parsed);
        return;
    }
    const char *content = sc_json_get_string(delta, "content");
    if (content) {
        size_t clen = strlen(content);
        if (clen > 0) {
            append_content(s, content, clen);
            sc_stream_chunk_t chunk = {
                .delta = content,
                .delta_len = clen,
                .is_final = false,
                .token_count = 0,
            };
            s->callback(s->callback_ctx, &chunk);
        }
    }
    sc_json_free(s->alloc, parsed);
}

static void append_content(openai_stream_ctx_t *s, const char *delta, size_t delta_len) {
    if (!delta || delta_len == 0)
        return;
    while (s->content_len + delta_len + 1 > s->content_cap) {
        size_t new_cap = s->content_cap ? s->content_cap * 2 : 256;
        while (new_cap < s->content_len + delta_len + 1)
            new_cap *= 2;
        char *n;
        if (s->content_buf) {
            n = (char *)s->alloc->realloc(s->alloc->ctx, s->content_buf, s->content_cap, new_cap);
        } else {
            n = (char *)s->alloc->alloc(s->alloc->ctx, new_cap);
        }
        if (!n) {
            s->last_error = SC_ERR_OUT_OF_MEMORY;
            return;
        }
        s->content_buf = n;
        s->content_cap = new_cap;
    }
    memcpy(s->content_buf + s->content_len, delta, delta_len);
    s->content_len += delta_len;
    s->content_buf[s->content_len] = '\0';
}

static void openai_sse_event_cb(const char *event_type, size_t event_type_len, const char *data,
                                size_t data_len, void *userdata) {
    openai_stream_ctx_t *s = (openai_stream_ctx_t *)userdata;
    (void)event_type;
    (void)event_type_len;
    if (s->last_error != SC_OK)
        return;
    if (!data || data_len == 0)
        return;
    if (is_done_signal(data, data_len)) {
        sc_stream_chunk_t chunk = {
            .delta = NULL,
            .delta_len = 0,
            .is_final = true,
            .token_count = 0,
        };
        s->callback(s->callback_ctx, &chunk);
        return;
    }
    extract_openai_delta(s, data, data_len);
}

static size_t openai_stream_write_cb(const char *data, size_t len, void *userdata) {
    openai_stream_ctx_t *s = (openai_stream_ctx_t *)userdata;
    sc_error_t err = sc_sse_parser_feed(&s->parser, data, len, openai_sse_event_cb, s);
    if (err != SC_OK) {
        s->last_error = err;
        return 0;
    }
    return len;
}
#endif /* !SC_IS_TEST */

/* WebSocket streaming: convert base URL to wss:// and stream via WS frames.
 * Returns SC_ERR_NOT_SUPPORTED if WS connect fails, signaling SSE fallback. */
#if !SC_IS_TEST && defined(SC_GATEWAY_POSIX)
static sc_error_t openai_stream_ws(sc_openai_ctx_t *oc, sc_allocator_t *alloc,
                                   const char *body, size_t body_len,
                                   sc_stream_callback_t callback, void *callback_ctx,
                                   sc_stream_chat_result_t *out) {
    const char *base = oc->base_url ? oc->base_url : "https://api.openai.com/v1/chat/completions";
    char ws_url[512];
    const char *p = base;
    if (strncmp(p, "https://", 8) == 0) {
        int n = snprintf(ws_url, sizeof(ws_url), "wss://%s", p + 8);
        if (n <= 0 || (size_t)n >= sizeof(ws_url))
            return SC_ERR_NOT_SUPPORTED;
    } else if (strncmp(p, "http://", 7) == 0) {
        int n = snprintf(ws_url, sizeof(ws_url), "ws://%s", p + 7);
        if (n <= 0 || (size_t)n >= sizeof(ws_url))
            return SC_ERR_NOT_SUPPORTED;
    } else {
        return SC_ERR_NOT_SUPPORTED;
    }

    char auth_hdr[300];
    if (oc->api_key && oc->api_key_len > 0)
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %.*s", (int)oc->api_key_len, oc->api_key);
    else
        auth_hdr[0] = '\0';

    sc_ws_conn_t *ws = sc_ws_connect(alloc, ws_url, NULL);
    if (!ws)
        return SC_ERR_NOT_SUPPORTED;

    sc_error_t err = sc_ws_send(ws, body, body_len, false);
    if (err != SC_OK) {
        sc_ws_close(ws);
        return SC_ERR_NOT_SUPPORTED;
    }

    char *content_buf = NULL;
    size_t content_len = 0, content_cap = 0;

    for (;;) {
        char *frame = NULL;
        size_t frame_len = 0;
        bool is_binary = false;
        err = sc_ws_recv(ws, alloc, &frame, &frame_len, &is_binary);
        if (err != SC_OK || !frame)
            break;

        if (frame_len >= 6 && memcmp(frame, "[DONE]", 6) == 0) {
            alloc->free(alloc->ctx, frame, frame_len);
            break;
        }

        sc_json_value_t *root = NULL;
        if (sc_json_parse(alloc, frame, frame_len, &root) == SC_OK && root) {
            sc_json_value_t *choices = sc_json_object_get(root, "choices");
            if (choices && choices->type == SC_JSON_ARRAY &&
                choices->data.array.count > 0) {
                sc_json_value_t *c0 = choices->data.array.items[0];
                sc_json_value_t *delta = sc_json_object_get(c0, "delta");
                if (delta) {
                    const char *text = sc_json_get_string(delta, "content");
                    if (text) {
                        size_t tlen = strlen(text);
                        sc_stream_chunk_t chunk = {
                            .delta = text, .delta_len = tlen,
                            .is_final = false, .token_count = 1,
                        };
                        callback(callback_ctx, &chunk);
                        if (content_len + tlen >= content_cap) {
                            size_t nc = content_cap ? content_cap * 2 : 256;
                            if (nc < content_len + tlen + 1)
                                nc = content_len + tlen + 1;
                            char *nb = (char *)alloc->alloc(alloc->ctx, nc);
                            if (nb) {
                                if (content_buf) {
                                    memcpy(nb, content_buf, content_len);
                                    alloc->free(alloc->ctx, content_buf, content_cap);
                                }
                                content_buf = nb;
                                content_cap = nc;
                            }
                        }
                        if (content_buf && content_len + tlen < content_cap) {
                            memcpy(content_buf + content_len, text, tlen);
                            content_len += tlen;
                            content_buf[content_len] = '\0';
                        }
                    }
                }
            }
            sc_json_free(alloc, root);
        }
        alloc->free(alloc->ctx, frame, frame_len);
    }

    sc_ws_close(ws);

    sc_stream_chunk_t final_chunk = {
        .delta = NULL, .delta_len = 0, .is_final = true, .token_count = 0,
    };
    callback(callback_ctx, &final_chunk);

    if (content_buf && content_len > 0) {
        out->content = sc_strndup(alloc, content_buf, content_len);
        out->content_len = content_len;
        alloc->free(alloc->ctx, content_buf, content_cap);
    }
    return SC_OK;
}
#endif /* !SC_IS_TEST && SC_GATEWAY_POSIX */

static sc_error_t openai_stream_chat(void *ctx, sc_allocator_t *alloc,
                                     const sc_chat_request_t *request, const char *model,
                                     size_t model_len, double temperature,
                                     sc_stream_callback_t callback, void *callback_ctx,
                                     sc_stream_chat_result_t *out) {
    sc_openai_ctx_t *oc = (sc_openai_ctx_t *)ctx;
    if (!oc || !request || !callback || !out)
        return SC_ERR_INVALID_ARGUMENT;
    (void)model;
    (void)model_len;
    (void)temperature;

    memset(out, 0, sizeof(*out));

#if SC_IS_TEST
    const char *chunks[] = {"Hello ", "from ", "mock"};
    for (int i = 0; i < 3; i++) {
        sc_stream_chunk_t c = {
            .delta = chunks[i],
            .delta_len = strlen(chunks[i]),
            .is_final = false,
            .token_count = 1,
        };
        callback(callback_ctx, &c);
    }
    {
        sc_stream_chunk_t c = {.delta = NULL, .delta_len = 0, .is_final = true, .token_count = 3};
        callback(callback_ctx, &c);
    }
    out->content = sc_strndup(alloc, "Hello from mock", 15);
    out->content_len = 15;
    out->usage.completion_tokens = 3;
    return SC_OK;
#else
    sc_json_value_t *root = sc_json_object_new(alloc);
    if (!root)
        return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *msgs_arr = sc_json_array_new(alloc);
    if (!msgs_arr) {
        sc_json_free(alloc, root);
        return SC_ERR_OUT_OF_MEMORY;
    }
    (void)sc_json_object_set(alloc, root, "messages", msgs_arr);

    for (size_t i = 0; i < request->messages_count; i++) {
        const sc_chat_message_t *m = &request->messages[i];
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) {
            sc_json_free(alloc, root);
            return SC_ERR_OUT_OF_MEMORY;
        }
        const char *role_str = "user";
        if (m->role == SC_ROLE_SYSTEM)
            role_str = "system";
        else if (m->role == SC_ROLE_ASSISTANT)
            role_str = "assistant";
        else if (m->role == SC_ROLE_TOOL)
            role_str = "tool";
        sc_json_object_set(alloc, obj, "role",
                           sc_json_string_new(alloc, role_str, strlen(role_str)));
        if (m->content && m->content_len > 0) {
            sc_json_object_set(alloc, obj, "content",
                               sc_json_string_new(alloc, m->content, m->content_len));
        }
        if (m->role == SC_ROLE_TOOL && m->tool_call_id) {
            sc_json_object_set(alloc, obj, "tool_call_id",
                               sc_json_string_new(alloc, m->tool_call_id, m->tool_call_id_len));
        }
        if (m->role == SC_ROLE_TOOL && m->name) {
            sc_json_object_set(alloc, obj, "name", sc_json_string_new(alloc, m->name, m->name_len));
        }
        sc_json_array_push(alloc, msgs_arr, obj);
    }

    sc_json_object_set(alloc, root, "model", sc_json_string_new(alloc, model, model_len));
    sc_json_object_set(alloc, root, "temperature", sc_json_number_new(alloc, temperature));
    sc_json_object_set(alloc, root, "stream", sc_json_bool_new(alloc, true));

    char *body = NULL;
    size_t body_len = 0;
    sc_error_t err = sc_json_stringify(alloc, root, &body, &body_len);
    sc_json_free(alloc, root);
    if (err != SC_OK)
        return err;

    const char *url = oc->base_url ? oc->base_url : SC_OPENAI_URL;
    char url_buf[512];
    size_t url_len = oc->base_url_len ? oc->base_url_len : SC_OPENAI_URL_LEN;
    if (url_len >= sizeof(url_buf)) {
        alloc->free(alloc->ctx, body, body_len);
        return SC_ERR_INVALID_ARGUMENT;
    }
    memcpy(url_buf, url, url_len);
    url_buf[url_len] = '\0';

    char auth_buf[256];
    size_t auth_len = 0;
    if (oc->api_key && oc->api_key_len > 0) {
        int n =
            snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)oc->api_key_len, oc->api_key);
        if (n > 0 && (size_t)n < sizeof(auth_buf))
            auth_len = (size_t)n;
    }

    openai_stream_ctx_t sctx = {
        .alloc = alloc,
        .callback = callback,
        .callback_ctx = callback_ctx,
        .last_error = SC_OK,
        .content_buf = NULL,
        .content_len = 0,
        .content_cap = 0,
    };
    err = sc_sse_parser_init(&sctx.parser, alloc);
    if (err != SC_OK) {
        alloc->free(alloc->ctx, body, body_len);
        return err;
    }
    err = sc_http_post_json_stream(alloc, url_buf, auth_len ? auth_buf : NULL, NULL, body, body_len,
                                   openai_stream_write_cb, &sctx);
    sc_sse_parser_deinit(&sctx.parser);
    alloc->free(alloc->ctx, body, body_len);
    if (err != SC_OK)
        return err;
    if (sctx.last_error != SC_OK) {
        if (sctx.content_buf)
            alloc->free(alloc->ctx, sctx.content_buf, sctx.content_cap);
        return sctx.last_error;
    }

    if (sctx.content_buf && sctx.content_len > 0) {
        out->content = sc_strndup(alloc, sctx.content_buf, sctx.content_len);
        out->content_len = sctx.content_len;
        alloc->free(alloc->ctx, sctx.content_buf, sctx.content_cap);
    }

    return SC_OK;
#endif
}

static const char *openai_get_name(void *ctx) {
    (void)ctx;
    return "openai";
}

static void openai_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    sc_openai_ctx_t *oc = (sc_openai_ctx_t *)ctx;
    if (!oc)
        return;
    if (oc->api_key)
        free(oc->api_key);
    if (oc->base_url)
        free(oc->base_url);
    free(oc);
}

static const sc_provider_vtable_t openai_vtable = {
    .chat_with_system = openai_chat_with_system,
    .chat = openai_chat,
    .supports_native_tools = openai_supports_native_tools,
    .get_name = openai_get_name,
    .deinit = openai_deinit,
    .warmup = NULL,
    .chat_with_tools = NULL,
    .supports_streaming = openai_supports_streaming,
    .supports_vision = NULL,
    .supports_vision_for_model = NULL,
    .stream_chat = openai_stream_chat,
};

sc_error_t sc_openai_create(sc_allocator_t *alloc, const char *api_key, size_t api_key_len,
                            const char *base_url, size_t base_url_len, sc_provider_t *out) {
    (void)alloc;
    sc_openai_ctx_t *oc = (sc_openai_ctx_t *)calloc(1, sizeof(*oc));
    if (!oc)
        return SC_ERR_OUT_OF_MEMORY;

    if (api_key && api_key_len > 0) {
        oc->api_key = (char *)malloc(api_key_len + 1);
        if (!oc->api_key) {
            free(oc);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(oc->api_key, api_key, api_key_len);
        oc->api_key[api_key_len] = '\0';
        oc->api_key_len = api_key_len;
    }
    if (base_url && base_url_len > 0) {
        oc->base_url = (char *)malloc(base_url_len + 1);
        if (!oc->base_url) {
            if (oc->api_key)
                free(oc->api_key);
            free(oc);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(oc->base_url, base_url, base_url_len);
        oc->base_url[base_url_len] = '\0';
        oc->base_url_len = base_url_len;
    }

    out->ctx = oc;
    out->vtable = &openai_vtable;
    return SC_OK;
}

void sc_openai_set_ws_streaming(sc_provider_t *p, bool enabled) {
    if (!p || !p->ctx)
        return;
    sc_openai_ctx_t *oc = (sc_openai_ctx_t *)p->ctx;
    oc->ws_streaming = enabled;
}
