#include "human/gateway/openai_compat.h"
#include "human/config.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/provider.h"
#include "human/providers/factory.h"
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

static hu_role_t role_from_string(const char *role, size_t len) {
    if (len == 4 && memcmp(role, "user", 4) == 0)
        return HU_ROLE_USER;
    if (len == 9 && memcmp(role, "assistant", 9) == 0)
        return HU_ROLE_ASSISTANT;
    if (len == 6 && memcmp(role, "system", 6) == 0)
        return HU_ROLE_SYSTEM;
    if (len == 4 && memcmp(role, "tool", 4) == 0)
        return HU_ROLE_TOOL;
    return HU_ROLE_USER;
}

/* Extract content string from message. Handles "content" as string or first text part. */
static const char *message_content(const hu_json_value_t *msg, size_t *out_len) {
    hu_json_value_t *content = hu_json_object_get(msg, "content");
    if (!content)
        return NULL;
    if (content->type == HU_JSON_STRING) {
        *out_len = content->data.string.len;
        return content->data.string.ptr;
    }
    if (content->type == HU_JSON_ARRAY && content->data.array.len > 0) {
        hu_json_value_t *first = content->data.array.items[0];
        if (first && first->type == HU_JSON_OBJECT) {
            hu_json_value_t *text = hu_json_object_get(first, "text");
            if (text && text->type == HU_JSON_STRING) {
                *out_len = text->data.string.len;
                return text->data.string.ptr;
            }
        }
    }
    return NULL;
}

static void error_response(hu_allocator_t *alloc, int status, const char *message, int *out_status,
                           char **out_body, size_t *out_body_len) {
    *out_status = status;
    hu_json_buf_t buf;
    if (hu_json_buf_init(&buf, alloc) != HU_OK) {
        *out_body = NULL;
        *out_body_len = 0;
        return;
    }
    const char *epfx = "{\"error\":{\"message\":\"";
    if (hu_json_buf_append_raw(&buf, epfx, strlen(epfx)) != HU_OK ||
        hu_json_append_string(&buf, message, strlen(message)) != HU_OK ||
        hu_json_buf_append_raw(&buf, "\"}}", 3) != HU_OK) {
        hu_json_buf_free(&buf);
        *out_body = NULL;
        *out_body_len = 0;
        return;
    }
    size_t n = buf.len + 1;
    char *body = (char *)alloc->alloc(alloc->ctx, n);
    if (!body) {
        hu_json_buf_free(&buf);
        *out_body = NULL;
        *out_body_len = 0;
        return;
    }
    memcpy(body, buf.ptr, buf.len);
    body[buf.len] = '\0';
    *out_body = body;
    *out_body_len = buf.len;
    hu_json_buf_free(&buf);
}

/* Build one SSE chunk event: data: {"id":"...","object":"chat.completion.chunk",...}\n\n */
/* sizeof(lit)-1 is a compile-time constant; avoids runtime strlen on string literals */
#define SSE_APPEND_LIT(buf, lit) hu_json_buf_append_raw((buf), (lit), sizeof(lit) - 1)

