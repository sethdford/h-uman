#include "seaclaw/gateway/openai_compat.h"
#include "seaclaw/config.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/provider.h"
#include "seaclaw/providers/factory.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Resolve provider name and model from OpenAI model string.
 * "anthropic/claude-sonnet" -> provider=anthropic, model=claude-sonnet
 * "gpt-4o" -> provider=default_provider, model=gpt-4o */
static void resolve_model(const char *model_req, size_t model_len, const char *default_provider,
                          const char *default_model, char *out_provider, size_t out_provider_len,
                          const char **out_model, size_t *out_model_len) {
    const char *slash = memchr(model_req, '/', model_len);
    if (slash && (size_t)(slash - model_req) < model_len) {
        size_t prov_len = (size_t)(slash - model_req);
        if (prov_len >= out_provider_len)
            prov_len = out_provider_len - 1;
        memcpy(out_provider, model_req, prov_len);
        out_provider[prov_len] = '\0';
        *out_model = slash + 1;
        *out_model_len = model_len - prov_len - 1;
    } else {
        snprintf(out_provider, out_provider_len, "%s",
                 default_provider ? default_provider : "openai");
        *out_model = model_req;
        *out_model_len = model_len;
    }
    if (*out_model_len == 0 && default_model) {
        *out_model = default_model;
        *out_model_len = strlen(default_model);
    }
}

static sc_role_t role_from_string(const char *role, size_t len) {
    if (len == 4 && memcmp(role, "user", 4) == 0)
        return SC_ROLE_USER;
    if (len == 9 && memcmp(role, "assistant", 9) == 0)
        return SC_ROLE_ASSISTANT;
    if (len == 6 && memcmp(role, "system", 6) == 0)
        return SC_ROLE_SYSTEM;
    if (len == 4 && memcmp(role, "tool", 4) == 0)
        return SC_ROLE_TOOL;
    return SC_ROLE_USER;
}

/* Extract content string from message. Handles "content" as string or first text part. */
static const char *message_content(const sc_json_value_t *msg, size_t *out_len) {
    sc_json_value_t *content = sc_json_object_get(msg, "content");
    if (!content)
        return NULL;
    if (content->type == SC_JSON_STRING) {
        *out_len = content->data.string.len;
        return content->data.string.ptr;
    }
    if (content->type == SC_JSON_ARRAY && content->data.array.len > 0) {
        sc_json_value_t *first = content->data.array.items[0];
        if (first && first->type == SC_JSON_OBJECT) {
            sc_json_value_t *text = sc_json_object_get(first, "text");
            if (text && text->type == SC_JSON_STRING) {
                *out_len = text->data.string.len;
                return text->data.string.ptr;
            }
        }
    }
    return NULL;
}

static void error_response(sc_allocator_t *alloc, int status, const char *message,
                           int *out_status, char **out_body, size_t *out_body_len) {
    *out_status = status;
    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != SC_OK) {
        *out_body = NULL;
        *out_body_len = 0;
        return;
    }
    const char *epfx = "{\"error\":{\"message\":\"";
    if (sc_json_buf_append_raw(&buf, epfx, strlen(epfx)) != SC_OK ||
        sc_json_append_string(&buf, message, strlen(message)) != SC_OK ||
        sc_json_buf_append_raw(&buf, "\"}}", 3) != SC_OK) {
        sc_json_buf_free(&buf);
        *out_body = NULL;
        *out_body_len = 0;
        return;
    }
    size_t n = buf.len + 1;
    char *body = (char *)alloc->alloc(alloc->ctx, n);
    if (!body) {
        sc_json_buf_free(&buf);
        *out_body = NULL;
        *out_body_len = 0;
        return;
    }
    memcpy(body, buf.ptr, buf.len);
    body[buf.len] = '\0';
    *out_body = body;
    *out_body_len = buf.len;
    sc_json_buf_free(&buf);
}

