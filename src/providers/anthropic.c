#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/provider.h"
#include "human/providers/provider_http.h"
#include "human/providers/sse.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_ANTHROPIC_URL                "https://api.anthropic.com/v1/messages"
#define HU_ANTHROPIC_DEFAULT_MAX_TOKENS 4096

typedef struct hu_anthropic_ctx {
    char *api_key;
    size_t api_key_len;
    char *base_url;
    size_t base_url_len;
} hu_anthropic_ctx_t;

static hu_error_t anthropic_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                                 const char *model, size_t model_len, double temperature,
                                 hu_chat_response_t *out);

static hu_error_t anthropic_chat_with_system(void *ctx, hu_allocator_t *alloc,
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
    hu_error_t err = anthropic_chat(ctx, alloc, &req, model, model_len, temperature, &resp);
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

static hu_error_t anthropic_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                                 const char *model, size_t model_len, double temperature,
                                 hu_chat_response_t *out) {
    hu_anthropic_ctx_t *ac = (hu_anthropic_ctx_t *)ctx;
    if (!ac || !request || !out)
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
            tcs[0].id = hu_strndup(alloc, "toolu_mock", 10);
            tcs[0].id_len = 10;
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
        const char *content = "Hello from mock Anthropic";
        size_t len = strlen(content);
        out->content = hu_strndup(alloc, content, len);
        out->content_len = len;
        out->usage.prompt_tokens = 10;
        out->usage.completion_tokens = 5;
        out->usage.total_tokens = 15;
    }
    return HU_OK;
