#include "human/providers/helpers.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <string.h>

void hu_helpers_openai_choice_apply_logprobs(hu_json_value_t *choice, hu_chat_response_t *out) {
    if (!choice || !out)
        return;
    out->logprob_mean_valid = false;
    out->logprob_mean = 0.0f;
    hu_json_value_t *lp = hu_json_object_get(choice, "logprobs");
    if (!lp || lp->type != HU_JSON_OBJECT)
        return;
    hu_json_value_t *content = hu_json_object_get(lp, "content");
    if (!content || content->type != HU_JSON_ARRAY || content->data.array.len == 0)
        return;
    double sum = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < content->data.array.len; i++) {
        hu_json_value_t *tok = content->data.array.items[i];
        if (!tok || tok->type != HU_JSON_OBJECT)
            continue;
        double v = hu_json_get_number(tok, "logprob", -999.0);
        if (v > -900.0) {
            sum += v;
            n++;
        }
    }
    if (n > 0) {
        out->logprob_mean_valid = true;
        out->logprob_mean = (float)(sum / (double)n);
    }
}

void hu_chat_response_free(hu_allocator_t *alloc, hu_chat_response_t *resp) {
    if (!alloc || !resp)
        return;
    if (resp->content) {
        alloc->free(alloc->ctx, (void *)resp->content, resp->content_len + 1);
        /* Cast away const for free; caller must not use resp after this */
    }
    if (resp->model) {
        alloc->free(alloc->ctx, (void *)resp->model, resp->model_len + 1);
    }
    if (resp->reasoning_content) {
        alloc->free(alloc->ctx, (void *)resp->reasoning_content, resp->reasoning_content_len + 1);
    }
    if (resp->tool_calls && resp->tool_calls_count > 0) {
        for (size_t i = 0; i < resp->tool_calls_count; i++) {
            const hu_tool_call_t *tc = &resp->tool_calls[i];
            if (tc->id)
                alloc->free(alloc->ctx, (void *)tc->id, tc->id_len + 1);
            if (tc->name)
                alloc->free(alloc->ctx, (void *)tc->name, tc->name_len + 1);
            if (tc->arguments)
                alloc->free(alloc->ctx, (void *)tc->arguments, tc->arguments_len + 1);
        }
        alloc->free(alloc->ctx, (void *)resp->tool_calls,
                    resp->tool_calls_count * sizeof(hu_tool_call_t));
    }
    memset(resp, 0, sizeof(*resp));
}

void hu_stream_chat_result_free(hu_allocator_t *alloc, hu_stream_chat_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->content)
        alloc->free(alloc->ctx, (void *)result->content, result->content_len + 1);
    if (result->reasoning_content)
        alloc->free(alloc->ctx, (void *)result->reasoning_content, result->reasoning_content_len + 1);
    if (result->model)
        alloc->free(alloc->ctx, (void *)result->model, result->model_len + 1);
    if (result->tool_calls && result->tool_calls_count > 0) {
        for (size_t i = 0; i < result->tool_calls_count; i++) {
            const hu_tool_call_t *tc = &result->tool_calls[i];
            if (tc->id)
                alloc->free(alloc->ctx, (void *)tc->id, tc->id_len + 1);
            if (tc->name)
                alloc->free(alloc->ctx, (void *)tc->name, tc->name_len + 1);
            if (tc->arguments)
                alloc->free(alloc->ctx, (void *)tc->arguments, tc->arguments_len + 1);
        }
        alloc->free(alloc->ctx, (void *)result->tool_calls,
                    result->tool_calls_count * sizeof(hu_tool_call_t));
    }
    memset(result, 0, sizeof(*result));
}

bool hu_helpers_is_reasoning_model(const char *model, size_t model_len) {
    if (!model || model_len == 0)
        return false;
    if (model_len >= 2 && memcmp(model, "o1", 2) == 0)
        return true;
    if (model_len >= 2 && memcmp(model, "o3", 2) == 0)
        return true;
    if (model_len >= 7 && memcmp(model, "o4-mini", 7) == 0)
        return true;
    if (model_len >= 5 && memcmp(model, "gpt-5", 5) == 0)
        return true;
    if (model_len >= 10 && memcmp(model, "codex-mini", 10) == 0)
        return true;
    return false;
}

