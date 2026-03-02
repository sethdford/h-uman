#include "seaclaw/provider.h"
#include <stdint.h>
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/core/http.h"
#include "seaclaw/providers/sse.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define SC_ANTHROPIC_URL "https://api.anthropic.com/v1/messages"
#define SC_ANTHROPIC_DEFAULT_MAX_TOKENS 4096

typedef struct sc_anthropic_ctx {
    char *api_key;
    size_t api_key_len;
    char *base_url;
    size_t base_url_len;
} sc_anthropic_ctx_t;

static sc_error_t anthropic_chat(void *ctx, sc_allocator_t *alloc,
    const sc_chat_request_t *request,
    const char *model, size_t model_len,
    double temperature,
    sc_chat_response_t *out);

static sc_error_t anthropic_chat_with_system(void *ctx, sc_allocator_t *alloc,
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
    sc_error_t err = anthropic_chat(ctx, alloc, &req, model, model_len, temperature, &resp);
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

static sc_error_t anthropic_chat(void *ctx, sc_allocator_t *alloc,
    const sc_chat_request_t *request,
    const char *model, size_t model_len,
    double temperature,
    sc_chat_response_t *out)
{
    sc_anthropic_ctx_t *ac = (sc_anthropic_ctx_t *)ctx;
    if (!ac || !request || !out) return SC_ERR_INVALID_ARGUMENT;
    (void)model; (void)model_len; (void)temperature;

#if SC_IS_TEST
    memset(out, 0, sizeof(*out));
    if (request->tools && request->tools_count > 0) {
        out->content = NULL;
        out->content_len = 0;
        sc_tool_call_t *tcs = (sc_tool_call_t *)alloc->alloc(alloc->ctx, sizeof(sc_tool_call_t));
        if (tcs) {
            memset(tcs, 0, sizeof(sc_tool_call_t));
            tcs[0].id = sc_strndup(alloc, "toolu_mock", 10);
            tcs[0].id_len = 10;
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
        const char *content = "Hello from mock Anthropic";
        size_t len = strlen(content);
        out->content = sc_strndup(alloc, content, len);
        out->content_len = len;
        out->usage.prompt_tokens = 10;
        out->usage.completion_tokens = 5;
        out->usage.total_tokens = 15;
    }
    return SC_OK;
#else
    if (!ac->api_key || ac->api_key_len == 0)
        return SC_ERR_PROVIDER_AUTH;

    sc_json_value_t *root = sc_json_object_new(alloc);
    if (!root) return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *model_val = sc_json_string_new(alloc, model, model_len);
    sc_json_object_set(alloc, root, "model", model_val);

    uint32_t max_tokens = request->max_tokens ? request->max_tokens : SC_ANTHROPIC_DEFAULT_MAX_TOKENS;
    sc_json_object_set(alloc, root, "max_tokens", sc_json_number_new(alloc, (double)max_tokens));
    sc_json_object_set(alloc, root, "temperature", sc_json_number_new(alloc, temperature));

    const char *system_prompt = NULL;
    size_t system_len = 0;
    for (size_t i = 0; i < request->messages_count; i++) {
        if (request->messages[i].role == SC_ROLE_SYSTEM) {
            system_prompt = request->messages[i].content;
            system_len = request->messages[i].content_len;
            break;
        }
    }
    if (system_prompt && system_len > 0) {
        sc_json_value_t *sys_val = sc_json_string_new(alloc, system_prompt, system_len);
        sc_json_object_set(alloc, root, "system", sys_val);
    }

    sc_json_value_t *msgs_arr = sc_json_array_new(alloc);
    if (!msgs_arr) { sc_json_free(alloc, root); return SC_ERR_OUT_OF_MEMORY; }
    sc_json_object_set(alloc, root, "messages", msgs_arr);

    for (size_t i = 0; i < request->messages_count; i++) {
        const sc_chat_message_t *m = &request->messages[i];
        if (m->role == SC_ROLE_SYSTEM) continue;

        if (m->role == SC_ROLE_TOOL) {
            /* Coalesce consecutive tool messages into one user message with tool_result blocks */
            sc_json_value_t *content_arr = sc_json_array_new(alloc);
            if (!content_arr) { sc_json_free(alloc, root); return SC_ERR_OUT_OF_MEMORY; }
            size_t j = i;
            while (j < request->messages_count && request->messages[j].role == SC_ROLE_TOOL) {
                const sc_chat_message_t *tm = &request->messages[j];
                if (tm->tool_call_id && tm->content) {
                    sc_json_value_t *tr = sc_json_object_new(alloc);
                    if (tr) {
                        sc_json_object_set(alloc, tr, "type", sc_json_string_new(alloc, "tool_result", 11));
                        sc_json_object_set(alloc, tr, "tool_use_id", sc_json_string_new(alloc, tm->tool_call_id, tm->tool_call_id_len));
                        sc_json_object_set(alloc, tr, "content", sc_json_string_new(alloc, tm->content, tm->content_len));
                        sc_json_array_push(alloc, content_arr, tr);
                    }
                }
                j++;
            }
            sc_json_value_t *obj = sc_json_object_new(alloc);
            if (!obj) { sc_json_free(alloc, content_arr); sc_json_free(alloc, root); return SC_ERR_OUT_OF_MEMORY; }
            sc_json_object_set(alloc, obj, "role", sc_json_string_new(alloc, "user", 4));
            sc_json_object_set(alloc, obj, "content", content_arr);
            sc_json_array_push(alloc, msgs_arr, obj);
            i = j - 1;
            continue;
        }

        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) { sc_json_free(alloc, root); return SC_ERR_OUT_OF_MEMORY; }

        const char *role_str = (m->role == SC_ROLE_ASSISTANT) ? "assistant" : "user";
        sc_json_object_set(alloc, obj, "role", sc_json_string_new(alloc, role_str, strlen(role_str)));
        if (m->role == SC_ROLE_ASSISTANT && m->tool_calls && m->tool_calls_count > 0) {
            sc_json_value_t *content_arr = sc_json_array_new(alloc);
            if (content_arr) {
                for (size_t k = 0; k < m->tool_calls_count; k++) {
                    const sc_tool_call_t *tc = &m->tool_calls[k];
                    sc_json_value_t *tu = sc_json_object_new(alloc);
                    if (!tu) { sc_json_free(alloc, content_arr); sc_json_free(alloc, root); return SC_ERR_OUT_OF_MEMORY; }
                    sc_json_object_set(alloc, tu, "type", sc_json_string_new(alloc, "tool_use", 8));
                    if (tc->id && tc->id_len > 0)
                        sc_json_object_set(alloc, tu, "id", sc_json_string_new(alloc, tc->id, tc->id_len));
                    if (tc->name && tc->name_len > 0)
                        sc_json_object_set(alloc, tu, "name", sc_json_string_new(alloc, tc->name, tc->name_len));
                    if (tc->arguments && tc->arguments_len > 0) {
                        sc_json_value_t *input_val = NULL;
                        if (sc_json_parse(alloc, tc->arguments, tc->arguments_len, &input_val) == SC_OK)
                            sc_json_object_set(alloc, tu, "input", input_val);
                    }
                    sc_json_array_push(alloc, content_arr, tu);
                }
                sc_json_object_set(alloc, obj, "content", content_arr);
            }
        } else if (m->content && m->content_len > 0) {
            sc_json_value_t *content_val = sc_json_string_new(alloc, m->content, m->content_len);
            sc_json_object_set(alloc, obj, "content", content_val);
        }
        sc_json_array_push(alloc, msgs_arr, obj);
    }
    if (request->tools && request->tools_count > 0) {
        sc_json_value_t *tools_arr = sc_json_array_new(alloc);
        if (tools_arr) {
            for (size_t i = 0; i < request->tools_count; i++) {
                sc_json_value_t *tool_obj = sc_json_object_new(alloc);
                if (!tool_obj) { sc_json_free(alloc, tools_arr); sc_json_free(alloc, root); return SC_ERR_OUT_OF_MEMORY; }
                sc_json_object_set(alloc, tool_obj, "name", sc_json_string_new(alloc, request->tools[i].name, request->tools[i].name_len));
                sc_json_object_set(alloc, tool_obj, "description", sc_json_string_new(alloc,
                    request->tools[i].description ? request->tools[i].description : "", request->tools[i].description_len));
                if (request->tools[i].parameters_json && request->tools[i].parameters_json_len > 0) {
                    sc_json_value_t *schema = NULL;
                    if (sc_json_parse(alloc, request->tools[i].parameters_json, request->tools[i].parameters_json_len, &schema) == SC_OK)
                        sc_json_object_set(alloc, tool_obj, "input_schema", schema);
                }
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

    const char *base = ac->base_url && ac->base_url_len > 0 ? ac->base_url : "https://api.anthropic.com/v1";
    size_t base_len = ac->base_url_len ? ac->base_url_len : 29;
    char url_buf[512];
    int n = snprintf(url_buf, sizeof(url_buf), "%.*s/messages", (int)base_len, base);
    if (n <= 0 || (size_t)n >= sizeof(url_buf)) {
        alloc->free(alloc->ctx, body, body_len);
        return SC_ERR_INVALID_ARGUMENT;
    }

    char extra_buf[512];
    n = snprintf(extra_buf, sizeof(extra_buf),
        "x-api-key: %.*s\r\nanthropic-version: 2023-06-01",
        (int)ac->api_key_len, ac->api_key);
    if (n <= 0 || (size_t)n >= sizeof(extra_buf)) {
        alloc->free(alloc->ctx, body, body_len);
        return SC_ERR_INVALID_ARGUMENT;
    }

    sc_http_response_t hresp = {0};
    err = sc_http_post_json_ex(alloc, url_buf, NULL, extra_buf, body, body_len, &hresp);
    alloc->free(alloc->ctx, body, body_len);
    if (err != SC_OK) return err;

    if (hresp.status_code < 200 || hresp.status_code >= 300) {
        sc_http_response_free(alloc, &hresp);
        if (hresp.status_code == 401) return SC_ERR_PROVIDER_AUTH;
        if (hresp.status_code == 429) return SC_ERR_PROVIDER_RATE_LIMITED;
        return SC_ERR_PROVIDER_RESPONSE;
    }

    sc_json_value_t *parsed = NULL;
    err = sc_json_parse(alloc, hresp.body, hresp.body_len, &parsed);
    sc_http_response_free(alloc, &hresp);
    if (err != SC_OK) return err;

    memset(out, 0, sizeof(*out));
    sc_json_value_t *content_arr = sc_json_object_get(parsed, "content");
    if (content_arr && content_arr->type == SC_JSON_ARRAY) {
        size_t block_count = content_arr->data.array.len;
        sc_tool_call_t *tcs = NULL;
        size_t tc_valid = 0;
        for (size_t b = 0; b < block_count; b++) {
            sc_json_value_t *block = content_arr->data.array.items[b];
            const char *block_type = sc_json_get_string(block, "type");
            if (block_type && strcmp(block_type, "text") == 0) {
                const char *text = sc_json_get_string(block, "text");
                if (text && !out->content) {
                    size_t tlen = strlen(text);
                    out->content = sc_strndup(alloc, text, tlen);
                    out->content_len = tlen;
                }
            } else if (block_type && strcmp(block_type, "tool_use") == 0) {
                const char *tname = sc_json_get_string(block, "name");
                if (!tname) continue;
                if (!tcs) {
                    tcs = (sc_tool_call_t *)alloc->alloc(alloc->ctx, block_count * sizeof(sc_tool_call_t));
                    if (!tcs) break;
                    memset(tcs, 0, block_count * sizeof(sc_tool_call_t));
                }
                const char *tid = sc_json_get_string(block, "id");
                sc_json_value_t *input = sc_json_object_get(block, "input");
                char *args_str = NULL;
                size_t args_len = 0;
                if (input && input->type == SC_JSON_OBJECT) {
                    sc_json_stringify(alloc, input, &args_str, &args_len);
                }
                tcs[tc_valid].id = tid ? sc_strndup(alloc, tid, strlen(tid)) : NULL;
                tcs[tc_valid].id_len = tid ? (size_t)strlen(tid) : 0;
                tcs[tc_valid].name = sc_strndup(alloc, tname, strlen(tname));
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
    sc_json_value_t *usage = sc_json_object_get(parsed, "usage");
    if (usage && usage->type == SC_JSON_OBJECT) {
        out->usage.prompt_tokens = (uint32_t)sc_json_get_number(usage, "input_tokens", 0);
        out->usage.completion_tokens = (uint32_t)sc_json_get_number(usage, "output_tokens", 0);
        out->usage.total_tokens = out->usage.prompt_tokens + out->usage.completion_tokens;
    }
    sc_json_free(alloc, parsed);
    return SC_OK;
#endif
}

static bool anthropic_supports_native_tools(void *ctx) { (void)ctx; return true; }

static bool anthropic_supports_streaming(void *ctx) { (void)ctx; return true; }

#if !SC_IS_TEST
typedef struct anthropic_stream_ctx {
    sc_allocator_t *alloc;
    sc_stream_callback_t callback;
    void *callback_ctx;
    sc_sse_parser_t parser;
    sc_error_t last_error;
    char *content_buf;
    size_t content_len;
    size_t content_cap;
} anthropic_stream_ctx_t;

static bool event_eq(const char *event_type, size_t len, const char *expected) {
    size_t elen = strlen(expected);
    return len == elen && memcmp(event_type, expected, len) == 0;
}

static void append_content_anthropic(anthropic_stream_ctx_t *s, const char *delta, size_t delta_len) {
    if (!delta || delta_len == 0) return;
    while (s->content_len + delta_len + 1 > s->content_cap) {
        size_t new_cap = s->content_cap ? s->content_cap * 2 : 256;
        while (new_cap < s->content_len + delta_len + 1) new_cap *= 2;
        char *n;
        if (s->content_buf) {
            n = (char *)s->alloc->realloc(s->alloc->ctx, s->content_buf, s->content_cap, new_cap);
        } else {
            n = (char *)s->alloc->alloc(s->alloc->ctx, new_cap);
        }
        if (!n) { s->last_error = SC_ERR_OUT_OF_MEMORY; return; }
        s->content_buf = n;
        s->content_cap = new_cap;
    }
    memcpy(s->content_buf + s->content_len, delta, delta_len);
    s->content_len += delta_len;
    s->content_buf[s->content_len] = '\0';
}

static void extract_anthropic_delta(anthropic_stream_ctx_t *s, const char *json_str, size_t json_len) {
    sc_json_value_t *parsed = NULL;
    if (sc_json_parse(s->alloc, json_str, json_len, &parsed) != SC_OK) return;
    const char *type = sc_json_get_string(parsed, "type");
    if (!type || strcmp(type, "content_block_delta") != 0) {
        sc_json_free(s->alloc, parsed);
        return;
    }
    sc_json_value_t *delta = sc_json_object_get(parsed, "delta");
    if (!delta || delta->type != SC_JSON_OBJECT) {
        sc_json_free(s->alloc, parsed);
        return;
    }
    const char *text = sc_json_get_string(delta, "text");
    if (text) {
        size_t tlen = strlen(text);
        if (tlen > 0) {
            append_content_anthropic(s, text, tlen);
            sc_stream_chunk_t chunk = {
                .delta = text,
                .delta_len = tlen,
                .is_final = false,
                .token_count = 0,
            };
            s->callback(s->callback_ctx, &chunk);
        }
    }
    sc_json_free(s->alloc, parsed);
}

static void anthropic_sse_event_cb(const char *event_type, size_t event_type_len,
    const char *data, size_t data_len, void *userdata)
{
    anthropic_stream_ctx_t *s = (anthropic_stream_ctx_t *)userdata;
    if (s->last_error != SC_OK) return;
    if (event_eq(event_type, event_type_len, "message_stop")) {
        sc_stream_chunk_t chunk = { .delta = NULL, .delta_len = 0, .is_final = true, .token_count = 0 };
        s->callback(s->callback_ctx, &chunk);
        return;
    }
    if (event_eq(event_type, event_type_len, "content_block_delta") && data && data_len > 0) {
        extract_anthropic_delta(s, data, data_len);
    }
}

static size_t anthropic_stream_write_cb(const char *data, size_t len, void *userdata) {
    anthropic_stream_ctx_t *s = (anthropic_stream_ctx_t *)userdata;
    sc_error_t err = sc_sse_parser_feed(&s->parser, data, len, anthropic_sse_event_cb, s);
    if (err != SC_OK) {
        s->last_error = err;
        return 0;
    }
    return len;
}
#endif /* !SC_IS_TEST */

static sc_error_t anthropic_stream_chat(void *ctx, sc_allocator_t *alloc,
    const sc_chat_request_t *request,
    const char *model, size_t model_len,
    double temperature,
    sc_stream_callback_t callback, void *callback_ctx,
    sc_stream_chat_result_t *out)
{
    sc_anthropic_ctx_t *ac = (sc_anthropic_ctx_t *)ctx;
    if (!ac || !request || !callback || !out) return SC_ERR_INVALID_ARGUMENT;
    (void)model; (void)model_len; (void)temperature;

    memset(out, 0, sizeof(*out));

#if SC_IS_TEST
    const char *chunks[] = { "Hello ", "from ", "Anthropic" };
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
        sc_stream_chunk_t c = { .delta = NULL, .delta_len = 0, .is_final = true, .token_count = 3 };
        callback(callback_ctx, &c);
    }
    out->content = sc_strndup(alloc, "Hello from Anthropic", 19);
    out->content_len = 19;
    out->usage.completion_tokens = 3;
    return SC_OK;
#else
    if (!ac->api_key || ac->api_key_len == 0) return SC_ERR_PROVIDER_AUTH;

    sc_json_value_t *root = sc_json_object_new(alloc);
    if (!root) return SC_ERR_OUT_OF_MEMORY;

    sc_json_object_set(alloc, root, "model", sc_json_string_new(alloc, model, model_len));
    uint32_t max_tokens = request->max_tokens ? request->max_tokens : SC_ANTHROPIC_DEFAULT_MAX_TOKENS;
    sc_json_object_set(alloc, root, "max_tokens", sc_json_number_new(alloc, (double)max_tokens));
    sc_json_object_set(alloc, root, "temperature", sc_json_number_new(alloc, temperature));
    sc_json_object_set(alloc, root, "stream", sc_json_bool_new(alloc, true));

    const char *system_prompt = NULL;
    size_t system_len = 0;
    for (size_t i = 0; i < request->messages_count; i++) {
        if (request->messages[i].role == SC_ROLE_SYSTEM) {
            system_prompt = request->messages[i].content;
            system_len = request->messages[i].content_len;
            break;
        }
    }
    if (system_prompt && system_len > 0) {
        sc_json_object_set(alloc, root, "system", sc_json_string_new(alloc, system_prompt, system_len));
    }

    sc_json_value_t *msgs_arr = sc_json_array_new(alloc);
    if (!msgs_arr) { sc_json_free(alloc, root); return SC_ERR_OUT_OF_MEMORY; }
    sc_json_object_set(alloc, root, "messages", msgs_arr);
    for (size_t i = 0; i < request->messages_count; i++) {
        const sc_chat_message_t *m = &request->messages[i];
        if (m->role == SC_ROLE_SYSTEM) continue;
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) { sc_json_free(alloc, root); return SC_ERR_OUT_OF_MEMORY; }
        const char *role_str = (m->role == SC_ROLE_ASSISTANT) ? "assistant" : "user";
        sc_json_object_set(alloc, obj, "role", sc_json_string_new(alloc, role_str, strlen(role_str)));
        if (m->content && m->content_len > 0) {
            sc_json_object_set(alloc, obj, "content", sc_json_string_new(alloc, m->content, m->content_len));
        }
        sc_json_array_push(alloc, msgs_arr, obj);
    }

    char *body = NULL;
    size_t body_len = 0;
    sc_error_t err = sc_json_stringify(alloc, root, &body, &body_len);
    sc_json_free(alloc, root);
    if (err != SC_OK) return err;

    const char *base = ac->base_url && ac->base_url_len > 0 ? ac->base_url : "https://api.anthropic.com/v1";
    size_t base_len = ac->base_url_len ? ac->base_url_len : 29;
    char url_buf[512];
    int n = snprintf(url_buf, sizeof(url_buf), "%.*s/messages", (int)base_len, base);
    if (n <= 0 || (size_t)n >= sizeof(url_buf)) {
        alloc->free(alloc->ctx, body, body_len);
        return SC_ERR_INVALID_ARGUMENT;
    }

    char extra_buf[512];
    n = snprintf(extra_buf, sizeof(extra_buf),
        "x-api-key: %.*s\r\nanthropic-version: 2023-06-01\r\nAccept: text/event-stream",
        (int)ac->api_key_len, ac->api_key);
    if (n <= 0 || (size_t)n >= sizeof(extra_buf)) {
        alloc->free(alloc->ctx, body, body_len);
        return SC_ERR_INVALID_ARGUMENT;
    }

    anthropic_stream_ctx_t sctx = {
        .alloc = alloc, .callback = callback, .callback_ctx = callback_ctx,
        .last_error = SC_OK, .content_buf = NULL, .content_len = 0, .content_cap = 0,
    };
    err = sc_sse_parser_init(&sctx.parser, alloc);
    if (err != SC_OK) {
        alloc->free(alloc->ctx, body, body_len);
        return err;
    }
    err = sc_http_post_json_stream(alloc, url_buf, NULL, extra_buf, body, body_len,
        anthropic_stream_write_cb, &sctx);
    sc_sse_parser_deinit(&sctx.parser);
    alloc->free(alloc->ctx, body, body_len);
    if (err != SC_OK) return err;
    if (sctx.last_error != SC_OK) {
        if (sctx.content_buf) alloc->free(alloc->ctx, sctx.content_buf, sctx.content_cap);
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

static const char *anthropic_get_name(void *ctx) { (void)ctx; return "anthropic"; }

static void anthropic_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    sc_anthropic_ctx_t *ac = (sc_anthropic_ctx_t *)ctx;
    if (!ac) return;
    if (ac->api_key) free(ac->api_key);
    if (ac->base_url) free(ac->base_url);
    free(ac);
}

static const sc_provider_vtable_t anthropic_vtable = {
    .chat_with_system = anthropic_chat_with_system,
    .chat = anthropic_chat,
    .supports_native_tools = anthropic_supports_native_tools,
    .get_name = anthropic_get_name,
    .deinit = anthropic_deinit,
    .warmup = NULL, .chat_with_tools = NULL,
    .supports_streaming = anthropic_supports_streaming,
    .supports_vision = NULL, .supports_vision_for_model = NULL,
    .stream_chat = anthropic_stream_chat,
};

sc_error_t sc_anthropic_create(sc_allocator_t *alloc,
    const char *api_key, size_t api_key_len,
    const char *base_url, size_t base_url_len,
    sc_provider_t *out)
{
    (void)alloc;
    sc_anthropic_ctx_t *ac = (sc_anthropic_ctx_t *)calloc(1, sizeof(*ac));
    if (!ac) return SC_ERR_OUT_OF_MEMORY;
    if (api_key && api_key_len > 0) {
        ac->api_key = (char *)malloc(api_key_len + 1);
        if (!ac->api_key) { free(ac); return SC_ERR_OUT_OF_MEMORY; }
        memcpy(ac->api_key, api_key, api_key_len);
        ac->api_key[api_key_len] = '\0';
        ac->api_key_len = api_key_len;
    }
    if (base_url && base_url_len > 0) {
        ac->base_url = (char *)malloc(base_url_len + 1);
        if (!ac->base_url) {
            if (ac->api_key) free(ac->api_key);
            free(ac);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(ac->base_url, base_url, base_url_len);
        ac->base_url[base_url_len] = '\0';
        ac->base_url_len = base_url_len;
    }
    out->ctx = ac;
    out->vtable = &anthropic_vtable;
    return SC_OK;
}