/* Build one SSE chunk event: data: {"id":"...","object":"chat.completion.chunk",...}\n\n */
static sc_error_t append_sse_chunk(sc_json_buf_t *buf, sc_allocator_t *alloc,
                                   const char *id, const char *model, size_t model_len,
                                   long created, const char *delta, size_t delta_len,
                                   bool is_final) {
    (void)alloc;
    if (sc_json_buf_append_raw(buf, "data: ", 6) != SC_OK)
        return SC_ERR_OUT_OF_MEMORY;
    if (sc_json_buf_append_raw(buf, "{\"id\":\"", 6) != SC_OK)
        return SC_ERR_OUT_OF_MEMORY;
    if (sc_json_buf_append_raw(buf, id, strlen(id)) != SC_OK)
        return SC_ERR_OUT_OF_MEMORY;
    if (sc_json_buf_append_raw(buf, "\",\"object\":\"chat.completion.chunk\",\"created\":", 38) != SC_OK)
        return SC_ERR_OUT_OF_MEMORY;
    char num[24];
    int n = snprintf(num, sizeof(num), "%ld", created);
    if (n > 0 && sc_json_buf_append_raw(buf, num, (size_t)n) != SC_OK)
        return SC_ERR_OUT_OF_MEMORY;
    if (sc_json_buf_append_raw(buf, ",\"model\":\"", 9) != SC_OK)
        return SC_ERR_OUT_OF_MEMORY;
    if (sc_json_buf_append_raw(buf, model, model_len) != SC_OK)
        return SC_ERR_OUT_OF_MEMORY;
    if (is_final) {
        if (sc_json_buf_append_raw(buf,
                "\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n", 60) != SC_OK)
            return SC_ERR_OUT_OF_MEMORY;
    } else {
        if (sc_json_buf_append_raw(buf, "\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"", 44) != SC_OK)
            return SC_ERR_OUT_OF_MEMORY;
        if (delta && delta_len > 0 && sc_json_append_string(buf, delta, delta_len) != SC_OK)
            return SC_ERR_OUT_OF_MEMORY;
        if (sc_json_buf_append_raw(buf, "\"},\"finish_reason\":null}]}\n\n", 32) != SC_OK)
            return SC_ERR_OUT_OF_MEMORY;
    }
    return SC_OK;
}

void sc_openai_compat_handle_chat_completions(const char *body, size_t body_len,
                                               sc_allocator_t *alloc,
                                               const sc_app_context_t *app_ctx,
                                               int *out_status, char **out_body,
                                               size_t *out_body_len,
                                               const char **out_content_type) {
    *out_status = 500;
    *out_body = NULL;
    *out_body_len = 0;
    if (out_content_type)
        *out_content_type = "application/json";

    if (!alloc || !out_status || !out_body || !out_body_len)
        return;
    if (!body && body_len > 0)
        return;

    if (!app_ctx || !app_ctx->config) {
        error_response(alloc, 503, "Service unavailable: no config", out_status, out_body,
                      out_body_len);
        return;
    }

    const sc_config_t *cfg = app_ctx->config;
    const char *default_provider = cfg->default_provider ? cfg->default_provider : "openai";
    const char *default_model = cfg->default_model ? cfg->default_model : "gpt-4o";

    if (body_len == 0) {
        error_response(alloc, 400, "Missing request body", out_status, out_body, out_body_len);
        return;
    }

    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(alloc, body, body_len, &root);
    if (err != SC_OK || !root || root->type != SC_JSON_OBJECT) {
        if (root)
            sc_json_free(alloc, root);
        error_response(alloc, 400, "Invalid JSON", out_status, out_body, out_body_len);
        return;
    }

    bool want_stream = sc_json_get_bool(root, "stream", false);

    const char *model_req = sc_json_get_string(root, "model");
    size_t model_len = model_req ? strlen(model_req) : 0;
    if (!model_req || model_len == 0) {
        model_req = default_model;
        model_len = strlen(model_req);
    }

    sc_json_value_t *messages_val = sc_json_object_get(root, "messages");
    if (!messages_val || messages_val->type != SC_JSON_ARRAY ||
        messages_val->data.array.len == 0) {
        sc_json_free(alloc, root);
        error_response(alloc, 400, "messages is required and must be non-empty", out_status,
                      out_body, out_body_len);
        return;
    }

    double temperature = sc_json_get_number(root, "temperature", 0.7);
    double max_tokens_val = sc_json_get_number(root, "max_tokens", 1024.0);
    uint32_t max_tokens = (uint32_t)(max_tokens_val > 0 ? max_tokens_val : 1024);

    /* Convert messages to sc_chat_message_t */
    size_t msg_count = messages_val->data.array.len;
    sc_chat_message_t *msgs =
        (sc_chat_message_t *)alloc->alloc(alloc->ctx, msg_count * sizeof(sc_chat_message_t));
    if (!msgs) {
        sc_json_free(alloc, root);
        error_response(alloc, 500, "Out of memory", out_status, out_body, out_body_len);
        return;
    }
    memset(msgs, 0, msg_count * sizeof(sc_chat_message_t));

    for (size_t i = 0; i < msg_count; i++) {
        sc_json_value_t *item = messages_val->data.array.items[i];
        if (!item || item->type != SC_JSON_OBJECT) {
            alloc->free(alloc->ctx, msgs, msg_count * sizeof(sc_chat_message_t));
            sc_json_free(alloc, root);
            error_response(alloc, 400, "Invalid message format", out_status, out_body,
                          out_body_len);
            return;
        }
        const char *role_str = sc_json_get_string(item, "role");
        size_t role_len = role_str ? strlen(role_str) : 0;
        msgs[i].role = role_from_string(role_str ? role_str : "user", role_len);

        size_t content_len = 0;
        const char *content = message_content(item, &content_len);
        if (!content) {
            content = "";
            content_len = 0;
        }
        msgs[i].content = content;
        msgs[i].content_len = content_len;
        msgs[i].content_parts = NULL;
        msgs[i].content_parts_count = 0;
        msgs[i].tool_calls = NULL;
        msgs[i].tool_calls_count = 0;
    }

    char provider_name[64];
    const char *model;
    size_t model_len_out;
    resolve_model(model_req, model_len, default_provider, default_model, provider_name,
                  sizeof(provider_name), &model, &model_len_out);

    /* Copy model string before freeing JSON tree (model may point into it) */
    char model_buf[256];
    if (model_len_out >= sizeof(model_buf))
        model_len_out = sizeof(model_buf) - 1;
    memcpy(model_buf, model, model_len_out);
    model_buf[model_len_out] = '\0';
    model = model_buf;

    sc_json_free(alloc, root);

    time_t now = time(NULL);
    char id_buf[32];
    snprintf(id_buf, sizeof(id_buf), "chatcmpl-%lx", (unsigned long)now);
    long created_ts = (long)now;

    if (want_stream) {
        /* Simulated SSE streaming: get full response, chunk it, return as SSE body */
        sc_json_buf_t buf;
        if (sc_json_buf_init(&buf, alloc) != SC_OK) {
            alloc->free(alloc->ctx, msgs, msg_count * sizeof(sc_chat_message_t));
            error_response(alloc, 500, "Out of memory", out_status, out_body, out_body_len);
            return;
        }
#ifdef SC_IS_TEST
        (void)temperature;
        (void)max_tokens;
        alloc->free(alloc->ctx, msgs, msg_count * sizeof(sc_chat_message_t));
        /* Mock: emit chunks "m","o","c","k" then [DONE] */
        const char *mock_chunks[] = {"m", "o", "c", "k"};
        for (size_t i = 0; i < sizeof(mock_chunks) / sizeof(mock_chunks[0]); i++) {
            if (append_sse_chunk(&buf, alloc, id_buf, model, model_len_out, created_ts,
                                mock_chunks[i], 1, false) != SC_OK) {
                sc_json_buf_free(&buf);
                return;
            }
        }
        if (append_sse_chunk(&buf, alloc, id_buf, model, model_len_out, created_ts,
                            NULL, 0, true) != SC_OK) {
            sc_json_buf_free(&buf);
            return;
        }
        if (sc_json_buf_append_raw(&buf, "data: [DONE]\n\n", 14) != SC_OK) {
            sc_json_buf_free(&buf);
            return;
        }
#else
        sc_provider_t provider = {0};
        err = sc_provider_create_from_config(alloc, cfg, provider_name, strlen(provider_name),
                                              &provider);
        if (err == SC_ERR_NOT_SUPPORTED) {
            const char *api_key = sc_config_get_provider_key(cfg, provider_name);
            size_t api_key_len = api_key ? strlen(api_key) : 0;
            const char *base_url = sc_config_get_provider_base_url(cfg, provider_name);
            size_t base_url_len = base_url ? strlen(base_url) : 0;
            err = sc_provider_create(alloc, provider_name, strlen(provider_name), api_key, api_key_len,
                                    base_url, base_url_len, &provider);
        }
        if (err != SC_OK) {
            alloc->free(alloc->ctx, msgs, msg_count * sizeof(sc_chat_message_t));
            sc_json_buf_free(&buf);
            error_response(alloc, 503, "Provider not available", out_status, out_body, out_body_len);
            return;
        }
        sc_chat_request_t req = {
            .messages = msgs,
            .messages_count = msg_count,
            .model = model,
            .model_len = model_len_out,
            .temperature = temperature,
            .max_tokens = max_tokens,
            .tools = NULL,
            .tools_count = 0,
        };
        sc_chat_response_t resp = {0};
        err = provider.vtable->chat(provider.ctx, alloc, &req, model, model_len_out, temperature,
                                    &resp);
        alloc->free(alloc->ctx, msgs, msg_count * sizeof(sc_chat_message_t));
        if (provider.vtable && provider.vtable->deinit)
            provider.vtable->deinit(provider.ctx, alloc);
        if (err != SC_OK) {
            sc_json_buf_free(&buf);
            error_response(alloc, 502, "Provider error", out_status, out_body, out_body_len);
            if (resp.content)
                sc_chat_response_free(alloc, &resp);
            return;
        }
        /* Split content into word/whitespace chunks and emit SSE events */
        const char *content = resp.content ? resp.content : "";
        size_t content_len = resp.content ? resp.content_len : 0;
        const char *p = content;
        const char *end = content + content_len;
        while (p < end) {
            const char *start = p;
            bool is_space = (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r');
            if (is_space) {
                while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
                    p++;
            } else {
                while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
                    p++;
            }
            if (p > start) {
                size_t chunk_len = (size_t)(p - start);
                if (append_sse_chunk(&buf, alloc, id_buf, model, model_len_out, created_ts,
                                     start, chunk_len, false) != SC_OK) {
                    sc_chat_response_free(alloc, &resp);
                    sc_json_buf_free(&buf);
                    return;
                }
            }
        }
        if (append_sse_chunk(&buf, alloc, id_buf, model, model_len_out, created_ts,
                            NULL, 0, true) != SC_OK) {
            sc_chat_response_free(alloc, &resp);
            sc_json_buf_free(&buf);
            return;
        }
        sc_chat_response_free(alloc, &resp);
        if (sc_json_buf_append_raw(&buf, "data: [DONE]\n\n", 14) != SC_OK) {
            sc_json_buf_free(&buf);
            return;
        }
#endif
        size_t body_len = buf.len;
        size_t n = body_len + 1;
        char *resp_body = (char *)alloc->alloc(alloc->ctx, n);
        if (!resp_body) {
            sc_json_buf_free(&buf);
            *out_body = NULL;
            *out_body_len = 0;
            return;
        }
        memcpy(resp_body, buf.ptr, body_len);
        resp_body[body_len] = '\0';
        sc_json_buf_free(&buf);
        *out_status = 200;
        *out_body = resp_body;
        *out_body_len = body_len;
        if (out_content_type)
            *out_content_type = "text/event-stream";
        return;
    }

#ifdef SC_IS_TEST
    /* In tests, skip real provider call; return mock response */
    (void)temperature;
    (void)max_tokens;
    alloc->free(alloc->ctx, msgs, msg_count * sizeof(sc_chat_message_t));
    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != SC_OK) {
        error_response(alloc, 500, "Out of memory", out_status, out_body, out_body_len);
        return;
    }
    now = time(NULL);
    snprintf(id_buf, sizeof(id_buf), "chatcmpl-%lx", (unsigned long)now);
    static const char mock_hdr[] = "\",\"object\":\"chat.completion\",\"created\":";
    static const char mock_tail[] =
        "\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
        "\"content\":\"mock\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":0,\"completion_tokens\":0,\"total_tokens\":0}}";
    if (sc_json_buf_append_raw(&buf, "{\"id\":\"", 7) != SC_OK ||
        sc_json_buf_append_raw(&buf, id_buf, strlen(id_buf)) != SC_OK ||
        sc_json_buf_append_raw(&buf, mock_hdr, sizeof(mock_hdr) - 1) != SC_OK) {
        sc_json_buf_free(&buf);
        return;
    }
    char created_buf[24];
    snprintf(created_buf, sizeof(created_buf), "%ld", (long)now);
    sc_json_buf_append_raw(&buf, created_buf, strlen(created_buf));
    sc_json_buf_append_raw(&buf, ",\"model\":\"", 10);
    sc_json_buf_append_raw(&buf, model, model_len_out);
    sc_json_buf_append_raw(&buf, mock_tail, sizeof(mock_tail) - 1);
    size_t n = buf.len + 1;
    char *resp_body = (char *)alloc->alloc(alloc->ctx, n);
    if (resp_body) {
        memcpy(resp_body, buf.ptr, buf.len);
        resp_body[buf.len] = '\0';
        *out_status = 200;
        *out_body = resp_body;
        *out_body_len = buf.len;
    }
    sc_json_buf_free(&buf);
    return;
#else
    sc_provider_t provider = {0};
    err = sc_provider_create_from_config(alloc, cfg, provider_name, strlen(provider_name),
                                          &provider);
    if (err == SC_ERR_NOT_SUPPORTED) {
        const char *api_key = sc_config_get_provider_key(cfg, provider_name);
        size_t api_key_len = api_key ? strlen(api_key) : 0;
        const char *base_url = sc_config_get_provider_base_url(cfg, provider_name);
        size_t base_url_len = base_url ? strlen(base_url) : 0;
        err = sc_provider_create(alloc, provider_name, strlen(provider_name), api_key, api_key_len,
                                 base_url, base_url_len, &provider);
    }

    if (err != SC_OK) {
        alloc->free(alloc->ctx, msgs, msg_count * sizeof(sc_chat_message_t));
        error_response(alloc, 503, "Provider not available", out_status, out_body, out_body_len);
        return;
    }

    sc_chat_request_t req = {
        .messages = msgs,
        .messages_count = msg_count,
        .model = model,
        .model_len = model_len_out,
        .temperature = temperature,
        .max_tokens = max_tokens,
        .tools = NULL,
        .tools_count = 0,
    };

    sc_chat_response_t resp = {0};
    err = provider.vtable->chat(provider.ctx, alloc, &req, model, model_len_out, temperature,
                                &resp);
    alloc->free(alloc->ctx, msgs, msg_count * sizeof(sc_chat_message_t));

    if (provider.vtable && provider.vtable->deinit)
        provider.vtable->deinit(provider.ctx, alloc);

    if (err != SC_OK) {
        error_response(alloc, 502, "Provider error", out_status, out_body, out_body_len);
        if (resp.content)
            sc_chat_response_free(alloc, &resp);
        return;
    }

    /* Build OpenAI-format response */
    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != SC_OK) {
        sc_chat_response_free(alloc, &resp);
        error_response(alloc, 500, "Out of memory", out_status, out_body, out_body_len);
        return;
    }
    now = time(NULL);
    snprintf(id_buf, sizeof(id_buf), "chatcmpl-%lx", (unsigned long)now);

    static const char oc_hdr[] = "\",\"object\":\"chat.completion\",\"created\":";
    static const char oc_choices[] =
        "\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"";
    static const char oc_usage[] =
        "},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":";
    if (sc_json_buf_append_raw(&buf, "{\"id\":\"", 7) != SC_OK ||
        sc_json_buf_append_raw(&buf, id_buf, strlen(id_buf)) != SC_OK ||
        sc_json_buf_append_raw(&buf, oc_hdr, sizeof(oc_hdr) - 1) != SC_OK) {
        sc_json_buf_free(&buf);
        sc_chat_response_free(alloc, &resp);
        *out_body = NULL;
        *out_body_len = 0;
        return;
    }
    char created_buf[24];
    snprintf(created_buf, sizeof(created_buf), "%ld", (long)now);
    sc_json_buf_append_raw(&buf, created_buf, strlen(created_buf));
    sc_json_buf_append_raw(&buf, ",\"model\":\"", 10);
    if (resp.model && resp.model_len > 0)
        sc_json_buf_append_raw(&buf, resp.model, resp.model_len);
    else
        sc_json_buf_append_raw(&buf, model, model_len_out);
    sc_json_buf_append_raw(&buf, oc_choices, sizeof(oc_choices) - 1);
    if (resp.content && resp.content_len > 0)
        sc_json_append_string(&buf, resp.content, resp.content_len);
    else
        sc_json_buf_append_raw(&buf, "\"\"", 2);
    sc_json_buf_append_raw(&buf, ",\"content\":", 11);
    if (resp.content && resp.content_len > 0)
        sc_json_append_string(&buf, resp.content, resp.content_len);
    else
        sc_json_buf_append_raw(&buf, "\"\"", 2);
    sc_json_buf_append_raw(&buf, oc_usage, sizeof(oc_usage) - 1);
    char usage_buf[24];
    snprintf(usage_buf, sizeof(usage_buf), "%u", resp.usage.prompt_tokens);
    sc_json_buf_append_raw(&buf, usage_buf, strlen(usage_buf));
    sc_json_buf_append_raw(&buf, ",\"completion_tokens\":", 21);
    snprintf(usage_buf, sizeof(usage_buf), "%u", resp.usage.completion_tokens);
    sc_json_buf_append_raw(&buf, usage_buf, strlen(usage_buf));
    sc_json_buf_append_raw(&buf, ",\"total_tokens\":", 16);
    snprintf(usage_buf, sizeof(usage_buf), "%u", resp.usage.total_tokens);
    sc_json_buf_append_raw(&buf, usage_buf, strlen(usage_buf));
    sc_json_buf_append_raw(&buf, "}}", 2);

    size_t n = buf.len + 1;
    char *resp_body = (char *)alloc->alloc(alloc->ctx, n);
    sc_chat_response_free(alloc, &resp);
    if (!resp_body) {
        sc_json_buf_free(&buf);
        *out_body = NULL;
        *out_body_len = 0;
        return;
    }
    memcpy(resp_body, buf.ptr, buf.len);
    resp_body[buf.len] = '\0';
    *out_status = 200;
    *out_body = resp_body;
    *out_body_len = buf.len;
    sc_json_buf_free(&buf);
#endif
}

void sc_openai_compat_handle_models(sc_allocator_t *alloc,
                                    const sc_app_context_t *app_ctx,
                                    int *out_status, char **out_body,
                                    size_t *out_body_len) {
    *out_status = 200;
    *out_body = NULL;
    *out_body_len = 0;

    if (!alloc || !out_status || !out_body || !out_body_len)
        return;

    if (!app_ctx || !app_ctx->config) {
        error_response(alloc, 503, "Service unavailable: no config", out_status, out_body,
                      out_body_len);
        return;
    }

    const sc_config_t *cfg = app_ctx->config;
    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != SC_OK) {
        *out_status = 500;
        return;
    }

    const char *hdr = "{\"object\":\"list\",\"data\":[";
    if (sc_json_buf_append_raw(&buf, hdr, strlen(hdr)) != SC_OK) {
        sc_json_buf_free(&buf);
        *out_status = 500;
        return;
    }

    const char *pfx = "{\"id\":\"";
    const char *sfx = "\",\"object\":\"model\",\"owned_by\":\"seaclaw\"}";
    size_t pfx_len = strlen(pfx);
    size_t sfx_len = strlen(sfx);

    bool first = true;
    if (cfg->providers && cfg->providers_len > 0) {
        for (size_t i = 0; i < cfg->providers_len; i++) {
            const char *name = cfg->providers[i].name;
            const char *model = cfg->default_model ? cfg->default_model : "default";
            if (!name || !name[0])
                continue;
            if (!first)
                sc_json_buf_append_raw(&buf, ",", 1);
            if (sc_json_buf_append_raw(&buf, pfx, pfx_len) != SC_OK ||
                sc_json_buf_append_raw(&buf, name, strlen(name)) != SC_OK ||
                sc_json_buf_append_raw(&buf, "/", 1) != SC_OK ||
                sc_json_buf_append_raw(&buf, model, strlen(model)) != SC_OK ||
                sc_json_buf_append_raw(&buf, sfx, sfx_len) != SC_OK) {
                sc_json_buf_free(&buf);
                *out_status = 500;
                return;
            }
            first = false;
        }
    }
    if (first && cfg->default_provider && cfg->default_provider[0]) {
        const char *name = cfg->default_provider;
        const char *model = cfg->default_model ? cfg->default_model : "default";
        if (sc_json_buf_append_raw(&buf, pfx, pfx_len) != SC_OK ||
            sc_json_buf_append_raw(&buf, name, strlen(name)) != SC_OK ||
            sc_json_buf_append_raw(&buf, "/", 1) != SC_OK ||
            sc_json_buf_append_raw(&buf, model, strlen(model)) != SC_OK ||
            sc_json_buf_append_raw(&buf, sfx, sfx_len) != SC_OK) {
            sc_json_buf_free(&buf);
            *out_status = 500;
            return;
        }
    }

    if (sc_json_buf_append_raw(&buf, "]}", 2) != SC_OK) {
        sc_json_buf_free(&buf);
        *out_status = 500;
        return;
    }

    size_t n = buf.len + 1;
    char *body = (char *)alloc->alloc(alloc->ctx, n);
    if (!body) {
        sc_json_buf_free(&buf);
        *out_status = 500;
        return;
    }
    memcpy(body, buf.ptr, buf.len);
    body[buf.len] = '\0';
    *out_body = body;
    *out_body_len = buf.len;
    sc_json_buf_free(&buf);
}

