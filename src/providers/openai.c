#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/provider.h"
#include "human/providers/provider_http.h"
#include "human/providers/sse.h"
#include "human/websocket/websocket.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_OPENAI_URL     "https://api.openai.com/v1/chat/completions"
#define HU_OPENAI_URL_LEN (sizeof(HU_OPENAI_URL) - 1)

typedef struct hu_openai_ctx {
    char *api_key; /* owned */
    size_t api_key_len;
    char *base_url; /* owned, optional override */
    size_t base_url_len;
    bool ws_streaming; /* prefer WebSocket over SSE for streaming */
} hu_openai_ctx_t;

#if HU_IS_TEST
/* Mock HTTP POST for tests. When body contains "tools" but NOT "tool_call_id", return tool_calls.
   When "tool_call_id" is present, return text. */
static hu_error_t hu_openai_http_post(hu_allocator_t *alloc, const char *url, size_t url_len,
                                      const char *auth_header, size_t auth_len, const char *body,
                                      size_t body_len, char **response_out,
                                      size_t *response_len_out) {
    (void)url;
    (void)url_len;
    (void)auth_header;
    (void)auth_len;
    *response_out = NULL;
    *response_len_out = 0;

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
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, mock, mock_len + 1);
    *response_out = buf;
    *response_len_out = mock_len;
    return HU_OK;
}
#endif

static hu_error_t openai_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                              const char *model, size_t model_len, double temperature,
                              hu_chat_response_t *out);

