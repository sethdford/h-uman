#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/provider.h"
#include "human/providers/helpers.h"
#include "human/providers/mlx_stream_utf8.h" /* T5: utf8 boundary helpers */
#include "human/providers/provider_http.h"
#include "human/util/harmony_filter.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> /* T8 — clock_gettime for first-token latency metric */

#if !HU_IS_TEST
#include "human/core/http.h"
#include "human/providers/sse.h"
#endif

/* Serialize chat / stream_chat HTTP calls to a single-threaded MLX-style
 * upstream (gemma-realtime, our inline mlx-server.py, mlx_lm.server).
 *
 * Problem: those servers use plain HTTPServer (not ThreadingHTTPServer)
 * because MLX requires per-thread GPU stream init. When the daemon
 * receives multiple iMessages concurrently and dispatches several agent
 * turns in parallel, the second-to-Nth curl calls race the first and
 * hit CURLE_COULDNT_CONNECT (code=7) — observed in production as
 * "curl stream POST failed: Couldn't connect to server".
 *
 * This mutex makes every chat / stream_chat call wait for the previous
 * one to finish before reaching curl. Inference still runs at the same
 * speed; the daemon just queues turns naturally instead of racing them.
 * iMessages that would otherwise get NO REPLY get a (delayed) reply.
 *
 * Per ~/.claude/rules/silent-config-gated-subsystems.md the lock is
 * always-on; no config knob (the only reason to disable would be if
 * the upstream is actually multi-threaded, which we'd discover via a
 * different bug than silent-no-reply).
 *
 * Guarded by #if !HU_IS_TEST: the only references live in the non-test
 * (#else) branches of compatible_chat / compatible_stream_chat, so in
 * HU_IS_TEST (human_tests) builds the declaration would be unused and
 * trip GCC's -Wunused-variable under -Werror. */
#if !HU_IS_TEST
static pthread_mutex_t g_compatible_chat_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

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

#if !HU_IS_TEST
/* Build JSON body for POST /chat/completions (shared by non-streaming and streaming). */
static hu_error_t compatible_build_chat_json(hu_allocator_t *alloc,
                                             const hu_chat_request_t *request, const char *model,
                                             size_t model_len, double temperature,
                                             hu_json_value_t **root_out) {
    if (!alloc || !request || !root_out)
        return HU_ERR_INVALID_ARGUMENT;
    *root_out = NULL;

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
                                           hu_json_string_new(alloc, "image_url", 9));
                        char url_buf[64];
                        int ulen = snprintf(url_buf, sizeof(url_buf), "data:%.*s;base64,",
                                            (int)cp->data.image_base64.media_type_len,
                                            cp->data.image_base64.media_type);
                        if (ulen < 0)
                            ulen = 0;
                        if ((size_t)ulen > sizeof(url_buf))
                            ulen = (int)sizeof(url_buf);
                        size_t total = (size_t)ulen + cp->data.image_base64.data_len;
                        char *data_url = (char *)alloc->alloc(alloc->ctx, total + 1);
                        if (data_url) {
                            memcpy(data_url, url_buf, (size_t)ulen);
                            memcpy(data_url + ulen, cp->data.image_base64.data,
                                   cp->data.image_base64.data_len);
                            data_url[total] = '\0';
                            hu_json_value_t *iu = hu_json_object_new(alloc);
                            if (iu) {
                                hu_json_object_set(alloc, iu, "url",
                                                   hu_json_string_new(alloc, data_url, total));
                                hu_json_object_set(alloc, part, "image_url", iu);
                            }
                            alloc->free(alloc->ctx, data_url, total + 1);
                        }
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
                    }
                    hu_json_array_push(alloc, parts_arr, part);
                }
                hu_json_object_set(alloc, obj, "content", parts_arr);
            }
        } else if (m->content && m->content_len > 0) {
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

    /* On-device streaming hint (gemma-realtime Option B). Tri-state: emit a
     * boolean only when the router has an opinion; omit otherwise so the
     * server falls through to its HU_STREAM_BUFFER_STRIP env / model default.
     * Harmless on non-stream requests — the server only reads it on the
     * stream path. */
    if (request->stream_strip != 0) {
        hu_json_object_set(alloc, root, "stream_strip",
                           hu_json_bool_new(alloc, request->stream_strip > 0));
    }

    if (request->stop_sequences && request->stop_sequences_count > 0) {
        hu_json_value_t *stop_arr = hu_json_array_new(alloc);
        if (stop_arr) {
            for (size_t i = 0; i < request->stop_sequences_count; i++) {
                hu_json_array_push(alloc, stop_arr,
                                   hu_json_string_new(alloc, request->stop_sequences[i],
                                                      strlen(request->stop_sequences[i])));
            }
            hu_json_object_set(alloc, root, "stop", stop_arr);
        }
    }

    /* HuLa compiler (`hu_hula_compiler_chat_compile_execute`) sets response_format to
     * `json_object` for OpenAI-style APIs. See `docs/providers/hula-json-mode-matrix.md`. */
    if (request->response_format && request->response_format_len > 0) {
        hu_json_value_t *rf_obj = hu_json_object_new(alloc);
        if (rf_obj) {
            hu_json_value_t *rf_type =
                hu_json_string_new(alloc, request->response_format, request->response_format_len);
            hu_json_object_set(alloc, rf_obj, "type", rf_type);
            hu_json_object_set(alloc, root, "response_format", rf_obj);
        }
    }

    if (request->include_completion_logprobs) {
        hu_json_value_t *lp_true = hu_json_bool_new(alloc, true);
        if (lp_true)
            hu_json_object_set(alloc, root, "logprobs", lp_true);
        hu_json_value_t *topn = hu_json_number_new(alloc, 1.0);
        if (topn)
            hu_json_object_set(alloc, root, "top_logprobs", topn);
    }

    *root_out = root;
    return HU_OK;
}
#endif /* !HU_IS_TEST */

