#include "human/providers/helpers.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <string.h>

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
