#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/provider.h"
#include "seaclaw/providers/sse.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_GEMINI_BASE               "https://generativelanguage.googleapis.com/v1beta/models"
#define SC_GEMINI_BASE_LEN           (sizeof(SC_GEMINI_BASE) - 1)
#define SC_GEMINI_DEFAULT_MAX_TOKENS 8192

typedef struct sc_gemini_ctx {
    char *api_key;
    size_t api_key_len;
    char *oauth_token; /* when set, use Bearer auth instead of ?key= */
    size_t oauth_token_len;
} sc_gemini_ctx_t;

#if !SC_IS_TEST
/* Extract text from Gemini SSE JSON: candidates[0].content.parts[0].text */
static char *gemini_extract_sse_delta(sc_allocator_t *alloc, const char *json_str,
                                      size_t json_len) {
    sc_json_value_t *parsed = NULL;
    if (sc_json_parse(alloc, json_str, json_len, &parsed) != SC_OK)
        return NULL;

    sc_json_value_t *candidates = sc_json_object_get(parsed, "candidates");
    if (!candidates || candidates->type != SC_JSON_ARRAY || candidates->data.array.len == 0) {
        sc_json_free(alloc, parsed);
        return NULL;
    }

    sc_json_value_t *first = candidates->data.array.items[0];
    if (first->type != SC_JSON_OBJECT) {
        sc_json_free(alloc, parsed);
        return NULL;
    }

    sc_json_value_t *content = sc_json_object_get(first, "content");
    if (!content || content->type != SC_JSON_OBJECT) {
        sc_json_free(alloc, parsed);
        return NULL;
    }

    sc_json_value_t *parts = sc_json_object_get(content, "parts");
    if (!parts || parts->type != SC_JSON_ARRAY || parts->data.array.len == 0) {
        sc_json_free(alloc, parsed);
        return NULL;
    }

    sc_json_value_t *part0 = parts->data.array.items[0];
    const char *text = sc_json_get_string(part0, "text");
    char *out = NULL;
    if (text && strlen(text) > 0)
        out = sc_strndup(alloc, text, strlen(text));
    sc_json_free(alloc, parsed);
    return out;
}
#endif

typedef struct gemini_stream_ctx {
    sc_allocator_t *alloc;
    sc_stream_callback_t callback;
    void *callback_ctx;
    sc_sse_parser_t parser;
    char *content_buf;
    size_t content_len;
    size_t content_cap;
    sc_error_t last_error;
} gemini_stream_ctx_t;

#if !SC_IS_TEST
static void gemini_sse_event_cb(const char *event_type, size_t event_type_len, const char *data,
                                size_t data_len, void *userdata) {
    (void)event_type;
    (void)event_type_len;
    gemini_stream_ctx_t *s = (gemini_stream_ctx_t *)userdata;
    if (!data || data_len == 0)
        return;

    if (data_len == 6 && memcmp(data, "[DONE]", 6) == 0) {
        sc_stream_chunk_t c = {.delta = NULL, .delta_len = 0, .is_final = true, .token_count = 0};
        s->callback(s->callback_ctx, &c);
        return;
    }

    char *delta = gemini_extract_sse_delta(s->alloc, data, data_len);
    if (!delta)
        return;

    size_t dlen = strlen(delta);

    if (s->content_len + dlen + 1 > s->content_cap) {
        size_t new_cap = s->content_cap ? s->content_cap * 2 : 4096;
        while (new_cap < s->content_len + dlen + 1)
            new_cap *= 2;
        char *nbuf = (char *)s->alloc->realloc(s->alloc->ctx, s->content_buf,
                                               s->content_cap ? s->content_cap : 0, new_cap);
        if (!nbuf) {
            s->alloc->free(s->alloc->ctx, delta, dlen + 1);
            s->last_error = SC_ERR_OUT_OF_MEMORY;
            return;
        }
        s->content_buf = nbuf;
        s->content_cap = new_cap;
    }
    memcpy(s->content_buf + s->content_len, delta, dlen + 1);
    s->content_len += dlen;

    sc_stream_chunk_t c = {
        .delta = delta,
        .delta_len = dlen,
        .is_final = false,
        .token_count = 1,
    };
    s->callback(s->callback_ctx, &c);
    s->alloc->free(s->alloc->ctx, delta, dlen + 1);
}