static hu_error_t compatible_chat(void *ctx, hu_allocator_t *alloc,
                                  const hu_chat_request_t *request, const char *model,
                                  size_t model_len, double temperature, hu_chat_response_t *out) {
    hu_compatible_ctx_t *cc = (hu_compatible_ctx_t *)ctx;
    if (!cc || !request || !out)
        return HU_ERR_INVALID_ARGUMENT;

#if HU_IS_TEST
    (void)model;
    (void)model_len;
    (void)temperature;
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

    hu_json_value_t *root = NULL;
    hu_error_t err =
        compatible_build_chat_json(alloc, request, model, model_len, temperature, &root);
    if (err != HU_OK)
        return err;

    char *body = NULL;
    size_t body_len = 0;
    err = hu_json_stringify(alloc, root, &body, &body_len);
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
    /* Serialize against concurrent stream_chat and other compatible_chat
     * calls to avoid CURLE_COULDNT_CONNECT against single-threaded MLX
     * upstreams. See g_compatible_chat_lock declaration. */
    pthread_mutex_lock(&g_compatible_chat_lock);
    err = hu_provider_http_post_json(alloc, url_buf, auth, NULL, body, body_len, &parsed);
    pthread_mutex_unlock(&g_compatible_chat_lock);
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
                out->content_len = out->content ? clen : 0;
            }
            hu_json_value_t *tc_arr = hu_json_object_get(msg, "tool_calls");
            if (tc_arr && tc_arr->type == HU_JSON_ARRAY && tc_arr->data.array.len > 0) {
                size_t tc_count = tc_arr->data.array.len;
                if (tc_count > SIZE_MAX / sizeof(hu_tool_call_t))
                    tc_count = 0;
                hu_tool_call_t *tcs = tc_count ? (hu_tool_call_t *)alloc->alloc(
                                                     alloc->ctx, tc_count * sizeof(hu_tool_call_t))
                                               : NULL;
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
                        tcs[valid].name = hu_strndup(alloc, fn_name, strlen(fn_name));
                        if (!tcs[valid].name)
                            continue;
                        tcs[valid].name_len = strlen(fn_name);
                        tcs[valid].id = tc_id ? hu_strndup(alloc, tc_id, strlen(tc_id)) : NULL;
                        tcs[valid].id_len = tcs[valid].id && tc_id ? strlen(tc_id) : 0;
                        tcs[valid].arguments =
                            fn_args ? hu_strndup(alloc, fn_args, strlen(fn_args)) : NULL;
                        tcs[valid].arguments_len =
                            tcs[valid].arguments && fn_args ? strlen(fn_args) : 0;
                        valid++;
                    }
                    if (valid > 0) {
                        out->tool_calls = tcs;
                        out->tool_calls_count = valid;
                    } else {
                        alloc->free(alloc->ctx, tcs, tc_count * sizeof(hu_tool_call_t));
                    }
                }
            }
        }
        hu_helpers_openai_choice_apply_logprobs(first, out);
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
        hu_str_free(alloc, cc->api_key);
    if (cc->base_url)
        hu_str_free(alloc, cc->base_url);
    alloc->free(alloc->ctx, cc, sizeof(*cc));
}

#if !HU_IS_TEST

#define COMPATIBLE_STREAM_MAX_TOOLS 16

typedef struct compatible_stream_tool {
    char *id;
    size_t id_len;
    char *name;
    size_t name_len;
    char *args;
    size_t args_len;
    size_t args_cap;
    bool started;
} compatible_stream_tool_t;