#else
    if (!ac->api_key || ac->api_key_len == 0)
        return HU_ERR_PROVIDER_AUTH;

    hu_json_value_t *root = hu_json_object_new(alloc);
    if (!root)
        return HU_ERR_OUT_OF_MEMORY;

    hu_json_value_t *model_val = hu_json_string_new(alloc, model, model_len);
    hu_json_object_set(alloc, root, "model", model_val);

    uint32_t max_tokens =
        request->max_tokens ? request->max_tokens : HU_ANTHROPIC_DEFAULT_MAX_TOKENS;
    hu_json_object_set(alloc, root, "max_tokens", hu_json_number_new(alloc, (double)max_tokens));
    hu_json_object_set(alloc, root, "temperature", hu_json_number_new(alloc, temperature));

    const char *system_prompt = NULL;
    size_t system_len = 0;
    for (size_t i = 0; i < request->messages_count; i++) {
        if (request->messages[i].role == HU_ROLE_SYSTEM) {
            system_prompt = request->messages[i].content;
            system_len = request->messages[i].content_len;
            break;
        }
    }
    if (system_prompt && system_len > 0) {
        hu_json_value_t *sys_val = hu_json_string_new(alloc, system_prompt, system_len);
        hu_json_object_set(alloc, root, "system", sys_val);
    }

    hu_json_value_t *msgs_arr = hu_json_array_new(alloc);
    if (!msgs_arr) {
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    hu_json_object_set(alloc, root, "messages", msgs_arr);

    for (size_t i = 0; i < request->messages_count; i++) {
        const hu_chat_message_t *m = &request->messages[i];
        if (m->role == HU_ROLE_SYSTEM)
            continue;

        if (m->role == HU_ROLE_TOOL) {
            /* Coalesce consecutive tool messages into one user message with tool_result blocks */
            hu_json_value_t *content_arr = hu_json_array_new(alloc);
            if (!content_arr) {
                hu_json_free(alloc, root);
                return HU_ERR_OUT_OF_MEMORY;
            }
            size_t j = i;
            while (j < request->messages_count && request->messages[j].role == HU_ROLE_TOOL) {
                const hu_chat_message_t *tm = &request->messages[j];
                if (tm->tool_call_id && tm->content) {
                    hu_json_value_t *tr = hu_json_object_new(alloc);
                    if (tr) {
                        hu_json_object_set(alloc, tr, "type",
                                           hu_json_string_new(alloc, "tool_result", 11));
                        hu_json_object_set(
                            alloc, tr, "tool_use_id",
                            hu_json_string_new(alloc, tm->tool_call_id, tm->tool_call_id_len));
                        hu_json_object_set(alloc, tr, "content",
                                           hu_json_string_new(alloc, tm->content, tm->content_len));
                        hu_json_array_push(alloc, content_arr, tr);
                    }
                }
                j++;
            }
            hu_json_value_t *obj = hu_json_object_new(alloc);
            if (!obj) {
                hu_json_free(alloc, content_arr);
                hu_json_free(alloc, root);
                return HU_ERR_OUT_OF_MEMORY;
            }
            hu_json_object_set(alloc, obj, "role", hu_json_string_new(alloc, "user", 4));
            hu_json_object_set(alloc, obj, "content", content_arr);
            hu_json_array_push(alloc, msgs_arr, obj);
            i = j - 1;
            continue;
        }

        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj) {
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }

        const char *role_str = (m->role == HU_ROLE_ASSISTANT) ? "assistant" : "user";
        hu_json_object_set(alloc, obj, "role",
                           hu_json_string_new(alloc, role_str, strlen(role_str)));
        if (m->role == HU_ROLE_ASSISTANT && m->tool_calls && m->tool_calls_count > 0) {
            hu_json_value_t *content_arr = hu_json_array_new(alloc);
            if (content_arr) {
                for (size_t k = 0; k < m->tool_calls_count; k++) {
                    const hu_tool_call_t *tc = &m->tool_calls[k];
                    hu_json_value_t *tu = hu_json_object_new(alloc);
                    if (!tu) {
                        hu_json_free(alloc, content_arr);
                        hu_json_free(alloc, root);
                        return HU_ERR_OUT_OF_MEMORY;
                    }
                    hu_json_object_set(alloc, tu, "type", hu_json_string_new(alloc, "tool_use", 8));
                    if (tc->id && tc->id_len > 0)
                        hu_json_object_set(alloc, tu, "id",
                                           hu_json_string_new(alloc, tc->id, tc->id_len));
                    if (tc->name && tc->name_len > 0)
                        hu_json_object_set(alloc, tu, "name",
                                           hu_json_string_new(alloc, tc->name, tc->name_len));
                    if (tc->arguments && tc->arguments_len > 0) {
                        hu_json_value_t *input_val = NULL;
                        if (hu_json_parse(alloc, tc->arguments, tc->arguments_len, &input_val) ==
                            HU_OK)
                            hu_json_object_set(alloc, tu, "input", input_val);
                    }
                    hu_json_array_push(alloc, content_arr, tu);
                }
                hu_json_object_set(alloc, obj, "content", content_arr);
            }
        } else if (m->content_parts && m->content_parts_count > 0) {
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
                    } else if (cp->tag == HU_CONTENT_PART_IMAGE_BASE64) {
                        hu_json_object_set(alloc, part, "type",
                                           hu_json_string_new(alloc, "image", 5));
                        hu_json_value_t *src_obj = hu_json_object_new(alloc);
                        if (src_obj) {
                            hu_json_object_set(alloc, src_obj, "type",
                                               hu_json_string_new(alloc, "base64", 6));
                            hu_json_object_set(
                                alloc, src_obj, "media_type",
                                hu_json_string_new(alloc, cp->data.image_base64.media_type,
                                                   cp->data.image_base64.media_type_len));
                            hu_json_object_set(alloc, src_obj, "data",
                                               hu_json_string_new(alloc, cp->data.image_base64.data,
                                                                  cp->data.image_base64.data_len));
                            hu_json_object_set(alloc, part, "source", src_obj);
                        }
                    } else if (cp->tag == HU_CONTENT_PART_IMAGE_URL) {
                        hu_json_object_set(alloc, part, "type",
                                           hu_json_string_new(alloc, "image", 5));
                        hu_json_value_t *src_obj = hu_json_object_new(alloc);
                        if (src_obj) {
                            hu_json_object_set(alloc, src_obj, "type",
                                               hu_json_string_new(alloc, "url", 3));
                            hu_json_object_set(alloc, src_obj, "url",
                                               hu_json_string_new(alloc, cp->data.image_url.url,
                                                                  cp->data.image_url.url_len));
                            hu_json_object_set(alloc, part, "source", src_obj);
                        }
                    } else if (cp->tag == HU_CONTENT_PART_AUDIO_BASE64 ||
                               cp->tag == HU_CONTENT_PART_VIDEO_URL) {
                        /* Anthropic does not support audio/video in content; skip */
                        hu_json_free(alloc, part);
                        continue;
                    }
                    hu_json_array_push(alloc, parts_arr, part);
                }
                hu_json_object_set(alloc, obj, "content", parts_arr);
            }
        } else if (m->content && m->content_len > 0) {
            hu_json_value_t *content_val = hu_json_string_new(alloc, m->content, m->content_len);
            hu_json_object_set(alloc, obj, "content", content_val);
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
                hu_json_object_set(
                    alloc, tool_obj, "name",
                    hu_json_string_new(alloc, request->tools[i].name, request->tools[i].name_len));
                hu_json_object_set(
                    alloc, tool_obj, "description",
                    hu_json_string_new(
                        alloc, request->tools[i].description ? request->tools[i].description : "",
                        request->tools[i].description_len));
                if (request->tools[i].parameters_json &&
                    request->tools[i].parameters_json_len > 0) {
                    hu_json_value_t *schema = NULL;
                    if (hu_json_parse(alloc, request->tools[i].parameters_json,
                                      request->tools[i].parameters_json_len, &schema) == HU_OK)
                        hu_json_object_set(alloc, tool_obj, "input_schema", schema);
                }
                hu_json_array_push(alloc, tools_arr, tool_obj);
            }
            hu_json_object_set(alloc, root, "tools", tools_arr);
        }
    }

    if (request->response_format && request->response_format_len > 0) {
        if ((request->response_format_len >= 11 &&
             memcmp(request->response_format, "json_object", 11) == 0) ||
            (request->response_format_len >= 4 &&
             memcmp(request->response_format, "json", 4) == 0)) {
            hu_json_value_t *rf_obj = hu_json_object_new(alloc);
            if (rf_obj) {
                hu_json_object_set(alloc, rf_obj, "type", hu_json_string_new(alloc, "json", 4));
                hu_json_object_set(alloc, root, "response_format", rf_obj);
            }
        }
    }

    char *body = NULL;
    size_t body_len = 0;
    hu_error_t err = hu_json_stringify(alloc, root, &body, &body_len);
    hu_json_free(alloc, root);
    if (err != HU_OK)
        return err;

    const char *base =
        ac->base_url && ac->base_url_len > 0 ? ac->base_url : "https://api.anthropic.com/v1";
    size_t base_len = ac->base_url_len ? ac->base_url_len : 29;
    char url_buf[512];
    int n = snprintf(url_buf, sizeof(url_buf), "%.*s/messages", (int)base_len, base);
    if (n <= 0 || (size_t)n >= sizeof(url_buf)) {
        alloc->free(alloc->ctx, body, body_len);
        return HU_ERR_INVALID_ARGUMENT;
    }

    char extra_buf[512];
    n = snprintf(extra_buf, sizeof(extra_buf), "x-api-key: %.*s\r\nanthropic-version: 2023-06-01",
                 (int)ac->api_key_len, ac->api_key);
    if (n <= 0 || (size_t)n >= sizeof(extra_buf)) {
        alloc->free(alloc->ctx, body, body_len);
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_json_value_t *parsed = NULL;
    err = hu_provider_http_post_json(alloc, url_buf, NULL, extra_buf, body, body_len, &parsed);
    alloc->free(alloc->ctx, body, body_len);
    if (err != HU_OK)
        return err;

    memset(out, 0, sizeof(*out));
    hu_json_value_t *content_arr = hu_json_object_get(parsed, "content");
    if (content_arr && content_arr->type == HU_JSON_ARRAY) {
        size_t block_count = content_arr->data.array.len;
        hu_tool_call_t *tcs = NULL;
        size_t tc_valid = 0;
        for (size_t b = 0; b < block_count; b++) {
            hu_json_value_t *block = content_arr->data.array.items[b];
            const char *block_type = hu_json_get_string(block, "type");
            if (block_type && strcmp(block_type, "text") == 0) {
                const char *text = hu_json_get_string(block, "text");
                if (text && !out->content) {
                    size_t tlen = strlen(text);
                    out->content = hu_strndup(alloc, text, tlen);
                    out->content_len = tlen;
                }
            } else if (block_type && strcmp(block_type, "tool_use") == 0) {
                const char *tname = hu_json_get_string(block, "name");
                if (!tname)
                    continue;
                if (!tcs) {
                    tcs = (hu_tool_call_t *)alloc->alloc(alloc->ctx,
                                                         block_count * sizeof(hu_tool_call_t));
                    if (!tcs)
                        break;
                    memset(tcs, 0, block_count * sizeof(hu_tool_call_t));
                }
                const char *tid = hu_json_get_string(block, "id");
                hu_json_value_t *input = hu_json_object_get(block, "input");
                char *args_str = NULL;
                size_t args_len = 0;
                if (input && input->type == HU_JSON_OBJECT) {
                    hu_json_stringify(alloc, input, &args_str, &args_len);
                }
                tcs[tc_valid].id = tid ? hu_strndup(alloc, tid, strlen(tid)) : NULL;
                tcs[tc_valid].id_len = tid ? (size_t)strlen(tid) : 0;
                tcs[tc_valid].name = hu_strndup(alloc, tname, strlen(tname));
                tcs[tc_valid].name_len = (size_t)strlen(tname);
                tcs[tc_valid].arguments = args_str;
                tcs[tc_valid].arguments_len = args_len;
                tc_valid++;
            }
        }
        if (tcs && tc_valid > 0) {
            out->tool_calls = tcs;
            out->tool_calls_count = tc_valid;
        }
    }
    hu_json_value_t *usage = hu_json_object_get(parsed, "usage");
    if (usage && usage->type == HU_JSON_OBJECT) {
        out->usage.prompt_tokens = (uint32_t)hu_json_get_number(usage, "input_tokens", 0);
        out->usage.completion_tokens = (uint32_t)hu_json_get_number(usage, "output_tokens", 0);
        out->usage.total_tokens = out->usage.prompt_tokens + out->usage.completion_tokens;
    }
    hu_json_free(alloc, parsed);
    return HU_OK;
#endif
}