static size_t gemini_stream_write_cb(const char *chunk, size_t chunk_len, void *userdata) {
    gemini_stream_ctx_t *s = (gemini_stream_ctx_t *)userdata;
    if (s->last_error != SC_OK)
        return chunk_len;
    sc_error_t err =
        sc_sse_parser_feed(&s->parser, chunk, chunk_len, gemini_sse_event_cb, userdata);
    if (err != SC_OK)
        s->last_error = err;
    return chunk_len;
}
#endif

static sc_error_t gemini_chat(void *ctx, sc_allocator_t *alloc, const sc_chat_request_t *request,
                              const char *model, size_t model_len, double temperature,
                              sc_chat_response_t *out);

static sc_error_t gemini_chat_with_system(void *ctx, sc_allocator_t *alloc,
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
    sc_error_t err = gemini_chat(ctx, alloc, &req, model, model_len, temperature, &resp);
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

static sc_error_t gemini_chat(void *ctx, sc_allocator_t *alloc, const sc_chat_request_t *request,
                              const char *model, size_t model_len, double temperature,
                              sc_chat_response_t *out) {
    sc_gemini_ctx_t *gc = (sc_gemini_ctx_t *)ctx;
    if (!gc || !request || !out)
        return SC_ERR_INVALID_ARGUMENT;
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
            tcs[0].id = sc_strndup(alloc, "call_gemini_mock", 17);
            tcs[0].id_len = 17;
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
        const char *content = "Hello from mock Gemini";
        size_t len = strlen(content);
        out->content = sc_strndup(alloc, content, len);
        out->content_len = len;
        out->usage.prompt_tokens = 10;
        out->usage.completion_tokens = 5;
        out->usage.total_tokens = 15;
    }
    return SC_OK;
#else
    if (!gc->oauth_token && (!gc->api_key || gc->api_key_len == 0))
        return SC_ERR_PROVIDER_AUTH;

    sc_json_value_t *root = sc_json_object_new(alloc);
    if (!root)
        return SC_ERR_OUT_OF_MEMORY;

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
        sc_json_value_t *si_parts = sc_json_array_new(alloc);
        if (!si_parts) {
            sc_json_free(alloc, root);
            return SC_ERR_OUT_OF_MEMORY;
        }
        sc_json_value_t *si_obj = sc_json_object_new(alloc);
        if (!si_obj) {
            sc_json_free(alloc, si_parts);
            sc_json_free(alloc, root);
            return SC_ERR_OUT_OF_MEMORY;
        }
        sc_json_object_set(alloc, si_obj, "text",
                           sc_json_string_new(alloc, system_prompt, system_len));
        sc_json_array_push(alloc, si_parts, si_obj);
        sc_json_value_t *si = sc_json_object_new(alloc);
        if (!si) {
            sc_json_free(alloc, si_parts);
            sc_json_free(alloc, root);
            return SC_ERR_OUT_OF_MEMORY;
        }
        sc_json_object_set(alloc, si, "parts", si_parts);
        sc_json_object_set(alloc, root, "systemInstruction", si);
    }

    sc_json_value_t *contents = sc_json_array_new(alloc);
    if (!contents) {
        sc_json_free(alloc, root);
        return SC_ERR_OUT_OF_MEMORY;
    }
    sc_json_object_set(alloc, root, "contents", contents);

    for (size_t i = 0; i < request->messages_count; i++) {
        const sc_chat_message_t *m = &request->messages[i];
        if (m->role == SC_ROLE_SYSTEM)
            continue;

        sc_json_value_t *part = sc_json_object_new(alloc);
        if (!part) {
            sc_json_free(alloc, root);
            return SC_ERR_OUT_OF_MEMORY;
        }
        const char *role_str = "user";
        if (m->role == SC_ROLE_ASSISTANT)
            role_str = "model";
        sc_json_object_set(alloc, part, "role",
                           sc_json_string_new(alloc, role_str, strlen(role_str)));
        sc_json_value_t *parts_arr = sc_json_array_new(alloc);
        if (!parts_arr) {
            sc_json_free(alloc, part);
            sc_json_free(alloc, root);
            return SC_ERR_OUT_OF_MEMORY;
        }
        if (m->role == SC_ROLE_TOOL && m->tool_call_id && m->name && m->content) {
            sc_json_value_t *fr = sc_json_object_new(alloc);
            if (fr) {
                sc_json_object_set(alloc, fr, "functionResponse", sc_json_object_new(alloc));
                sc_json_value_t *fresp = sc_json_object_get(fr, "functionResponse");
                if (fresp) {
                    sc_json_object_set(alloc, fresp, "name",
                                       sc_json_string_new(alloc, m->name, m->name_len));
                    sc_json_object_set(alloc, fresp, "response",
                                       sc_json_string_new(alloc, m->content, m->content_len));
                }
                sc_json_array_push(alloc, parts_arr, fr);
            }
        } else if (m->role == SC_ROLE_ASSISTANT && m->tool_calls && m->tool_calls_count > 0) {
            for (size_t k = 0; k < m->tool_calls_count; k++) {
                const sc_tool_call_t *tc = &m->tool_calls[k];
                sc_json_value_t *fc = sc_json_object_new(alloc);
                if (fc) {
                    sc_json_object_set(alloc, fc, "functionCall", sc_json_object_new(alloc));
                    sc_json_value_t *fc_inner = sc_json_object_get(fc, "functionCall");
                    if (fc_inner && tc->name && tc->name_len > 0) {
                        sc_json_object_set(alloc, fc_inner, "name",
                                           sc_json_string_new(alloc, tc->name, tc->name_len));
                        if (tc->arguments && tc->arguments_len > 0) {
                            sc_json_value_t *args_val = NULL;
                            if (sc_json_parse(alloc, tc->arguments, tc->arguments_len, &args_val) ==
                                SC_OK)
                                sc_json_object_set(alloc, fc_inner, "args", args_val);
                        }
                    }
                    sc_json_array_push(alloc, parts_arr, fc);
                }
            }
        } else if (m->content && m->content_len > 0) {
            sc_json_value_t *text_part = sc_json_object_new(alloc);
            if (text_part) {
                sc_json_object_set(alloc, text_part, "text",
                                   sc_json_string_new(alloc, m->content, m->content_len));
                sc_json_array_push(alloc, parts_arr, text_part);
            }
        }
        sc_json_object_set(alloc, part, "parts", parts_arr);
        sc_json_array_push(alloc, contents, part);
    }
    if (request->tools && request->tools_count > 0) {
        sc_json_value_t *func_decls = sc_json_array_new(alloc);
        if (func_decls) {
            for (size_t i = 0; i < request->tools_count; i++) {
                sc_json_value_t *decl = sc_json_object_new(alloc);
                if (!decl) {
                    sc_json_free(alloc, func_decls);
                    sc_json_free(alloc, root);
                    return SC_ERR_OUT_OF_MEMORY;
                }
                sc_json_object_set(
                    alloc, decl, "name",
                    sc_json_string_new(alloc, request->tools[i].name, request->tools[i].name_len));
                sc_json_object_set(
                    alloc, decl, "description",
                    sc_json_string_new(
                        alloc, request->tools[i].description ? request->tools[i].description : "",
                        request->tools[i].description_len));
                if (request->tools[i].parameters_json &&
                    request->tools[i].parameters_json_len > 0) {
                    sc_json_value_t *params = NULL;
                    if (sc_json_parse(alloc, request->tools[i].parameters_json,
                                      request->tools[i].parameters_json_len, &params) == SC_OK)
                        sc_json_object_set(alloc, decl, "parameters", params);
                }
                sc_json_array_push(alloc, func_decls, decl);
            }
            sc_json_value_t *tools_wrapper = sc_json_object_new(alloc);
            if (tools_wrapper) {
                sc_json_object_set(alloc, tools_wrapper, "functionDeclarations", func_decls);
                sc_json_value_t *tools_arr = sc_json_array_new(alloc);
                if (tools_arr) {
                    sc_json_array_push(alloc, tools_arr, tools_wrapper);
                    sc_json_object_set(alloc, root, "tools", tools_arr);
                }
            }
        }
    }

    sc_json_object_set(alloc, root, "generationConfig", sc_json_object_new(alloc));
    sc_json_value_t *gen_cfg = sc_json_object_get(root, "generationConfig");
    if (gen_cfg) {
        sc_json_object_set(alloc, gen_cfg, "temperature", sc_json_number_new(alloc, temperature));
        uint32_t max_tok = request->max_tokens ? request->max_tokens : SC_GEMINI_DEFAULT_MAX_TOKENS;
        sc_json_object_set(alloc, gen_cfg, "maxOutputTokens",
                           sc_json_number_new(alloc, (double)max_tok));
    }

    /* Safety settings: BLOCK_NONE for minimal filtering (matching Zig default) */
    sc_json_value_t *safety_arr = sc_json_array_new(alloc);
    if (safety_arr) {
        const char *categories[] = {"HARM_CATEGORY_HARASSMENT", "HARM_CATEGORY_HATE_SPEECH",
                                    "HARM_CATEGORY_SEXUALLY_EXPLICIT",
                                    "HARM_CATEGORY_DANGEROUS_CONTENT",
                                    "HARM_CATEGORY_CIVIC_INTEGRITY"};
        for (size_t c = 0; c < sizeof(categories) / sizeof(categories[0]); c++) {
            sc_json_value_t *ent = sc_json_object_new(alloc);
            if (ent) {
                sc_json_object_set(alloc, ent, "category",
                                   sc_json_string_new(alloc, categories[c], strlen(categories[c])));
                sc_json_object_set(alloc, ent, "threshold",
                                   sc_json_string_new(alloc, "BLOCK_NONE", 10));
                sc_json_array_push(alloc, safety_arr, ent);
            }
        }
        sc_json_object_set(alloc, root, "safetySettings", safety_arr);
    }

    char *body = NULL;
    size_t body_len = 0;
    sc_error_t err = sc_json_stringify(alloc, root, &body, &body_len);
    sc_json_free(alloc, root);
    if (err != SC_OK)
        return err;

    char url_buf[768];
    char auth_buf[512];
    const char *auth_header = NULL;
    size_t auth_len = 0;

    if (gc->oauth_token && gc->oauth_token_len > 0) {
        int na = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)gc->oauth_token_len,
                          gc->oauth_token);
        if (na > 0 && (size_t)na < sizeof(auth_buf)) {
            auth_header = auth_buf;
            auth_len = (size_t)na;
        }
        int n = snprintf(url_buf, sizeof(url_buf), "%.*s/%.*s:generateContent",
                         (int)SC_GEMINI_BASE_LEN, SC_GEMINI_BASE, (int)model_len, model);
        if (n <= 0 || (size_t)n >= sizeof(url_buf)) {
            alloc->free(alloc->ctx, body, body_len);
            return SC_ERR_INVALID_ARGUMENT;
        }
    } else {
        if (!gc->api_key || gc->api_key_len == 0) {
            alloc->free(alloc->ctx, body, body_len);
            return SC_ERR_PROVIDER_AUTH;
        }
        int n = snprintf(url_buf, sizeof(url_buf), "%.*s/%.*s:generateContent?key=%.*s",
                         (int)SC_GEMINI_BASE_LEN, SC_GEMINI_BASE, (int)model_len, model,
                         (int)gc->api_key_len, gc->api_key);
        if (n <= 0 || (size_t)n >= sizeof(url_buf)) {
            alloc->free(alloc->ctx, body, body_len);
            return SC_ERR_INVALID_ARGUMENT;
        }
    }
    (void)auth_len;

    sc_http_response_t hresp = {0};
    err = sc_http_post_json(alloc, url_buf, auth_header, body, body_len, &hresp);
    alloc->free(alloc->ctx, body, body_len);
    if (err != SC_OK)
        return err;

    if (hresp.status_code < 200 || hresp.status_code >= 300) {
        sc_http_response_free(alloc, &hresp);
        if (hresp.status_code == 401 || hresp.status_code == 403)
            return SC_ERR_PROVIDER_AUTH;
        if (hresp.status_code == 429)
            return SC_ERR_PROVIDER_RATE_LIMITED;
        return SC_ERR_PROVIDER_RESPONSE;
    }

    sc_json_value_t *parsed = NULL;
    err = sc_json_parse(alloc, hresp.body, hresp.body_len, &parsed);
    sc_http_response_free(alloc, &hresp);
    if (err != SC_OK)
        return err;

    memset(out, 0, sizeof(*out));
    sc_json_value_t *candidates = sc_json_object_get(parsed, "candidates");
    if (candidates && candidates->type == SC_JSON_ARRAY && candidates->data.array.len > 0) {
        sc_json_value_t *first = candidates->data.array.items[0];
        sc_json_value_t *content = sc_json_object_get(first, "content");
        if (content && content->type == SC_JSON_OBJECT) {
            sc_json_value_t *parts = sc_json_object_get(content, "parts");
            if (parts && parts->type == SC_JSON_ARRAY) {
                sc_tool_call_t *tcs = NULL;
                size_t tc_valid = 0;
                for (size_t p = 0; p < parts->data.array.len; p++) {
                    sc_json_value_t *part = parts->data.array.items[p];
                    const char *text = sc_json_get_string(part, "text");
                    if (text && !out->content) {
                        size_t tlen = strlen(text);
                        out->content = sc_strndup(alloc, text, tlen);
                        out->content_len = tlen;
                    }
                    sc_json_value_t *fc = sc_json_object_get(part, "functionCall");
                    if (fc && fc->type == SC_JSON_OBJECT) {
                        const char *fname = sc_json_get_string(fc, "name");
                        if (!fname)
                            continue;
                        if (!tcs) {
                            tcs = (sc_tool_call_t *)alloc->alloc(
                                alloc->ctx, parts->data.array.len * sizeof(sc_tool_call_t));
                            if (!tcs)
                                break;
                            memset(tcs, 0, parts->data.array.len * sizeof(sc_tool_call_t));
                        }
                        sc_json_value_t *args = sc_json_object_get(fc, "args");
                        char *args_str = NULL;
                        size_t args_len = 0;
                        if (args && args->type == SC_JSON_OBJECT) {
                            sc_json_stringify(alloc, args, &args_str, &args_len);
                        }
                        tcs[tc_valid].id = NULL;
                        tcs[tc_valid].id_len = 0;
                        tcs[tc_valid].name = sc_strndup(alloc, fname, strlen(fname));
                        tcs[tc_valid].name_len = (size_t)strlen(fname);
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
        }
    }
    sc_json_value_t *usage = sc_json_object_get(parsed, "usageMetadata");
    if (usage && usage->type == SC_JSON_OBJECT) {
        out->usage.prompt_tokens = (uint32_t)sc_json_get_number(usage, "promptTokenCount", 0);
        out->usage.completion_tokens =
            (uint32_t)sc_json_get_number(usage, "candidatesTokenCount", 0);
        out->usage.total_tokens = out->usage.prompt_tokens + out->usage.completion_tokens;
    }
    sc_json_free(alloc, parsed);
    return SC_OK;
#endif
}

static bool gemini_supports_native_tools(void *ctx) {
    (void)ctx;
    return true;
}
static bool gemini_supports_streaming(void *ctx) {
    (void)ctx;
    return true;
}
static bool gemini_supports_vision(void *ctx) {
    (void)ctx;
    return true;
}
static bool gemini_supports_vision_for_model(void *ctx, const char *model, size_t model_len) {
    (void)ctx;
    (void)model;
    (void)model_len;
    return true;
}
static const char *gemini_get_name(void *ctx) {
    (void)ctx;
    return "gemini";
}

static sc_error_t gemini_stream_chat(void *ctx, sc_allocator_t *alloc,
                                     const sc_chat_request_t *request, const char *model,
                                     size_t model_len, double temperature,
                                     sc_stream_callback_t callback, void *callback_ctx,
                                     sc_stream_chat_result_t *out) {
    sc_gemini_ctx_t *gc = (sc_gemini_ctx_t *)ctx;
    if (!gc || !request || !callback || !out)
        return SC_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

#if SC_IS_TEST
    (void)model;
    (void)model_len;
    (void)temperature;
    {
        const char *chunks[] = {"Hello ", "from ", "Gemini ", "stream"};
        for (int i = 0; i < 4; i++) {
            sc_stream_chunk_t c = {
                .delta = chunks[i],
                .delta_len = strlen(chunks[i]),
                .is_final = false,
                .token_count = 1,
            };
            callback(callback_ctx, &c);
        }
        sc_stream_chunk_t c = {.delta = NULL, .delta_len = 0, .is_final = true, .token_count = 4};
        callback(callback_ctx, &c);
        out->content = sc_strndup(alloc, "Hello from Gemini stream", 25);
        out->content_len = 25;
        out->usage.completion_tokens = 4;
        return SC_OK;
    }
#else
    if (!gc->oauth_token && (!gc->api_key || gc->api_key_len == 0))
        return SC_ERR_PROVIDER_AUTH;

    sc_json_value_t *root = sc_json_object_new(alloc);
    if (!root)
        return SC_ERR_OUT_OF_MEMORY;

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
        sc_json_value_t *si_parts = sc_json_array_new(alloc);
        if (!si_parts) {
            sc_json_free(alloc, root);
            return SC_ERR_OUT_OF_MEMORY;
        }
        sc_json_value_t *si_obj = sc_json_object_new(alloc);
        if (!si_obj) {
            sc_json_free(alloc, si_parts);
            sc_json_free(alloc, root);
            return SC_ERR_OUT_OF_MEMORY;
        }
        sc_json_object_set(alloc, si_obj, "text",
                           sc_json_string_new(alloc, system_prompt, system_len));
        sc_json_array_push(alloc, si_parts, si_obj);
        sc_json_value_t *si = sc_json_object_new(alloc);
        if (!si) {
            sc_json_free(alloc, si_parts);
            sc_json_free(alloc, root);
            return SC_ERR_OUT_OF_MEMORY;
        }
        sc_json_object_set(alloc, si, "parts", si_parts);
        sc_json_object_set(alloc, root, "systemInstruction", si);
    }

    sc_json_value_t *contents = sc_json_array_new(alloc);
    if (!contents) {
        sc_json_free(alloc, root);
        return SC_ERR_OUT_OF_MEMORY;
    }
    sc_json_object_set(alloc, root, "contents", contents);

    for (size_t i = 0; i < request->messages_count; i++) {
        const sc_chat_message_t *m = &request->messages[i];
        if (m->role == SC_ROLE_SYSTEM)
            continue;

        sc_json_value_t *part = sc_json_object_new(alloc);
        if (!part) {
            sc_json_free(alloc, root);
            return SC_ERR_OUT_OF_MEMORY;
        }
        const char *role_str = "user";
        if (m->role == SC_ROLE_ASSISTANT)
            role_str = "model";
        sc_json_object_set(alloc, part, "role",
                           sc_json_string_new(alloc, role_str, strlen(role_str)));
        sc_json_value_t *parts_arr = sc_json_array_new(alloc);
        if (!parts_arr) {
            sc_json_free(alloc, part);
            sc_json_free(alloc, root);
            return SC_ERR_OUT_OF_MEMORY;
        }
        if (m->content && m->content_len > 0) {
            sc_json_value_t *text_part = sc_json_object_new(alloc);
            if (text_part) {
                sc_json_object_set(alloc, text_part, "text",
                                   sc_json_string_new(alloc, m->content, m->content_len));
                sc_json_array_push(alloc, parts_arr, text_part);
            }
        }
        sc_json_object_set(alloc, part, "parts", parts_arr);
        sc_json_array_push(alloc, contents, part);
    }

    sc_json_object_set(alloc, root, "generationConfig", sc_json_object_new(alloc));
    sc_json_value_t *gen_cfg = sc_json_object_get(root, "generationConfig");
    if (gen_cfg) {
        sc_json_object_set(alloc, gen_cfg, "temperature", sc_json_number_new(alloc, temperature));
        uint32_t max_tok = request->max_tokens ? request->max_tokens : SC_GEMINI_DEFAULT_MAX_TOKENS;
        sc_json_object_set(alloc, gen_cfg, "maxOutputTokens",
                           sc_json_number_new(alloc, (double)max_tok));
    }

    char *body = NULL;
    size_t body_len = 0;
    sc_error_t err = sc_json_stringify(alloc, root, &body, &body_len);
    sc_json_free(alloc, root);
    if (err != SC_OK)
        return err;

    char url_buf[768];
    char auth_buf[512];
    const char *auth_header = NULL;

    if (gc->oauth_token && gc->oauth_token_len > 0) {
        int na = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)gc->oauth_token_len,
                          gc->oauth_token);
        if (na > 0 && (size_t)na < sizeof(auth_buf))
            auth_header = auth_buf;
        snprintf(url_buf, sizeof(url_buf), "%.*s/%.*s:streamGenerateContent?alt=sse",
                 (int)SC_GEMINI_BASE_LEN, SC_GEMINI_BASE, (int)model_len, model);
    } else {
        snprintf(url_buf, sizeof(url_buf), "%.*s/%.*s:streamGenerateContent?key=%.*s&alt=sse",
                 (int)SC_GEMINI_BASE_LEN, SC_GEMINI_BASE, (int)model_len, model,
                 (int)gc->api_key_len, gc->api_key);
    }

    gemini_stream_ctx_t sctx = {
        .alloc = alloc,
        .callback = callback,
        .callback_ctx = callback_ctx,
        .content_buf = NULL,
        .content_len = 0,
        .content_cap = 0,
        .last_error = SC_OK,
    };
    err = sc_sse_parser_init(&sctx.parser, alloc);
    if (err != SC_OK) {
        alloc->free(alloc->ctx, body, body_len);
        return err;
    }
    err = sc_http_post_json_stream(alloc, url_buf, auth_header, NULL, body, body_len,
                                   gemini_stream_write_cb, &sctx);
    sc_sse_parser_deinit(&sctx.parser);
    alloc->free(alloc->ctx, body, body_len);
    if (err != SC_OK)
        return err;
    if (sctx.last_error != SC_OK) {
        if (sctx.content_buf)
            alloc->free(alloc->ctx, sctx.content_buf, sctx.content_cap);
        return sctx.last_error;
    }

    {
        sc_stream_chunk_t c = {.delta = NULL, .delta_len = 0, .is_final = true, .token_count = 0};
        callback(callback_ctx, &c);
    }

    if (sctx.content_buf && sctx.content_len > 0) {
        out->content = sc_strndup(alloc, sctx.content_buf, sctx.content_len);
        out->content_len = sctx.content_len;
        alloc->free(alloc->ctx, sctx.content_buf, sctx.content_cap);
    }
    out->usage.completion_tokens = (uint32_t)((sctx.content_len + 3) / 4);
    return SC_OK;