typedef struct compatible_stream_ctx {
    hu_allocator_t *alloc;
    hu_stream_callback_t callback;
    void *callback_ctx;
    hu_provider_sse_parser_t parser;
    hu_error_t last_error;
    char *content_buf;
    size_t content_len;
    size_t content_cap;
    compatible_stream_tool_t tools[COMPATIBLE_STREAM_MAX_TOOLS];
    size_t tools_count;
    /* T6 — caller's stop signal. When the user callback returns false,
     * subsequent callback fires are suppressed. The HTTP request keeps
     * running (no clean abort path through hu_http_post_json_stream
     * yet), but the consumer stops receiving chunks they don't want. */
    bool stop_requested;
    /* T8 — first-token latency metric. t0_ns is set BEFORE the HTTP
     * POST in compatible_stream_chat; first_token_latency_ms is filled
     * in by compatible_fire_chunk on the first content emission. */
    uint64_t t0_ns;
    uint64_t first_token_latency_ms;
    bool first_chunk_fired;
    /* T7 — buffered fallback for servers that don't speak SSE. Sniffed
     * on the first write_cb invocation: if the first bytes look like a
     * JSON object ('{') instead of SSE ('data:' / ':' / 'event:'), we
     * switch to buffered_mode for the rest of the stream — accumulate
     * raw bytes, parse as one-shot JSON after the HTTP call, fire a
     * single content chunk. Caches the verdict per call (each
     * stream_chat sniffs fresh). */
    bool buffered_mode;
    bool sniff_done;
    char *raw_body;
    size_t raw_body_len;
    size_t raw_body_cap;
    /* T5 (2026-05-26) — UTF-8 boundary safety carry buffer. SSE events
     * align with mlx-server tokens, not bytes; a multi-byte codepoint can
     * straddle two tokens if the tokenizer emits byte-level pieces (rare
     * but real for some Unicode ranges with `ensure_ascii=False` servers).
     * When the content of one event ends with the START bytes of a multi-
     * byte UTF-8 sequence, those bytes get stashed here and prepended to
     * the next event's content before the callback fires — so the
     * downstream consumer never sees a partial codepoint.
     *
     * Max valid prefix is 3 bytes (4-byte codepoint with 3 bytes seen so
     * far). Pure logic in `hu_mlx_utf8_safe_emit_len` from
     * src/providers/mlx_stream_utf8.c. */
    char utf8_carry[4];
    size_t utf8_carry_len;
} compatible_stream_ctx_t;

/* T8 — wall-clock now in ns via CLOCK_MONOTONIC. Returns 0 if the
 * clock isn't available (defensive; shouldn't happen on supported
 * platforms but avoids leaking garbage values). */
static inline uint64_t compatible_monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Fire `s->callback` honoring stop_requested. If the callback returns
 * false (stop), set stop_requested so future fires no-op.
 *
 * T8 — on the FIRST content-chunk fire after the HTTP POST, record
 * the wall-clock delta from t0_ns. The metric goes into
 * first_token_latency_ms for compatible_stream_chat to inspect after
 * the call returns. Tool-start/done/thinking chunks are also "first
 * tokens" from the operator's perspective; we measure the first one
 * regardless of type because that's when the user-visible UI gets
 * its first signal that the model is responding. */
static inline void compatible_fire_chunk(compatible_stream_ctx_t *s,
                                         const hu_stream_chunk_t *chunk) {
    if (!s || !s->callback || s->stop_requested)
        return;
    if (!s->first_chunk_fired && s->t0_ns > 0) {
        uint64_t now_ns = compatible_monotonic_ns();
        if (now_ns > s->t0_ns)
            s->first_token_latency_ms = (now_ns - s->t0_ns) / 1000000ULL;
        s->first_chunk_fired = true;
    }
    if (!s->callback(s->callback_ctx, chunk))
        s->stop_requested = true;
}

static void compatible_append_content(compatible_stream_ctx_t *s, const char *delta,
                                      size_t delta_len);
static void compatible_append_tool_args(compatible_stream_ctx_t *s, size_t idx, const char *delta,
                                        size_t delta_len);