static hu_error_t append_sse_chunk(hu_json_buf_t *buf, hu_allocator_t *alloc, const char *id,
                                   const char *model, size_t model_len, long created,
                                   const char *delta, size_t delta_len, bool is_final) {
    (void)alloc;
    if (SSE_APPEND_LIT(buf, "data: ") != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;
    if (SSE_APPEND_LIT(buf, "{\"id\":\"") != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;
    if (hu_json_buf_append_raw(buf, id, strlen(id)) != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;
    if (SSE_APPEND_LIT(buf, "\",\"object\":\"chat.completion.chunk\",\"created\":") != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;
    char num[24];
    int n = snprintf(num, sizeof(num), "%ld", created);
    if (n > 0 && hu_json_buf_append_raw(buf, num, (size_t)n) != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;
    if (SSE_APPEND_LIT(buf, ",\"model\":\"") != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;
    if (hu_json_buf_append_raw(buf, model, model_len) != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;
    if (is_final) {
        if (SSE_APPEND_LIT(
                buf,
                "\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n") !=
            HU_OK)
            return HU_ERR_OUT_OF_MEMORY;
    } else {
        if (SSE_APPEND_LIT(buf, "\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"") != HU_OK)
            return HU_ERR_OUT_OF_MEMORY;
        if (delta && delta_len > 0 && hu_json_append_string(buf, delta, delta_len) != HU_OK)
            return HU_ERR_OUT_OF_MEMORY;
        if (SSE_APPEND_LIT(buf, "\"},\"finish_reason\":null}]}\n\n") != HU_OK)
            return HU_ERR_OUT_OF_MEMORY;
    }
    return HU_OK;
}

void hu_openai_compat_handle_chat_completions(const char *body, size_t body_len,
                                              hu_allocator_t *alloc,
                                              const hu_app_context_t *app_ctx, int *out_status,
                                              char **out_body, size_t *out_body_len,
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

    const hu_config_t *cfg = app_ctx->config;
    const char *default_provider = cfg->default_provider ? cfg->default_provider : "openai";
    const char *default_model = cfg->default_model ? cfg->default_model : "gpt-4o";

    if (body_len == 0) {
        error_response(alloc, 400, "Missing request body", out_status, out_body, out_body_len);
        return;
    }

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, body, body_len, &root);
    if (err != HU_OK || !root || root->type != HU_JSON_OBJECT) {
        if (root)
            hu_json_free(alloc, root);
        error_response(alloc, 400, "Invalid JSON", out_status, out_body, out_body_len);
        return;
    }

    bool want_stream = hu_json_get_bool(root, "stream", false);

    const char *model_req = hu_json_get_string(root, "model");
    size_t model_len = model_req ? strlen(model_req) : 0;
    if (!model_req || model_len == 0) {
        model_req = default_model;
        model_len = strlen(model_req);
    }

    hu_json_value_t *messages_val = hu_json_object_get(root, "messages");
    if (!messages_val || messages_val->type != HU_JSON_ARRAY || messages_val->data.array.len == 0) {
        hu_json_free(alloc, root);
        error_response(alloc, 400, "messages is required and must be non-empty", out_status,
                       out_body, out_body_len);
        return;
    }

    double temperature = hu_json_get_number(root, "temperature", 0.7);
    double max_tokens_val = hu_json_get_number(root, "max_tokens", 1024.0);
    uint32_t max_tokens = (uint32_t)(max_tokens_val > 0 ? max_tokens_val : 1024);

    /* Convert messages to hu_chat_message_t */
    size_t msg_count = messages_val->data.array.len;
    hu_chat_message_t *msgs =
        (hu_chat_message_t *)alloc->alloc(alloc->ctx, msg_count * sizeof(hu_chat_message_t));
    if (!msgs) {
        hu_json_free(alloc, root);
        error_response(alloc, 500, "Out of memory", out_status, out_body, out_body_len);
        return;
    }
    memset(msgs, 0, msg_count * sizeof(hu_chat_message_t));

    for (size_t i = 0; i < msg_count; i++) {
        hu_json_value_t *item = messages_val->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT) {
            alloc->free(alloc->ctx, msgs, msg_count * sizeof(hu_chat_message_t));
            hu_json_free(alloc, root);
            error_response(alloc, 400, "Invalid message format", out_status, out_body,
                           out_body_len);
            return;
        }
        const char *role_str = hu_json_get_string(item, "role");
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

    hu_json_free(alloc, root);

    time_t now = time(NULL);
    char id_buf[32];
    snprintf(id_buf, sizeof(id_buf), "chatcmpl-%lx", (unsigned long)now);
    long created_ts = (long)now;

    if (want_stream) {
        /* Simulated SSE streaming: get full response, chunk it, return as SSE body */
        hu_json_buf_t buf;
        if (hu_json_buf_init(&buf, alloc) != HU_OK) {
            alloc->free(alloc->ctx, msgs, msg_count * sizeof(hu_chat_message_t));
            error_response(alloc, 500, "Out of memory", out_status, out_body, out_body_len);
            return;
        }
#ifdef HU_IS_TEST
        (void)temperature;
        (void)max_tokens;
        alloc->free(alloc->ctx, msgs, msg_count * sizeof(hu_chat_message_t));
        /* Mock: emit chunks "m","o","c","k" then [DONE] */
        const char *mock_chunks[] = {"m", "o", "c", "k"};
        for (size_t i = 0; i < sizeof(mock_chunks) / sizeof(mock_chunks[0]); i++) {
            if (append_sse_chunk(&buf, alloc, id_buf, model, model_len_out, created_ts,
                                 mock_chunks[i], 1, false) != HU_OK) {
                hu_json_buf_free(&buf);
                return;
            }
        }
        if (append_sse_chunk(&buf, alloc, id_buf, model, model_len_out, created_ts, NULL, 0,
                             true) != HU_OK) {
            hu_json_buf_free(&buf);
            return;
        }
        if (hu_json_buf_append_raw(&buf, "data: [DONE]\n\n", 14) != HU_OK) {
            hu_json_buf_free(&buf);
            return;
        }
#else
        hu_provider_t provider = {0};
        err = hu_provider_create_from_config(alloc, cfg, provider_name, strlen(provider_name),
                                             &provider);
        if (err == HU_ERR_NOT_SUPPORTED) {
            const char *api_key = hu_config_get_provider_key(cfg, provider_name);
            size_t api_key_len = api_key ? strlen(api_key) : 0;
            const char *base_url = hu_config_get_provider_base_url(cfg, provider_name);
            size_t base_url_len = base_url ? strlen(base_url) : 0;
            err = hu_provider_create(alloc, provider_name, strlen(provider_name), api_key,
                                     api_key_len, base_url, base_url_len, &provider);
        }
        if (err != HU_OK) {
            alloc->free(alloc->ctx, msgs, msg_count * sizeof(hu_chat_message_t));
            hu_json_buf_free(&buf);
            error_response(alloc, 503, "Provider not available", out_status, out_body,
                           out_body_len);
            return;
        }
        hu_chat_request_t req = {
            .messages = msgs,
            .messages_count = msg_count,
            .model = model,
            .model_len = model_len_out,
            .temperature = temperature,
            .max_tokens = max_tokens,
            .tools = NULL,
            .tools_count = 0,
        };
        hu_chat_response_t resp = {0};
        err = provider.vtable->chat(provider.ctx, alloc, &req, model, model_len_out, temperature,
                                    &resp);
        alloc->free(alloc->ctx, msgs, msg_count * sizeof(hu_chat_message_t));
        if (provider.vtable && provider.vtable->deinit)
            provider.vtable->deinit(provider.ctx, alloc);
        if (err != HU_OK) {
            hu_json_buf_free(&buf);
            error_response(alloc, 502, "Provider error", out_status, out_body, out_body_len);
            if (resp.content)
                hu_chat_response_free(alloc, &resp);
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
                if (append_sse_chunk(&buf, alloc, id_buf, model, model_len_out, created_ts, start,
                                     chunk_len, false) != HU_OK) {
                    hu_chat_response_free(alloc, &resp);
                    hu_json_buf_free(&buf);
                    return;
                }
            }
        }
        if (append_sse_chunk(&buf, alloc, id_buf, model, model_len_out, created_ts, NULL, 0,
                             true) != HU_OK) {
            hu_chat_response_free(alloc, &resp);
            hu_json_buf_free(&buf);
            return;
        }
        hu_chat_response_free(alloc, &resp);
        if (hu_json_buf_append_raw(&buf, "data: [DONE]\n\n", 14) != HU_OK) {
            hu_json_buf_free(&buf);
            return;
        }
#endif
        size_t resp_len = buf.len;
        size_t n = resp_len + 1;
        char *resp_body = (char *)alloc->alloc(alloc->ctx, n);
        if (!resp_body) {
            hu_json_buf_free(&buf);
            *out_body = NULL;
            *out_body_len = 0;
            return;
        }
        memcpy(resp_body, buf.ptr, resp_len);
        resp_body[resp_len] = '\0';
        hu_json_buf_free(&buf);
        *out_status = 200;
        *out_body = resp_body;
        *out_body_len = resp_len;
        if (out_content_type)
            *out_content_type = "text/event-stream";
        return;
    }

#ifdef HU_IS_TEST
    /* In tests, skip real provider call; return mock response */
    (void)temperature;
    (void)max_tokens;
    alloc->free(alloc->ctx, msgs, msg_count * sizeof(hu_chat_message_t));
    hu_json_buf_t buf;
    if (hu_json_buf_init(&buf, alloc) != HU_OK) {
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
    if (hu_json_buf_append_raw(&buf, "{\"id\":\"", 7) != HU_OK ||
        hu_json_buf_append_raw(&buf, id_buf, strlen(id_buf)) != HU_OK ||
        hu_json_buf_append_raw(&buf, mock_hdr, sizeof(mock_hdr) - 1) != HU_OK) {
        hu_json_buf_free(&buf);
        return;
    }
    char created_buf[24];
    snprintf(created_buf, sizeof(created_buf), "%ld", (long)now);
    hu_json_buf_append_raw(&buf, created_buf, strlen(created_buf));
    hu_json_buf_append_raw(&buf, ",\"model\":\"", 10);
    hu_json_buf_append_raw(&buf, model, model_len_out);
    hu_json_buf_append_raw(&buf, mock_tail, sizeof(mock_tail) - 1);
    size_t n = buf.len + 1;
    char *resp_body = (char *)alloc->alloc(alloc->ctx, n);
    if (resp_body) {
        memcpy(resp_body, buf.ptr, buf.len);
        resp_body[buf.len] = '\0';
        *out_status = 200;
        *out_body = resp_body;
        *out_body_len = buf.len;
    }
    hu_json_buf_free(&buf);
    return;
#else
    hu_provider_t provider = {0};
    err =
        hu_provider_create_from_config(alloc, cfg, provider_name, strlen(provider_name), &provider);
    if (err == HU_ERR_NOT_SUPPORTED) {
        const char *api_key = hu_config_get_provider_key(cfg, provider_name);
        size_t api_key_len = api_key ? strlen(api_key) : 0;
        const char *base_url = hu_config_get_provider_base_url(cfg, provider_name);
        size_t base_url_len = base_url ? strlen(base_url) : 0;
        err = hu_provider_create(alloc, provider_name, strlen(provider_name), api_key, api_key_len,
                                 base_url, base_url_len, &provider);
    }

    if (err != HU_OK) {
        alloc->free(alloc->ctx, msgs, msg_count * sizeof(hu_chat_message_t));
        error_response(alloc, 503, "Provider not available", out_status, out_body, out_body_len);
        return;
    }

    hu_chat_request_t req = {
        .messages = msgs,
        .messages_count = msg_count,
        .model = model,
        .model_len = model_len_out,
        .temperature = temperature,
        .max_tokens = max_tokens,
        .tools = NULL,
        .tools_count = 0,
    };

    hu_chat_response_t resp = {0};
    err =
        provider.vtable->chat(provider.ctx, alloc, &req, model, model_len_out, temperature, &resp);
    alloc->free(alloc->ctx, msgs, msg_count * sizeof(hu_chat_message_t));

    if (provider.vtable && provider.vtable->deinit)
        provider.vtable->deinit(provider.ctx, alloc);

    if (err != HU_OK) {
        error_response(alloc, 502, "Provider error", out_status, out_body, out_body_len);
        if (resp.content)
            hu_chat_response_free(alloc, &resp);
        return;
    }

    /* Build OpenAI-format response */
    hu_json_buf_t buf;
    if (hu_json_buf_init(&buf, alloc) != HU_OK) {
        hu_chat_response_free(alloc, &resp);
        error_response(alloc, 500, "Out of memory", out_status, out_body, out_body_len);
        return;
    }
    now = time(NULL);
    snprintf(id_buf, sizeof(id_buf), "chatcmpl-%lx", (unsigned long)now);

    static const char oc_hdr[] = "\",\"object\":\"chat.completion\",\"created\":";
    static const char oc_choices[] =
        "\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"";
    static const char oc_usage[] = "},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":";
    if (hu_json_buf_append_raw(&buf, "{\"id\":\"", 7) != HU_OK ||
        hu_json_buf_append_raw(&buf, id_buf, strlen(id_buf)) != HU_OK ||
        hu_json_buf_append_raw(&buf, oc_hdr, sizeof(oc_hdr) - 1) != HU_OK) {
        hu_json_buf_free(&buf);
        hu_chat_response_free(alloc, &resp);
        *out_body = NULL;
        *out_body_len = 0;
        return;
    }
    char created_buf[24];
    snprintf(created_buf, sizeof(created_buf), "%ld", (long)now);
    hu_json_buf_append_raw(&buf, created_buf, strlen(created_buf));
    hu_json_buf_append_raw(&buf, ",\"model\":\"", 10);
    if (resp.model && resp.model_len > 0)
        hu_json_buf_append_raw(&buf, resp.model, resp.model_len);
    else
        hu_json_buf_append_raw(&buf, model, model_len_out);
    hu_json_buf_append_raw(&buf, oc_choices, sizeof(oc_choices) - 1);
    if (resp.content && resp.content_len > 0)
        hu_json_append_string(&buf, resp.content, resp.content_len);
    else
        hu_json_buf_append_raw(&buf, "\"\"", 2);
    hu_json_buf_append_raw(&buf, ",\"content\":", 11);
    if (resp.content && resp.content_len > 0)
        hu_json_append_string(&buf, resp.content, resp.content_len);
    else
        hu_json_buf_append_raw(&buf, "\"\"", 2);
    hu_json_buf_append_raw(&buf, oc_usage, sizeof(oc_usage) - 1);
    char usage_buf[24];
    snprintf(usage_buf, sizeof(usage_buf), "%u", resp.usage.prompt_tokens);
    hu_json_buf_append_raw(&buf, usage_buf, strlen(usage_buf));
    hu_json_buf_append_raw(&buf, ",\"completion_tokens\":", 21);
    snprintf(usage_buf, sizeof(usage_buf), "%u", resp.usage.completion_tokens);
    hu_json_buf_append_raw(&buf, usage_buf, strlen(usage_buf));
    hu_json_buf_append_raw(&buf, ",\"total_tokens\":", 16);
    snprintf(usage_buf, sizeof(usage_buf), "%u", resp.usage.total_tokens);
    hu_json_buf_append_raw(&buf, usage_buf, strlen(usage_buf));
    hu_json_buf_append_raw(&buf, "}}", 2);

    size_t n = buf.len + 1;
    char *resp_body = (char *)alloc->alloc(alloc->ctx, n);
    hu_chat_response_free(alloc, &resp);
    if (!resp_body) {
        hu_json_buf_free(&buf);
        *out_body = NULL;
        *out_body_len = 0;
        return;
    }
    memcpy(resp_body, buf.ptr, buf.len);
    resp_body[buf.len] = '\0';
    *out_status = 200;
    *out_body = resp_body;
    *out_body_len = buf.len;
    hu_json_buf_free(&buf);
#endif
}

void hu_openai_compat_handle_models(hu_allocator_t *alloc, const hu_app_context_t *app_ctx,
                                    int *out_status, char **out_body, size_t *out_body_len) {
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

    const hu_config_t *cfg = app_ctx->config;
    hu_json_buf_t buf;
    if (hu_json_buf_init(&buf, alloc) != HU_OK) {
        *out_status = 500;
        return;
    }

    const char *hdr = "{\"object\":\"list\",\"data\":[";
    if (hu_json_buf_append_raw(&buf, hdr, strlen(hdr)) != HU_OK) {
        hu_json_buf_free(&buf);
        *out_status = 500;
        return;
    }

    const char *pfx = "{\"id\":\"";
    const char *sfx = "\",\"object\":\"model\",\"owned_by\":\"human\"}";
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
                hu_json_buf_append_raw(&buf, ",", 1);
            if (hu_json_buf_append_raw(&buf, pfx, pfx_len) != HU_OK ||
                hu_json_buf_append_raw(&buf, name, strlen(name)) != HU_OK ||
                hu_json_buf_append_raw(&buf, "/", 1) != HU_OK ||
                hu_json_buf_append_raw(&buf, model, strlen(model)) != HU_OK ||
                hu_json_buf_append_raw(&buf, sfx, sfx_len) != HU_OK) {
                hu_json_buf_free(&buf);
                *out_status = 500;
                return;
            }
            first = false;
        }
    }
    if (first && cfg->default_provider && cfg->default_provider[0]) {
        const char *name = cfg->default_provider;
        const char *model = cfg->default_model ? cfg->default_model : "default";
        if (hu_json_buf_append_raw(&buf, pfx, pfx_len) != HU_OK ||
            hu_json_buf_append_raw(&buf, name, strlen(name)) != HU_OK ||
            hu_json_buf_append_raw(&buf, "/", 1) != HU_OK ||
            hu_json_buf_append_raw(&buf, model, strlen(model)) != HU_OK ||
            hu_json_buf_append_raw(&buf, sfx, sfx_len) != HU_OK) {
            hu_json_buf_free(&buf);
            *out_status = 500;
            return;
        }
    }

    if (hu_json_buf_append_raw(&buf, "]}", 2) != HU_OK) {
        hu_json_buf_free(&buf);
        *out_status = 500;
        return;
    }

    size_t n = buf.len + 1;
    char *body = (char *)alloc->alloc(alloc->ctx, n);
    if (!body) {
        hu_json_buf_free(&buf);
        *out_status = 500;
        return;
    }
    memcpy(body, buf.ptr, buf.len);
    body[buf.len] = '\0';
    *out_body = body;
    *out_body_len = buf.len;
    hu_json_buf_free(&buf);
}