#endif
}

static void gemini_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    sc_gemini_ctx_t *gc = (sc_gemini_ctx_t *)ctx;
    if (!gc)
        return;
    if (gc->api_key)
        free(gc->api_key);
    if (gc->oauth_token)
        free(gc->oauth_token);
    free(gc);
}

static const sc_provider_vtable_t gemini_vtable = {
    .chat_with_system = gemini_chat_with_system,
    .chat = gemini_chat,
    .supports_native_tools = gemini_supports_native_tools,
    .get_name = gemini_get_name,
    .deinit = gemini_deinit,
    .warmup = NULL,
    .chat_with_tools = NULL,
    .supports_streaming = gemini_supports_streaming,
    .supports_vision = gemini_supports_vision,
    .supports_vision_for_model = gemini_supports_vision_for_model,
    .stream_chat = gemini_stream_chat,
};

sc_error_t sc_gemini_create(sc_allocator_t *alloc, const char *api_key, size_t api_key_len,
                            const char *base_url, size_t base_url_len, sc_provider_t *out) {
    (void)alloc;
    (void)base_url;
    (void)base_url_len;
    sc_gemini_ctx_t *gc = (sc_gemini_ctx_t *)calloc(1, sizeof(*gc));
    if (!gc)
        return SC_ERR_OUT_OF_MEMORY;
    if (api_key && api_key_len > 0) {
        gc->api_key = (char *)malloc(api_key_len + 1);
        if (!gc->api_key) {
            free(gc);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(gc->api_key, api_key, api_key_len);
        gc->api_key[api_key_len] = '\0';
        gc->api_key_len = api_key_len;
    }
    out->ctx = gc;
    out->vtable = &gemini_vtable;
    return SC_OK;
}

sc_error_t sc_gemini_create_with_oauth(sc_allocator_t *alloc, const char *oauth_token,
                                       size_t oauth_token_len, const char *base_url,
                                       size_t base_url_len, sc_provider_t *out) {
    (void)alloc;
    (void)base_url;
    (void)base_url_len;
    if (!oauth_token || oauth_token_len == 0)
        return SC_ERR_INVALID_ARGUMENT;
    sc_gemini_ctx_t *gc = (sc_gemini_ctx_t *)calloc(1, sizeof(*gc));
    if (!gc)
        return SC_ERR_OUT_OF_MEMORY;
    gc->oauth_token = (char *)malloc(oauth_token_len + 1);
    if (!gc->oauth_token) {
        free(gc);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memcpy(gc->oauth_token, oauth_token, oauth_token_len);
    gc->oauth_token[oauth_token_len] = '\0';
    gc->oauth_token_len = oauth_token_len;
    out->ctx = gc;
    out->vtable = &gemini_vtable;
    return SC_OK;
}