static bool compatible_is_done_signal(const char *data, size_t data_len) {
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

static void compatible_extract_stream_delta(compatible_stream_ctx_t *s, const char *json_str,
                                            size_t json_len) {
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
            /* Final assembled content_buf (out->content) accumulates EVERY
             * byte exactly once, in event-arrival order — no carry logic
             * needed here because the final out->content has no boundary
             * to split a codepoint across. */
            compatible_append_content(s, content, clen);

            /* T5 (2026-05-26) — UTF-8 boundary safety for the STREAMED
             * callback. SSE events align with tokens; a token that's a
             * lone byte of a multi-byte codepoint would otherwise reach
             * the consumer as a malformed byte. The helper stitches the
             * prior carry + new content, fires only complete codepoints,
             * and stashes the trailing partial sequence for next event. */
            char emit_buf[512];
            size_t fire_len =
                hu_mlx_utf8_carry_emit(s->utf8_carry, &s->utf8_carry_len, sizeof(s->utf8_carry),
                                       content, clen, emit_buf, sizeof(emit_buf));
            if (fire_len > 0) {
                hu_stream_chunk_t chunk;
                memset(&chunk, 0, sizeof(chunk));
                chunk.type = HU_STREAM_CONTENT;
                chunk.delta = emit_buf;
                chunk.delta_len = fire_len;
                compatible_fire_chunk(s, &chunk);
            }
        }
    }

    hu_json_value_t *tc_arr = hu_json_object_get(delta, "tool_calls");
    if (tc_arr && tc_arr->type == HU_JSON_ARRAY) {
        for (size_t i = 0; i < tc_arr->data.array.len; i++) {
            hu_json_value_t *tc = tc_arr->data.array.items[i];
            if (!tc || tc->type != HU_JSON_OBJECT)
                continue;
            int idx = (int)hu_json_get_number(tc, "index", (double)i);
            if (idx < 0 || (size_t)idx >= COMPATIBLE_STREAM_MAX_TOOLS)
                continue;

            while (s->tools_count <= (size_t)idx) {
                memset(&s->tools[s->tools_count], 0, sizeof(compatible_stream_tool_t));
                s->tools_count++;
            }
            compatible_stream_tool_t *t = &s->tools[idx];

            const char *tc_id = hu_json_get_string(tc, "id");
            hu_json_value_t *fn = hu_json_object_get(tc, "function");

            if (tc_id && !t->started) {
                size_t id_len = strlen(tc_id);
                t->id = hu_strndup(s->alloc, tc_id, id_len);
                t->id_len = id_len;
                const char *fn_name = fn ? hu_json_get_string(fn, "name") : NULL;
                if (fn_name) {
                    size_t name_len = strlen(fn_name);
                    t->name = hu_strndup(s->alloc, fn_name, name_len);
                    t->name_len = name_len;
                }
                t->started = true;
                hu_stream_chunk_t chunk;
                memset(&chunk, 0, sizeof(chunk));
                chunk.type = HU_STREAM_TOOL_START;
                chunk.tool_name = t->name;
                chunk.tool_name_len = t->name_len;
                chunk.tool_call_id = t->id;
                chunk.tool_call_id_len = t->id_len;
                chunk.tool_index = idx;
                compatible_fire_chunk(s, &chunk);
            }

            const char *fn_args = fn ? hu_json_get_string(fn, "arguments") : NULL;
            if (fn_args) {
                size_t args_len = strlen(fn_args);
                if (args_len > 0) {
                    compatible_append_tool_args(s, (size_t)idx, fn_args, args_len);
                    hu_stream_chunk_t chunk;
                    memset(&chunk, 0, sizeof(chunk));
                    chunk.type = HU_STREAM_TOOL_DELTA;
                    chunk.delta = fn_args;
                    chunk.delta_len = args_len;
                    chunk.tool_call_id = t->id;
                    chunk.tool_call_id_len = t->id_len;
                    chunk.tool_name = t->name;
                    chunk.tool_name_len = t->name_len;
                    chunk.tool_index = idx;
                    compatible_fire_chunk(s, &chunk);
                }
            }
        }
    }

    hu_json_free(s->alloc, parsed);
}

static void compatible_append_tool_args(compatible_stream_ctx_t *s, size_t idx, const char *delta,
                                        size_t delta_len) {
    if (idx >= s->tools_count || !delta || delta_len == 0)
        return;
    compatible_stream_tool_t *t = &s->tools[idx];
    while (t->args_len + delta_len + 1 > t->args_cap) {
        size_t new_cap = t->args_cap ? t->args_cap * 2 : 256;
        while (new_cap < t->args_len + delta_len + 1)
            new_cap *= 2;
        char *nb = (char *)s->alloc->alloc(s->alloc->ctx, new_cap);
        if (!nb)
            return;
        if (t->args && t->args_len > 0)
            memcpy(nb, t->args, t->args_len);
        if (t->args)
            s->alloc->free(s->alloc->ctx, t->args, t->args_cap);
        t->args = nb;
        t->args_cap = new_cap;
    }
    memcpy(t->args + t->args_len, delta, delta_len);
    t->args_len += delta_len;
    t->args[t->args_len] = '\0';
}