static hu_error_t openai_chat_with_system(void *ctx, hu_allocator_t *alloc,
                                          const char *system_prompt, size_t system_prompt_len,
                                          const char *message, size_t message_len,
                                          const char *model, size_t model_len, double temperature,
                                          char **out, size_t *out_len) {
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
    hu_error_t err = openai_chat(ctx, alloc, &req, model, model_len, temperature, &resp);
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

static hu_error_t openai_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                              const char *model, size_t model_len, double temperature,
                              hu_chat_response_t *out) {
    hu_openai_ctx_t *oc = (hu_openai_ctx_t *)ctx;
    if (!oc || !request || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_json_value_t *root = hu_json_object_new(alloc);
    if (!root)
        return HU_ERR_OUT_OF_MEMORY;

    hu_json_value_t *msgs_arr = hu_json_array_new(alloc);
    if (!msgs_arr) {
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    (void)hu_json_object_set(alloc, root, "messages", msgs_arr);

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

        hu_json_value_t *role_val = hu_json_string_new(alloc, role_str, strlen(role_str));
        hu_json_object_set(alloc, obj, "role", role_val);

        if (m->content_parts && m->content_parts_count > 0) {
            hu_json_value_t *parts_arr = hu_json_array_new(alloc);
            if (parts_arr) {
                for (size_t p = 0; p < m->content_parts_count; p++) {
                    const hu_content_part_t *cp = &m->content_parts[p];
                    hu_json_value_t *part = hu_json_object_new(alloc);
                    if (!part)
                        break;
                    if (cp->tag == HU_CONTENT_PART_TEXT) {
                        hu_json_object_set(alloc, part, "type",
                                           hu_json_string_new(alloc, "text", 4));
                        hu_json_object_set(
                            alloc, part, "text",
                            hu_json_string_new(alloc, cp->data.text.ptr, cp->data.text.len));
                    } else if (cp->tag == HU_CONTENT_PART_IMAGE_URL) {
                        hu_json_object_set(alloc, part, "type",
                                           hu_json_string_new(alloc, "image_url", 9));
                        hu_json_value_t *iu = hu_json_object_new(alloc);
                        if (iu) {
                            hu_json_object_set(alloc, iu, "url",
                                               hu_json_string_new(alloc, cp->data.image_url.url,
                                                                  cp->data.image_url.url_len));
                            hu_json_object_set(alloc, part, "image_url", iu);
                        }
                    } else if (cp->tag == HU_CONTENT_PART_IMAGE_BASE64) {
                        hu_json_object_set(alloc, part, "type",
                                           hu_json_string_new(alloc, "image_url", 9));
                        hu_json_value_t *iu = hu_json_object_new(alloc);
                        if (iu) {
                            /* Build data URI: data:<mime>;base64,<data> */
                            size_t uri_len = 5 + cp->data.image_base64.media_type_len + 8 +
                                             cp->data.image_base64.data_len;
                            char *uri = (char *)alloc->alloc(alloc->ctx, uri_len + 1);
                            if (uri) {
                                size_t off = 0;
                                memcpy(uri + off, "data:", 5);
                                off += 5;
                                memcpy(uri + off, cp->data.image_base64.media_type,
                                       cp->data.image_base64.media_type_len);
                                off += cp->data.image_base64.media_type_len;
                                memcpy(uri + off, ";base64,", 8);
                                off += 8;
                                memcpy(uri + off, cp->data.image_base64.data,
                                       cp->data.image_base64.data_len);
                                off += cp->data.image_base64.data_len;
                                uri[off] = '\0';
                                hu_json_object_set(alloc, iu, "url",
                                                   hu_json_string_new(alloc, uri, off));
                                alloc->free(alloc->ctx, uri, uri_len + 1);
                            }
                            hu_json_object_set(alloc, part, "image_url", iu);
                        }
                    } else if (cp->tag == HU_CONTENT_PART_AUDIO_BASE64) {
                        /* OpenAI input_audio:
                         * {"type":"input_audio","input_audio":{"data":"<base64>","format":"<format>"}}
                         */
                        const char *mt = cp->data.audio_base64.media_type;
                        size_t mt_len = cp->data.audio_base64.media_type_len;
                        const char *slash = memchr(mt, '/', mt_len);
                        const char *format = slash ? slash + 1 : mt;
                        size_t format_len = slash ? (size_t)(mt + mt_len - format) : mt_len;
                        hu_json_object_set(alloc, part, "type",
                                           hu_json_string_new(alloc, "input_audio", 11));
                        hu_json_value_t *ia = hu_json_object_new(alloc);
                        if (ia) {
                            hu_json_object_set(alloc, ia, "data",
                                               hu_json_string_new(alloc, cp->data.audio_base64.data,
                                                                  cp->data.audio_base64.data_len));
                            hu_json_object_set(alloc, ia, "format",
                                               hu_json_string_new(alloc, format, format_len));
                            hu_json_object_set(alloc, part, "input_audio", ia);
                        }
                    } else if (cp->tag == HU_CONTENT_PART_VIDEO_URL) {
                        /* OpenAI does not support video; treat URL as text description */
                        hu_json_object_set(alloc, part, "type",
                                           hu_json_string_new(alloc, "text", 4));
                        hu_json_object_set(alloc, part, "text",
                                           hu_json_string_new(alloc, cp->data.video_url.url,
                                                              cp->data.video_url.url_len));
                    }
                    hu_json_array_push(alloc, parts_arr, part);
                }
                hu_json_object_set(alloc, obj, "content", parts_arr);
            }
        } else if (m->content && m->content_len > 0) {
            hu_json_value_t *content_val = hu_json_string_new(alloc, m->content, m->content_len);
            hu_json_object_set(alloc, obj, "content", content_val);
        }
        if (m->role == HU_ROLE_TOOL && m->tool_call_id) {
            hu_json_value_t *id_val =
                hu_json_string_new(alloc, m->tool_call_id, m->tool_call_id_len);
            hu_json_object_set(alloc, obj, "tool_call_id", id_val);
        }
        if (m->role == HU_ROLE_TOOL && m->name) {
            hu_json_value_t *name_val = hu_json_string_new(alloc, m->name, m->name_len);
            hu_json_object_set(alloc, obj, "name", name_val);
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
        if (!tools_arr) {
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < request->tools_count; i++) {
            hu_json_value_t *tool_obj = hu_json_object_new(alloc);
            if (!tool_obj) {
                hu_json_free(alloc, tools_arr);
                hu_json_free(alloc, root);
                return HU_ERR_OUT_OF_MEMORY;
            }
            hu_json_object_set(alloc, tool_obj, "type", hu_json_string_new(alloc, "function", 8));
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
            if (request->tools[i].parameters_json && request->tools[i].parameters_json_len > 0) {
                hu_json_value_t *params = NULL;
                if (hu_json_parse(alloc, request->tools[i].parameters_json,
                                  request->tools[i].parameters_json_len, &params) == HU_OK) {
                    hu_json_object_set(alloc, fn_obj, "parameters", params);
                }
            }
            hu_json_object_set(alloc, tool_obj, "function", fn_obj);
            hu_json_array_push(alloc, tools_arr, tool_obj);
        }
        hu_json_object_set(alloc, root, "tools", tools_arr);
    }

    hu_json_value_t *model_val = hu_json_string_new(alloc, model, model_len);
    hu_json_object_set(alloc, root, "model", model_val);

    hu_json_value_t *temp_val = hu_json_number_new(alloc, temperature);
    hu_json_object_set(alloc, root, "temperature", temp_val);

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

    const char *url = oc->base_url ? oc->base_url : HU_OPENAI_URL;
    size_t url_len = oc->base_url_len ? oc->base_url_len : HU_OPENAI_URL_LEN;

    char auth_buf[256];
    size_t auth_len = 0;
    if (oc->api_key && oc->api_key_len > 0) {
        int n =
            snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)oc->api_key_len, oc->api_key);
        if (n > 0 && (size_t)n < sizeof(auth_buf))
            auth_len = (size_t)n;
    }

    hu_json_value_t *parsed = NULL;
#if HU_IS_TEST
    {
        char *resp_body = NULL;
        size_t resp_len = 0;
        err = hu_openai_http_post(alloc, url, url_len, auth_buf, auth_len, body, body_len,
                                  &resp_body, &resp_len);
        if (err == HU_OK && resp_body)
            err = hu_json_parse(alloc, resp_body, resp_len, &parsed);
        if (resp_body)
            alloc->free(alloc->ctx, resp_body, resp_len);
    }
#else
    {
        char url_buf[512];
        if (url_len >= sizeof(url_buf)) {
            alloc->free(alloc->ctx, body, body_len);
            return HU_ERR_INVALID_ARGUMENT;
        }
        memcpy(url_buf, url, url_len);
        url_buf[url_len] = '\0';
        err = hu_provider_http_post_json(alloc, url_buf, auth_len ? auth_buf : NULL, NULL, body,
                                         body_len, &parsed);
    }
#endif
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
}