static bool anthropic_supports_native_tools(void *ctx) {
    (void)ctx;
    return true;
}

static bool anthropic_supports_streaming(void *ctx) {
    (void)ctx;
    return true;
}

#if !HU_IS_TEST
typedef struct anthropic_stream_ctx {
    hu_allocator_t *alloc;
    hu_stream_callback_t callback;
    void *callback_ctx;
    hu_sse_parser_t parser;
    hu_error_t last_error;
    char *content_buf;
    size_t content_len;
    size_t content_cap;
} anthropic_stream_ctx_t;

static bool event_eq(const char *event_type, size_t len, const char *expected) {
    size_t elen = strlen(expected);
    return len == elen && memcmp(event_type, expected, len) == 0;
}

static void append_content_anthropic(anthropic_stream_ctx_t *s, const char *delta,
                                     size_t delta_len) {
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

static void extract_anthropic_delta(anthropic_stream_ctx_t *s, const char *json_str,
                                    size_t json_len) {
    hu_json_value_t *parsed = NULL;
    if (hu_json_parse(s->alloc, json_str, json_len, &parsed) != HU_OK)
        return;
    const char *type = hu_json_get_string(parsed, "type");
    if (!type || strcmp(type, "content_block_delta") != 0) {
        hu_json_free(s->alloc, parsed);
        return;
    }
    hu_json_value_t *delta = hu_json_object_get(parsed, "delta");
    if (!delta || delta->type != HU_JSON_OBJECT) {
        hu_json_free(s->alloc, parsed);
        return;
    }
    const char *text = hu_json_get_string(delta, "text");
    if (text) {
        size_t tlen = strlen(text);
        if (tlen > 0) {
            append_content_anthropic(s, text, tlen);
            hu_stream_chunk_t chunk = {
                .delta = text,
                .delta_len = tlen,
                .is_final = false,
                .token_count = 0,
            };
            s->callback(s->callback_ctx, &chunk);
        }
    }
    hu_json_free(s->alloc, parsed);
}

static void anthropic_sse_event_cb(const char *event_type, size_t event_type_len, const char *data,
                                   size_t data_len, void *userdata) {
    anthropic_stream_ctx_t *s = (anthropic_stream_ctx_t *)userdata;
    if (s->last_error != HU_OK)
        return;
    if (event_eq(event_type, event_type_len, "message_stop")) {
        hu_stream_chunk_t chunk = {
            .delta = NULL, .delta_len = 0, .is_final = true, .token_count = 0};
        s->callback(s->callback_ctx, &chunk);
        return;
    }
    if (event_eq(event_type, event_type_len, "content_block_delta") && data && data_len > 0) {
        extract_anthropic_delta(s, data, data_len);
    }
}

static size_t anthropic_stream_write_cb(const char *data, size_t len, void *userdata) {
    anthropic_stream_ctx_t *s = (anthropic_stream_ctx_t *)userdata;
    hu_error_t err = hu_sse_parser_feed(&s->parser, data, len, anthropic_sse_event_cb, s);
    if (err != HU_OK) {
        s->last_error = err;
        return 0;
    }
    return len;
}
#endif /* !HU_IS_TEST */

static hu_error_t anthropic_stream_chat(void *ctx, hu_allocator_t *alloc,
                                        const hu_chat_request_t *request, const char *model,
                                        size_t model_len, double temperature,
                                        hu_stream_callback_t callback, void *callback_ctx,
                                        hu_stream_chat_result_t *out) {
    hu_anthropic_ctx_t *ac = (hu_anthropic_ctx_t *)ctx;
    if (!ac || !request || !callback || !out)
        return HU_ERR_INVALID_ARGUMENT;
    (void)model;
    (void)model_len;
    (void)temperature;

    memset(out, 0, sizeof(*out));

#if HU_IS_TEST
    const char *chunks[] = {"Hello ", "from ", "Anthropic"};
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
    out->content = hu_strndup(alloc, "Hello from Anthropic", 19);
    out->content_len = 19;
    out->usage.completion_tokens = 3;
    return HU_OK;
#else
    if (!ac->api_key || ac->api_key_len == 0)
        return HU_ERR_PROVIDER_AUTH;

    hu_json_value_t *root = hu_json_object_new(alloc);
    if (!root)
        return HU_ERR_OUT_OF_MEMORY;

    hu_json_object_set(alloc, root, "model", hu_json_string_new(alloc, model, model_len));
    uint32_t max_tokens =
        request->max_tokens ? request->max_tokens : HU_ANTHROPIC_DEFAULT_MAX_TOKENS;
    hu_json_object_set(alloc, root, "max_tokens", hu_json_number_new(alloc, (double)max_tokens));
    hu_json_object_set(alloc, root, "temperature", hu_json_number_new(alloc, temperature));
    hu_json_object_set(alloc, root, "stream", hu_json_bool_new(alloc, true));

    const char *system_prompt = NULL;
    size_t system_len = 0;
    for (size_t i = 0; i < request->messages_count; i++) {
        if (request->messages[i].role == HU_ROLE_SYSTEM) {
            system_prompt = request->messages[i].content;
            system_len = request->messages[i].content_len;
            break;
        }
    }
    if (system_prompt && system_len > 0) {
        hu_json_object_set(alloc, root, "system",
                           hu_json_string_new(alloc, system_prompt, system_len));
    }

    hu_json_value_t *msgs_arr = hu_json_array_new(alloc);
    if (!msgs_arr) {
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    hu_json_object_set(alloc, root, "messages", msgs_arr);
    for (size_t i = 0; i < request->messages_count; i++) {
        const hu_chat_message_t *m = &request->messages[i];
        if (m->role == HU_ROLE_SYSTEM)
            continue;
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj) {
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
        const char *role_str = (m->role == HU_ROLE_ASSISTANT) ? "assistant" : "user";
        hu_json_object_set(alloc, obj, "role",
                           hu_json_string_new(alloc, role_str, strlen(role_str)));
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
                    } else if (cp->tag == HU_CONTENT_PART_IMAGE_BASE64) {
                        hu_json_object_set(alloc, part, "type",
                                           hu_json_string_new(alloc, "image", 5));
                        hu_json_value_t *src_obj = hu_json_object_new(alloc);
                        if (src_obj) {
                            hu_json_object_set(alloc, src_obj, "type",
                                               hu_json_string_new(alloc, "base64", 6));
                            hu_json_object_set(
                                alloc, src_obj, "media_type",
                                hu_json_string_new(alloc, cp->data.image_base64.media_type,
                                                   cp->data.image_base64.media_type_len));
                            hu_json_object_set(alloc, src_obj, "data",
                                               hu_json_string_new(alloc, cp->data.image_base64.data,
                                                                  cp->data.image_base64.data_len));
                            hu_json_object_set(alloc, part, "source", src_obj);
                        }
                    } else if (cp->tag == HU_CONTENT_PART_IMAGE_URL) {
                        hu_json_object_set(alloc, part, "type",
                                           hu_json_string_new(alloc, "image", 5));
                        hu_json_value_t *src_obj = hu_json_object_new(alloc);
                        if (src_obj) {
                            hu_json_object_set(alloc, src_obj, "type",
                                               hu_json_string_new(alloc, "url", 3));
                            hu_json_object_set(alloc, src_obj, "url",
                                               hu_json_string_new(alloc, cp->data.image_url.url,
                                                                  cp->data.image_url.url_len));
                            hu_json_object_set(alloc, part, "source", src_obj);
                        }
                    } else if (cp->tag == HU_CONTENT_PART_AUDIO_BASE64 ||
                               cp->tag == HU_CONTENT_PART_VIDEO_URL) {
                        /* Anthropic does not support audio/video in content; skip */
                        hu_json_free(alloc, part);
                        continue;
                    }
                    hu_json_array_push(alloc, parts_arr, part);
                }
                hu_json_object_set(alloc, obj, "content", parts_arr);
            }
        } else if (m->content && m->content_len > 0) {
            hu_json_object_set(alloc, obj, "content",
                               hu_json_string_new(alloc, m->content, m->content_len));
        }
        hu_json_array_push(alloc, msgs_arr, obj);
    }

    if (request->response_format && request->response_format_len > 0) {
        if ((request->response_format_len >= 11 &&
             memcmp(request->response_format, "json_object", 11) == 0) ||
            (request->response_format_len >= 4 &&
             memcmp(request->response_format, "json", 4) == 0)) {
            hu_json_value_t *rf_obj = hu_json_object_new(alloc);
            if (rf_obj) {
                hu_json_object_set(alloc, rf_obj, "type", hu_json_string_new(alloc, "json", 4));
                hu_json_object_set(alloc, root, "response_format", rf_obj);
            }
        }
    }

    char *body = NULL;
    size_t body_len = 0;
    hu_error_t err = hu_json_stringify(alloc, root, &body, &body_len);
    hu_json_free(alloc, root);
    if (err != HU_OK)
        return err;

    const char *base =
        ac->base_url && ac->base_url_len > 0 ? ac->base_url : "https://api.anthropic.com/v1";
    size_t base_len = ac->base_url_len ? ac->base_url_len : 29;
    char url_buf[512];
    int n = snprintf(url_buf, sizeof(url_buf), "%.*s/messages", (int)base_len, base);
    if (n <= 0 || (size_t)n >= sizeof(url_buf)) {
        alloc->free(alloc->ctx, body, body_len);
        return HU_ERR_INVALID_ARGUMENT;
    }

    char extra_buf[512];
    n = snprintf(extra_buf, sizeof(extra_buf),
                 "x-api-key: %.*s\r\nanthropic-version: 2023-06-01\r\nAccept: text/event-stream",
                 (int)ac->api_key_len, ac->api_key);
    if (n <= 0 || (size_t)n >= sizeof(extra_buf)) {
        alloc->free(alloc->ctx, body, body_len);
        return HU_ERR_INVALID_ARGUMENT;
    }

    anthropic_stream_ctx_t sctx = {
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
    err = hu_http_post_json_stream(alloc, url_buf, NULL, extra_buf, body, body_len,
                                   anthropic_stream_write_cb, &sctx);
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

static const char *anthropic_get_name(void *ctx) {
    (void)ctx;
    return "anthropic";
}

static void anthropic_deinit(void *ctx, hu_allocator_t *alloc) {
    hu_anthropic_ctx_t *ac = (hu_anthropic_ctx_t *)ctx;
    if (!ac || !alloc)
        return;
    if (ac->api_key)
        alloc->free(alloc->ctx, ac->api_key, ac->api_key_len + 1);
    if (ac->base_url)
        alloc->free(alloc->ctx, ac->base_url, ac->base_url_len + 1);
    alloc->free(alloc->ctx, ac, sizeof(*ac));
}

static const hu_provider_vtable_t anthropic_vtable = {
    .chat_with_system = anthropic_chat_with_system,
    .chat = anthropic_chat,
    .supports_native_tools = anthropic_supports_native_tools,
    .get_name = anthropic_get_name,
    .deinit = anthropic_deinit,
    .warmup = NULL,
    .chat_with_tools = NULL,
    .supports_streaming = anthropic_supports_streaming,
    .supports_vision = NULL,
    .supports_vision_for_model = NULL,
    .stream_chat = anthropic_stream_chat,
};

hu_error_t hu_anthropic_create(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                               const char *base_url, size_t base_url_len, hu_provider_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_anthropic_ctx_t *ac = (hu_anthropic_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*ac));
    if (!ac)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ac, 0, sizeof(*ac));
    if (api_key && api_key_len > 0) {
        ac->api_key = (char *)alloc->alloc(alloc->ctx, api_key_len + 1);
        if (!ac->api_key) {
            alloc->free(alloc->ctx, ac, sizeof(*ac));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(ac->api_key, api_key, api_key_len);
        ac->api_key[api_key_len] = '\0';
        ac->api_key_len = api_key_len;
    }
    if (base_url && base_url_len > 0) {
        ac->base_url = (char *)alloc->alloc(alloc->ctx, base_url_len + 1);
        if (!ac->base_url) {
            if (ac->api_key)
                alloc->free(alloc->ctx, ac->api_key, ac->api_key_len + 1);
            alloc->free(alloc->ctx, ac, sizeof(*ac));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(ac->base_url, base_url, base_url_len);
        ac->base_url[base_url_len] = '\0';
        ac->base_url_len = base_url_len;
    }
    out->ctx = ac;
    out->vtable = &anthropic_vtable;
    return HU_OK;
}