static void compatible_append_content(compatible_stream_ctx_t *s, const char *delta,
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

static void compatible_sse_event_cb(const char *event_type, size_t event_type_len, const char *data,
                                    size_t data_len, void *userdata) {
    compatible_stream_ctx_t *s = (compatible_stream_ctx_t *)userdata;
    (void)event_type;
    (void)event_type_len;
    if (s->last_error != HU_OK)
        return;
    if (!data || data_len == 0)
        return;
    if (compatible_is_done_signal(data, data_len)) {
        hu_stream_chunk_t chunk;
        memset(&chunk, 0, sizeof(chunk));
        chunk.type = HU_STREAM_CONTENT;
        chunk.is_final = true;
        compatible_fire_chunk(s, &chunk);
        return;
    }
    compatible_extract_stream_delta(s, data, data_len);
}

/* T7 — accumulate raw bytes into s->raw_body for buffered-mode parse.
 * Grows geometrically up to a 4 MiB hard cap; OOM signals via
 * s->last_error and the caller stops feeding. */
#define HU_COMPATIBLE_RAW_BODY_MAX ((size_t)4 * 1024 * 1024)
static void compatible_raw_body_append(compatible_stream_ctx_t *s, const char *data, size_t len) {
    if (!s || !data || len == 0)
        return;
    size_t needed = s->raw_body_len + len + 1;
    if (needed > HU_COMPATIBLE_RAW_BODY_MAX) {
        s->last_error = HU_ERR_OUT_OF_MEMORY;
        return;
    }
    if (needed > s->raw_body_cap) {
        size_t new_cap = s->raw_body_cap ? s->raw_body_cap : (size_t)1024;
        while (new_cap < needed)
            new_cap *= (size_t)2;
        if (new_cap > HU_COMPATIBLE_RAW_BODY_MAX)
            new_cap = HU_COMPATIBLE_RAW_BODY_MAX;
        char *nb = (char *)s->alloc->alloc(s->alloc->ctx, new_cap);
        if (!nb) {
            s->last_error = HU_ERR_OUT_OF_MEMORY;
            return;
        }
        if (s->raw_body_len > 0)
            memcpy(nb, s->raw_body, s->raw_body_len);
        if (s->raw_body)
            s->alloc->free(s->alloc->ctx, s->raw_body, s->raw_body_cap);
        s->raw_body = nb;
        s->raw_body_cap = new_cap;
    }
    memcpy(s->raw_body + s->raw_body_len, data, len);
    s->raw_body_len += len;
    s->raw_body[s->raw_body_len] = '\0';
}

static size_t compatible_stream_write_cb(const char *data, size_t len, void *userdata) {
    compatible_stream_ctx_t *s = (compatible_stream_ctx_t *)userdata;

    /* T7 — sniff on first chunk to decide SSE vs buffered mode. SSE
     * responses start with "data:", ":" (comment), or "event:". A JSON
     * response starts with "{" (or whitespace before it). Be permissive:
     * if the first non-whitespace byte is '{', treat as buffered. */
    if (!s->sniff_done && len > 0) {
        size_t i = 0;
        while (i < len && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n'))
            i++;
        if (i < len && data[i] == '{') {
            s->buffered_mode = true;
        }
        s->sniff_done = true;
    }

    if (s->buffered_mode) {
        compatible_raw_body_append(s, data, len);
        if (s->last_error != HU_OK)
            return 0;
        return len;
    }

    hu_error_t err = hu_provider_sse_parser_feed(&s->parser, data, len, compatible_sse_event_cb, s);
    if (err != HU_OK) {
        hu_log_error("compatible", NULL, "SSE parser feed error: %s (data_len=%zu)",
                     hu_error_string(err), len);
        s->last_error = err;
        return 0;
    }
    return len;
}

#endif /* !HU_IS_TEST */

static bool compatible_supports_streaming(void *ctx) {
    (void)ctx;
    return true;
}

static bool compatible_supports_vision(void *ctx) {
    (void)ctx;
    return true;
}

static hu_error_t compatible_stream_chat(void *ctx, hu_allocator_t *alloc,
                                         const hu_chat_request_t *request, const char *model,
                                         size_t model_len, double temperature,
                                         hu_stream_callback_t callback, void *callback_ctx,
                                         hu_stream_chat_result_t *out) {
    hu_compatible_ctx_t *cc = (hu_compatible_ctx_t *)ctx;
    if (!cc || !request || !out)
        return HU_ERR_INVALID_ARGUMENT;

#if HU_IS_TEST
    (void)model;
    (void)model_len;
    (void)temperature;
    memset(out, 0, sizeof(*out));

    if (request->tools && request->tools_count > 0) {
        if (callback) {
            hu_stream_chunk_t c;
            memset(&c, 0, sizeof(c));
            c.type = HU_STREAM_CONTENT;
            c.delta = "Let me ";
            c.delta_len = 7;
            callback(callback_ctx, &c);
            memset(&c, 0, sizeof(c));
            c.type = HU_STREAM_CONTENT;
            c.delta = "check. ";
            c.delta_len = 7;
            callback(callback_ctx, &c);
            memset(&c, 0, sizeof(c));
            c.type = HU_STREAM_TOOL_START;
            c.tool_name = request->tools[0].name;
            c.tool_name_len = request->tools[0].name_len;
            c.tool_call_id = "call_stream_mock1";
            c.tool_call_id_len = 17;
            c.tool_index = 0;
            callback(callback_ctx, &c);
            memset(&c, 0, sizeof(c));
            c.type = HU_STREAM_TOOL_DELTA;
            c.delta = "{\"command\":\"ls\"}";
            c.delta_len = 16;
            c.tool_call_id = "call_stream_mock1";
            c.tool_call_id_len = 17;
            c.tool_index = 0;
            callback(callback_ctx, &c);
            memset(&c, 0, sizeof(c));
            c.type = HU_STREAM_TOOL_DONE;
            c.tool_name = request->tools[0].name;
            c.tool_name_len = request->tools[0].name_len;
            c.tool_call_id = "call_stream_mock1";
            c.tool_call_id_len = 17;
            c.tool_index = 0;
            callback(callback_ctx, &c);
            memset(&c, 0, sizeof(c));
            c.type = HU_STREAM_CONTENT;
            c.is_final = true;
            callback(callback_ctx, &c);
        }
        out->content = hu_strndup(alloc, "Let me check. ", 14);
        out->content_len = 14;
        hu_tool_call_t *tcs = (hu_tool_call_t *)alloc->alloc(alloc->ctx, sizeof(hu_tool_call_t));
        if (tcs) {
            memset(tcs, 0, sizeof(hu_tool_call_t));
            tcs[0].id = hu_strndup(alloc, "call_stream_mock1", 17);
            tcs[0].id_len = 17;
            tcs[0].name = hu_strndup(alloc, request->tools[0].name, request->tools[0].name_len);
            tcs[0].name_len = request->tools[0].name_len;
            tcs[0].arguments = hu_strndup(alloc, "{\"command\":\"ls\"}", 16);
            tcs[0].arguments_len = 16;
            out->tool_calls = tcs;
            out->tool_calls_count = 1;
        }
        out->usage.completion_tokens = 5;
        return HU_OK;
    }

    hu_stream_chunk_t chunk;
    memset(&chunk, 0, sizeof(chunk));
    chunk.type = HU_STREAM_CONTENT;
    chunk.delta = "test";
    chunk.delta_len = 4;
    chunk.is_final = false;
    if (callback)
        callback(callback_ctx, &chunk);
    memset(&chunk, 0, sizeof(chunk));
    chunk.type = HU_STREAM_CONTENT;
    chunk.is_final = true;
    if (callback)
        callback(callback_ctx, &chunk);
    out->content = hu_strndup(alloc, "test", 4);
    out->content_len = 4;
    return HU_OK;
#else
    if (!callback)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    if (!cc->base_url || cc->base_url_len == 0)
        return HU_ERR_CONFIG_INVALID;

    hu_json_value_t *root = NULL;
    hu_error_t err =
        compatible_build_chat_json(alloc, request, model, model_len, temperature, &root);
    if (err != HU_OK)
        return err;

    hu_json_object_set(alloc, root, "stream", hu_json_bool_new(alloc, true));

    char *body = NULL;
    size_t body_len = 0;
    err = hu_json_stringify(alloc, root, &body, &body_len);
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

    compatible_stream_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.alloc = alloc;
    sctx.callback = callback;
    sctx.callback_ctx = callback_ctx;
    sctx.last_error = HU_OK;
    err = hu_provider_sse_parser_init(&sctx.parser, alloc);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, body, body_len);
        return err;
    }

    /* M3 B4 T4-part3 (2026-05-26) — wrap the per-chunk user callback
     * in a Harmony channel-marker filter so streaming content arrives
     * cleaned (mlx-server.py's strip_thought_channels postprocessor
     * only runs in the non-streaming path). Fail-open if init fails. */
    hu_harmony_filter_t *harmony_filter = NULL;
    hu_harmony_callback_wrap_t harmony_wrap;
    memset(&harmony_wrap, 0, sizeof(harmony_wrap));
    if (hu_harmony_filter_init(alloc, &harmony_filter) == HU_OK) {
        harmony_wrap.inner = sctx.callback;
        harmony_wrap.inner_ctx = sctx.callback_ctx;
        harmony_wrap.filter = harmony_filter;
        harmony_wrap.alloc = alloc;
        sctx.callback = hu_harmony_callback_wrap_fn;
        sctx.callback_ctx = &harmony_wrap;
    }

    hu_log_info("compatible", NULL, "stream_chat: provider=compatible model=%.*s body_len=%zu",
                (int)model_len, model, body_len);
    /* T8 — record the request-start wall clock so compatible_fire_chunk
     * can measure first-token latency on the first content emission.
     * MUST be set AFTER any wrapping (sctx.callback may have been
     * swapped above) but BEFORE the HTTP call. */
    sctx.t0_ns = compatible_monotonic_ns();
    sctx.first_chunk_fired = false;
    sctx.first_token_latency_ms = 0;

    /* Serialize against concurrent chat/stream_chat to a single-threaded
     * MLX upstream. See g_compatible_chat_lock declaration. The lock is
     * held for the duration of the stream (covers SSE keep-alive); other
     * iMessages dispatched in parallel will wait their turn. */
    pthread_mutex_lock(&g_compatible_chat_lock);
    err = hu_http_post_json_stream(alloc, url_buf, auth, NULL, body, body_len,
                                   compatible_stream_write_cb, &sctx);
    pthread_mutex_unlock(&g_compatible_chat_lock);
    hu_provider_sse_parser_deinit(&sctx.parser);
    alloc->free(alloc->ctx, body, body_len);

    /* T7 — buffered fallback. If write_cb detected the server returned
     * a JSON object instead of an SSE stream, parse the accumulated
     * raw_body now and emit a single content chunk through the wrapped
     * callback (so harmony filtering still applies). Caller still gets
     * the final_chunk + tool_done emissions below. Logged one-shot so
     * operators see the server-version mismatch without log spam. */
    if (sctx.buffered_mode && sctx.raw_body && sctx.raw_body_len > 0) {
        static atomic_bool warned_buffered_fallback = false;
        hu_log_info_once(&warned_buffered_fallback, "compatible", NULL,
                         "stream_chat: server returned non-SSE response (%zu bytes); "
                         "using buffered fallback. Upgrade mlx-server to a streaming-"
                         "capable version for incremental token delivery.",
                         sctx.raw_body_len);
        hu_json_value_t *fb_root = NULL;
        if (hu_json_parse(alloc, sctx.raw_body, sctx.raw_body_len, &fb_root) == HU_OK && fb_root) {
            hu_json_value_t *choices = hu_json_object_get(fb_root, "choices");
            if (choices && choices->type == HU_JSON_ARRAY && choices->data.array.len > 0) {
                hu_json_value_t *first = choices->data.array.items[0];
                hu_json_value_t *msg = first ? hu_json_object_get(first, "message") : NULL;
                const char *content = msg ? hu_json_get_string(msg, "content") : NULL;
                if (content) {
                    size_t clen = strlen(content);
                    if (clen > 0) {
                        compatible_append_content(&sctx, content, clen);
                        hu_stream_chunk_t chunk;
                        memset(&chunk, 0, sizeof(chunk));
                        chunk.type = HU_STREAM_CONTENT;
                        chunk.delta = content;
                        chunk.delta_len = clen;
                        compatible_fire_chunk(&sctx, &chunk);
                    }
                }
            }
            hu_json_free(alloc, fb_root);
        }
    }
    if (sctx.raw_body) {
        alloc->free(alloc->ctx, sctx.raw_body, sctx.raw_body_cap);
        sctx.raw_body = NULL;
        sctx.raw_body_cap = 0;
        sctx.raw_body_len = 0;
    }

    /* T8 — log warn-once if first-token latency exceeded the soft
     * budget. Hardcoded to 500 ms (matches cfg.mlx_local.first_token_
     * budget_ms default per T2 spec D6). Operators see this and can
     * decide whether the mlx-server has regressed. Skipped when no
     * first chunk fired (request errored out before any response). */
    if (sctx.first_chunk_fired && sctx.first_token_latency_ms > 500) {
        static atomic_bool warned_first_token_slow = false;
        hu_log_info_once(&warned_first_token_slow, "compatible", NULL,
                         "stream_chat: first-token latency %llums exceeded "
                         "500ms soft budget (one-shot warning per process)",
                         (unsigned long long)sctx.first_token_latency_ms);
    }

    /* T4-part3 — drain held-back filter bytes through the original
     * user callback. Wrapper never propagates is_final on filtered
     * chunks; the explicit final_chunk below carries end-of-stream. */
    if (harmony_filter) {
        char *drained = NULL;
        size_t drained_len = 0;
        if (hu_harmony_filter_finish(harmony_filter, &drained, &drained_len) == HU_OK &&
            drained_len > 0) {
            hu_stream_chunk_t tail_chunk;
            memset(&tail_chunk, 0, sizeof(tail_chunk));
            tail_chunk.type = HU_STREAM_CONTENT;
            tail_chunk.delta = drained;
            tail_chunk.delta_len = drained_len;
            /* T6 — skip drain emission if caller already requested stop. */
            if (!sctx.stop_requested)
                callback(callback_ctx, &tail_chunk);
        }
        if (drained)
            alloc->free(alloc->ctx, drained, drained_len + 1);
        hu_harmony_filter_free(harmony_filter);
    }

#define COMPATIBLE_STREAM_CLEANUP_TOOLS()                            \
    do {                                                             \
        for (size_t _ti = 0; _ti < sctx.tools_count; _ti++) {        \
            compatible_stream_tool_t *_t = &sctx.tools[_ti];         \
            if (_t->id)                                              \
                alloc->free(alloc->ctx, _t->id, _t->id_len + 1);     \
            if (_t->name)                                            \
                alloc->free(alloc->ctx, _t->name, _t->name_len + 1); \
            if (_t->args)                                            \
                alloc->free(alloc->ctx, _t->args, _t->args_cap);     \
        }                                                            \
    } while (0)

    if (err != HU_OK) {
        COMPATIBLE_STREAM_CLEANUP_TOOLS();
        return err;
    }
    if (sctx.last_error != HU_OK) {
        if (sctx.content_buf)
            alloc->free(alloc->ctx, sctx.content_buf, sctx.content_cap);
        COMPATIBLE_STREAM_CLEANUP_TOOLS();
        return sctx.last_error;
    }

    if (sctx.content_buf && sctx.content_len > 0) {
        out->content = hu_strndup(alloc, sctx.content_buf, sctx.content_len);
        out->content_len = sctx.content_len;
        alloc->free(alloc->ctx, sctx.content_buf, sctx.content_cap);
    }

    if (sctx.tools_count > 0) {
        for (size_t ti = 0; ti < sctx.tools_count; ti++) {
            compatible_stream_tool_t *t = &sctx.tools[ti];
            if (t->started) {
                hu_stream_chunk_t done_chunk;
                memset(&done_chunk, 0, sizeof(done_chunk));
                done_chunk.type = HU_STREAM_TOOL_DONE;
                done_chunk.tool_name = t->name;
                done_chunk.tool_name_len = t->name_len;
                done_chunk.tool_call_id = t->id;
                done_chunk.tool_call_id_len = t->id_len;
                done_chunk.tool_index = (int)ti;
                /* T6 — honor caller's stop signal even for post-stream
                 * synthetic chunks. */
                if (!sctx.stop_requested)
                    callback(callback_ctx, &done_chunk);
            }
        }
        hu_tool_call_t *tcs =
            (hu_tool_call_t *)alloc->alloc(alloc->ctx, sctx.tools_count * sizeof(hu_tool_call_t));
        if (tcs) {
            size_t valid = 0;
            for (size_t ti = 0; ti < sctx.tools_count; ti++) {
                compatible_stream_tool_t *t = &sctx.tools[ti];
                if (!t->started)
                    continue;
                tcs[valid].id = t->id;
                tcs[valid].id_len = t->id_len;
                tcs[valid].name = t->name;
                tcs[valid].name_len = t->name_len;
                tcs[valid].arguments = t->args;
                tcs[valid].arguments_len = t->args_len;
                t->id = NULL;
                t->name = NULL;
                t->args = NULL;
                valid++;
            }
            out->tool_calls = tcs;
            out->tool_calls_count = valid;
        }
    }
    COMPATIBLE_STREAM_CLEANUP_TOOLS();
#undef COMPATIBLE_STREAM_CLEANUP_TOOLS

    if (!sctx.stop_requested) {
        hu_stream_chunk_t final_chunk;
        memset(&final_chunk, 0, sizeof(final_chunk));
        final_chunk.type = HU_STREAM_CONTENT;
        final_chunk.is_final = true;
        callback(callback_ctx, &final_chunk);
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
    .supports_vision = compatible_supports_vision,
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
        cc->api_key = hu_strndup(alloc, api_key, api_key_len);
        if (!cc->api_key) {
            alloc->free(alloc->ctx, cc, sizeof(*cc));
            return HU_ERR_OUT_OF_MEMORY;
        }
        cc->api_key_len = api_key_len;
    }
    if (base_url && base_url_len > 0) {
        cc->base_url = hu_strndup(alloc, base_url, base_url_len);
        if (!cc->base_url) {
            if (cc->api_key)
                hu_str_free(alloc, cc->api_key);
            alloc->free(alloc->ctx, cc, sizeof(*cc));
            return HU_ERR_OUT_OF_MEMORY;
        }
        cc->base_url_len = base_url_len;
    }
    out->ctx = cc;
    out->vtable = &compatible_vtable;
    return HU_OK;
}