static bool openai_supports_native_tools(void *ctx) {
    (void)ctx;
    return true;
}

static bool openai_supports_streaming(void *ctx) {
    (void)ctx;
    return true;
}

#if !HU_IS_TEST
typedef struct openai_stream_ctx {
    hu_allocator_t *alloc;
    hu_stream_callback_t callback;
    void *callback_ctx;
    hu_sse_parser_t parser;
    hu_error_t last_error;
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
    hu_json_value_t *parsed = NULL;
    if (hu_json_parse(s->alloc, json_str, json_len, &parsed) != HU_OK)
        return;
    hu_json_value_t *choices = hu_json_object_get(parsed, "choices");
    if (!choices || choices->type != HU_JSON_ARRAY || choices->data.array.len == 0) {
        hu_json_free(s->alloc, parsed);
        return;
    }
    hu_json_value_t *first = choices->data.array.items[0];
    hu_json_value_t *delta = hu_json_object_get(first, "delta");
    if (!delta || delta->type != HU_JSON_OBJECT) {
        hu_json_free(s->alloc, parsed);
        return;
    }
    const char *content = hu_json_get_string(delta, "content");
    if (content) {
        size_t clen = strlen(content);
        if (clen > 0) {
            append_content(s, content, clen);
            hu_stream_chunk_t chunk = {
                .delta = content,
                .delta_len = clen,
                .is_final = false,
                .token_count = 0,
            };
            s->callback(s->callback_ctx, &chunk);
        }
    }
    hu_json_free(s->alloc, parsed);
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
            s->last_error = HU_ERR_OUT_OF_MEMORY;
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
    if (s->last_error != HU_OK)
        return;
    if (!data || data_len == 0)
        return;
    if (is_done_signal(data, data_len)) {
        hu_stream_chunk_t chunk = {
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
    hu_error_t err = hu_sse_parser_feed(&s->parser, data, len, openai_sse_event_cb, s);
    if (err != HU_OK) {
        s->last_error = err;
        return 0;
    }
    return len;
}
#endif /* !HU_IS_TEST */

/* WebSocket streaming: convert base URL to wss:// and stream via WS frames.
 * Returns HU_ERR_NOT_SUPPORTED if WS connect fails, signaling SSE fallback. */
#if !HU_IS_TEST && defined(HU_GATEWAY_POSIX)
static hu_error_t openai_stream_ws(hu_openai_ctx_t *oc, hu_allocator_t *alloc, const char *body,
                                   size_t body_len, hu_stream_callback_t callback,
                                   void *callback_ctx, hu_stream_chat_result_t *out) {
    const char *base = oc->base_url ? oc->base_url : "https://api.openai.com/v1/chat/completions";
    char ws_url[512];
    const char *p = base;
    if (strncmp(p, "https://", 8) == 0) {
        int n = snprintf(ws_url, sizeof(ws_url), "wss://%s", p + 8);
        if (n <= 0 || (size_t)n >= sizeof(ws_url))
            return HU_ERR_NOT_SUPPORTED;
    } else if (strncmp(p, "http://", 7) == 0) {
        int n = snprintf(ws_url, sizeof(ws_url), "ws://%s", p + 7);
        if (n <= 0 || (size_t)n >= sizeof(ws_url))
            return HU_ERR_NOT_SUPPORTED;
    } else {
        return HU_ERR_NOT_SUPPORTED;
    }

    char auth_hdr[300];
    if (oc->api_key && oc->api_key_len > 0)
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %.*s", (int)oc->api_key_len, oc->api_key);
    else
        auth_hdr[0] = '\0';

    hu_ws_client_t *ws = NULL;
    hu_error_t conn_err = hu_ws_connect(alloc, ws_url, &ws);
    if (conn_err != HU_OK || !ws)
        return HU_ERR_NOT_SUPPORTED;

    hu_error_t err = hu_ws_send(ws, body, body_len);
    if (err != HU_OK) {
        hu_ws_close(ws, alloc);
        return HU_ERR_NOT_SUPPORTED;
    }

    char *content_buf = NULL;
    size_t content_len = 0, content_cap = 0;

    for (;;) {
        char *frame = NULL;
        size_t frame_len = 0;
        err = hu_ws_recv(ws, alloc, &frame, &frame_len);
        if (err != HU_OK || !frame)
            break;

        if (frame_len >= 6 && memcmp(frame, "[DONE]", 6) == 0) {
            alloc->free(alloc->ctx, frame, frame_len);
            break;
        }

        hu_json_value_t *root = NULL;
        if (hu_json_parse(alloc, frame, frame_len, &root) == HU_OK && root) {
            hu_json_value_t *choices = hu_json_object_get(root, "choices");
            if (choices && choices->type == HU_JSON_ARRAY && choices->data.array.len > 0) {
                hu_json_value_t *c0 = choices->data.array.items[0];
                hu_json_value_t *delta = hu_json_object_get(c0, "delta");
                if (delta) {
                    const char *text = hu_json_get_string(delta, "content");
                    if (text) {
                        size_t tlen = strlen(text);
                        hu_stream_chunk_t chunk = {
                            .delta = text,
                            .delta_len = tlen,
                            .is_final = false,
                            .token_count = 1,
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
            hu_json_free(alloc, root);
        }
        alloc->free(alloc->ctx, frame, frame_len);
    }

    hu_ws_close(ws, alloc);

    hu_stream_chunk_t final_chunk = {
        .delta = NULL,
        .delta_len = 0,
        .is_final = true,
        .token_count = 0,
    };
    callback(callback_ctx, &final_chunk);

    if (content_buf && content_len > 0) {
        out->content = hu_strndup(alloc, content_buf, content_len);
        out->content_len = content_len;
        alloc->free(alloc->ctx, content_buf, content_cap);
    }
    return HU_OK;
}
#endif /* !HU_IS_TEST && HU_GATEWAY_POSIX */

static hu_error_t openai_stream_chat(void *ctx, hu_allocator_t *alloc,
                                     const hu_chat_request_t *request, const char *model,
                                     size_t model_len, double temperature,
                                     hu_stream_callback_t callback, void *callback_ctx,
                                     hu_stream_chat_result_t *out) {
    hu_openai_ctx_t *oc = (hu_openai_ctx_t *)ctx;
    if (!oc || !request || !callback || !out)
        return HU_ERR_INVALID_ARGUMENT;
    (void)model;
    (void)model_len;
    (void)temperature;

    memset(out, 0, sizeof(*out));

#if HU_IS_TEST
    const char *chunks[] = {"Hello ", "from ", "mock"};
    for (int i = 0; i < 3; i++) {
        hu_stream_chunk_t c = {
            .delta = chunks[i],
            .delta_len = strlen(chunks[i]),
            .is_final = false,
            .token_count = 1,
        };
        callback(callback_ctx, &c);
    }
    {
        hu_stream_chunk_t c = {.delta = NULL, .delta_len = 0, .is_final = true, .token_count = 3};
        callback(callback_ctx, &c);
    }
    out->content = hu_strndup(alloc, "Hello from mock", 15);
    out->content_len = 15;
    out->usage.completion_tokens = 3;
    return HU_OK;
#else
    hu_json_value_t *root = hu_json_object_new(alloc);
    if (!root)
        return HU_ERR_OUT_OF_MEMORY;

    hu_json_value_t *msgs_arr = hu_json_array_new(alloc);
    if (!msgs_arr) {
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    (void)hu_json_object_set(alloc, root, "messages", msgs_arr);

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
        hu_json_array_push(alloc, msgs_arr, obj);
    }

    hu_json_object_set(alloc, root, "model", hu_json_string_new(alloc, model, model_len));
    hu_json_object_set(alloc, root, "temperature", hu_json_number_new(alloc, temperature));
    hu_json_object_set(alloc, root, "stream", hu_json_bool_new(alloc, true));

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

#if defined(HU_GATEWAY_POSIX)
    if (oc->ws_streaming) {
        hu_error_t ws_err =
            openai_stream_ws(oc, alloc, body, body_len, callback, callback_ctx, out);
        if (ws_err == HU_OK) {
            alloc->free(alloc->ctx, body, body_len);
            return HU_OK;
        }
    }
#endif

    const char *url = oc->base_url ? oc->base_url : HU_OPENAI_URL;
    char url_buf[512];
    size_t url_len = oc->base_url_len ? oc->base_url_len : HU_OPENAI_URL_LEN;
    if (url_len >= sizeof(url_buf)) {
        alloc->free(alloc->ctx, body, body_len);
        return HU_ERR_INVALID_ARGUMENT;
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
        .last_error = HU_OK,
        .content_buf = NULL,
        .content_len = 0,
        .content_cap = 0,
    };
    err = hu_sse_parser_init(&sctx.parser, alloc);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, body, body_len);
        return err;
    }
    err = hu_http_post_json_stream(alloc, url_buf, auth_len ? auth_buf : NULL, NULL, body, body_len,
                                   openai_stream_write_cb, &sctx);
    hu_sse_parser_deinit(&sctx.parser);
    alloc->free(alloc->ctx, body, body_len);
    if (err != HU_OK)
        return err;
    if (sctx.last_error != HU_OK) {
        if (sctx.content_buf)
            alloc->free(alloc->ctx, sctx.content_buf, sctx.content_cap);
        return sctx.last_error;
    }

    if (sctx.content_buf && sctx.content_len > 0) {
        out->content = hu_strndup(alloc, sctx.content_buf, sctx.content_len);
        out->content_len = sctx.content_len;
        alloc->free(alloc->ctx, sctx.content_buf, sctx.content_cap);
    }

    return HU_OK;
#endif
}

static const char *openai_get_name(void *ctx) {
    (void)ctx;
    return "openai";
}

static void openai_deinit(void *ctx, hu_allocator_t *alloc) {
    hu_openai_ctx_t *oc = (hu_openai_ctx_t *)ctx;
    if (!oc || !alloc)
        return;
    if (oc->api_key)
        alloc->free(alloc->ctx, oc->api_key, oc->api_key_len + 1);
    if (oc->base_url)
        alloc->free(alloc->ctx, oc->base_url, oc->base_url_len + 1);
    alloc->free(alloc->ctx, oc, sizeof(*oc));
}

static const hu_provider_vtable_t openai_vtable = {
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

hu_error_t hu_openai_create(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                            const char *base_url, size_t base_url_len, hu_provider_t *out) {
    (void)alloc;
    hu_openai_ctx_t *oc = (hu_openai_ctx_t *)calloc(1, sizeof(*oc));
    if (!oc)
        return HU_ERR_OUT_OF_MEMORY;

    if (api_key && api_key_len > 0) {
        oc->api_key = (char *)malloc(api_key_len + 1);
        if (!oc->api_key) {
            free(oc);
            return HU_ERR_OUT_OF_MEMORY;
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
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(oc->base_url, base_url, base_url_len);
        oc->base_url[base_url_len] = '\0';
        oc->base_url_len = base_url_len;
    }

    out->ctx = oc;
    out->vtable = &openai_vtable;
    return HU_OK;
}

void hu_openai_set_ws_streaming(hu_provider_t *p, bool enabled) {
    if (!p || !p->ctx)
        return;
    hu_openai_ctx_t *oc = (hu_openai_ctx_t *)p->ctx;
    oc->ws_streaming = enabled;
}