char *hu_helpers_extract_openai_content(hu_allocator_t *alloc, const char *body, size_t body_len) {
    hu_json_value_t *parsed = NULL;
    if (hu_json_parse(alloc, body, body_len, &parsed) != 0)
        return NULL;
    hu_json_value_t *choices = hu_json_object_get(parsed, "choices");
    if (!choices || choices->type != HU_JSON_ARRAY || choices->data.array.len == 0) {
        hu_json_free(alloc, parsed);
        return NULL;
    }
    hu_json_value_t *first = choices->data.array.items[0];
    hu_json_value_t *msg = hu_json_object_get(first, "message");
    if (!msg || msg->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, parsed);
        return NULL;
    }
    const char *content = hu_json_get_string(msg, "content");
    char *out = content ? hu_strndup(alloc, content, strlen(content)) : NULL;
    hu_json_free(alloc, parsed);
    return out;
}

char *hu_helpers_extract_anthropic_content(hu_allocator_t *alloc, const char *body,
                                           size_t body_len) {
    hu_json_value_t *parsed = NULL;
    if (hu_json_parse(alloc, body, body_len, &parsed) != 0)
        return NULL;
    hu_json_value_t *content = hu_json_object_get(parsed, "content");
    if (!content || content->type != HU_JSON_ARRAY || content->data.array.len == 0) {
        hu_json_free(alloc, parsed);
        return NULL;
    }
    hu_json_value_t *first = content->data.array.items[0];
    const char *text = hu_json_get_string(first, "text");
    char *out = text ? hu_strndup(alloc, text, strlen(text)) : NULL;
    hu_json_free(alloc, parsed);
    return out;
}

/* W13 — adapter loading helpers. Each NULL-checks the vtable triple
 * (load_adapter, unload_adapter, active_adapter) and returns
 * HU_ERR_NOT_SUPPORTED when the provider doesn't implement adapters
 * (the cloud-API common case). Returning a structured error here lets
 * callers cleanly fall back to the no-adapter path without crashing. */
hu_error_t hu_provider_load_adapter(hu_provider_t *provider, hu_allocator_t *alloc,
                                    const char *adapter_path, size_t adapter_path_len,
                                    const char *adapter_id, size_t adapter_id_len) {
    if (!provider || !provider->vtable || !alloc || !adapter_path || adapter_path_len == 0 ||
        !adapter_id || adapter_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!provider->vtable->load_adapter)
        return HU_ERR_NOT_SUPPORTED;
    return provider->vtable->load_adapter(provider->ctx, alloc, adapter_path, adapter_path_len,
                                          adapter_id, adapter_id_len);
}

hu_error_t hu_provider_unload_adapter(hu_provider_t *provider, const char *adapter_id,
                                      size_t adapter_id_len) {
    if (!provider || !provider->vtable || !adapter_id || adapter_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!provider->vtable->unload_adapter)
        return HU_ERR_NOT_SUPPORTED;
    return provider->vtable->unload_adapter(provider->ctx, adapter_id, adapter_id_len);
}

const char *hu_provider_active_adapter(const hu_provider_t *provider) {
    if (!provider || !provider->vtable || !provider->vtable->active_adapter)
        return NULL;
    return provider->vtable->active_adapter(provider->ctx);
}

/* SOTA-2026 init-01 — activation steering helper. Same safe-no-op
 * pattern as `hu_provider_load_adapter`: NULL vtable slot →
 * HU_ERR_NOT_SUPPORTED, callers fall back to prompt-side directive.
 *
 * Bounds checks run BEFORE dispatch so providers can assume
 * `dim <= HU_STEERING_VEC_MAX_DIM` and `vec != NULL when dim > 0`.
 * The (NULL, 0) reset form is forwarded verbatim — providers that
 * implement steering use it to drop any retained vector and fall
 * back to base behavior on the next chat(). */
hu_error_t hu_provider_apply_steering(hu_provider_t *provider, const float *vec, size_t dim) {
    if (!provider || !provider->vtable)
        return HU_ERR_INVALID_ARGUMENT;
    if (dim > HU_STEERING_VEC_MAX_DIM)
        return HU_ERR_INVALID_ARGUMENT;
    if (dim > 0 && !vec)
        return HU_ERR_INVALID_ARGUMENT;
    if (!provider->vtable->apply_steering)
        return HU_ERR_NOT_SUPPORTED;
    return provider->vtable->apply_steering(provider->ctx, vec, dim);
}
