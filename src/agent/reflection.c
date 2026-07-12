#include "human/agent/reflection.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <string.h>

static void reflection_result_init_axes(hu_reflection_result_t *r) {
    memset(r, 0, sizeof(*r));
    r->accuracy = -1.0;
    r->relevance = -1.0;
    r->tone = -1.0;
    r->completeness = -1.0;
    r->conciseness = -1.0;
}

static double reflection_clamp_axis(double v) {
    if (v < -0.5)
        return -1.0;
    if (v > 1.0)
        return 1.0;
    return v;
}

static hu_reflection_quality_t reflection_quality_from_string(const char *q) {
    if (!q)
        return HU_QUALITY_ACCEPTABLE;
    if (hu_str_contains_ci(q, strlen(q), "needs_retry", 11))
        return HU_QUALITY_NEEDS_RETRY;
    if (hu_str_contains_ci(q, strlen(q), "acceptable", 10))
        return HU_QUALITY_ACCEPTABLE;
    if (hu_str_contains_ci(q, strlen(q), "good", 4))
        return HU_QUALITY_GOOD;
    return HU_QUALITY_ACCEPTABLE;
}

static hu_error_t reflection_fill_heuristic(hu_reflection_result_t *out, const char *user_query,
                                            size_t user_query_len, const char *response,
                                            size_t response_len) {
    reflection_result_init_axes(out);
    out->quality =
        hu_reflection_evaluate(user_query, user_query_len, response, response_len, NULL);
    return HU_OK;
}

hu_reflection_quality_t hu_reflection_evaluate(const char *user_query, size_t user_query_len,
                                               const char *response, size_t response_len,
                                               const hu_reflection_config_t *config) {
    if (!response || response_len == 0)
        return HU_QUALITY_NEEDS_RETRY;

    /* Check for empty/trivial responses */
    if (response_len < 10)
        return HU_QUALITY_NEEDS_RETRY;

    /* Check for known failure patterns */
    if (hu_str_contains_ci(response, response_len, "i cannot", 8) ||
        hu_str_contains_ci(response, response_len, "i can't", 7) ||
        hu_str_contains_ci(response, response_len, "i'm unable", 10) ||
        hu_str_contains_ci(response, response_len, "as an ai", 8))
        return HU_QUALITY_ACCEPTABLE;

    /* If user asked a question, check response addresses it */
    bool user_has_question = false;
    if (user_query && user_query_len > 0) {
        for (size_t i = 0; i < user_query_len; i++) {
            if (user_query[i] == '?') {
                user_has_question = true;
                break;
            }
        }
    }

    if (user_has_question && response_len < 30)
        return HU_QUALITY_ACCEPTABLE;

    /* Min response tokens check (approximate tokens as words) */
    if (config && config->min_response_tokens > 0) {
        int approx_words = 1;
        for (size_t i = 0; i < response_len; i++)
            if (response[i] == ' ')
                approx_words++;
        if (approx_words < config->min_response_tokens)
            return HU_QUALITY_ACCEPTABLE;
    }

    return HU_QUALITY_GOOD;
}

hu_error_t hu_reflection_build_retry_prompt(hu_allocator_t *alloc, char **out_prompt,
                                            size_t *out_prompt_len) {
    if (!alloc || !out_prompt)
        return HU_ERR_INVALID_ARGUMENT;
    static const char instruction[] =
        "That draft isn't quite right for this conversation. Rewrite your "
        "reply to their last message from scratch: natural, in your own "
        "voice, matching their energy and length. Output ONLY the message "
        "text you would send. Do not judge, explain, or compare drafts.";
    size_t len = sizeof(instruction) - 1;
    char *buf = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, instruction, len + 1);
    *out_prompt = buf;
    if (out_prompt_len)
        *out_prompt_len = len;
    return HU_OK;
}

hu_error_t hu_reflection_build_critique_prompt(hu_allocator_t *alloc, const char *user_query,
                                               size_t user_query_len, const char *response,
                                               size_t response_len, char **out_prompt,
                                               size_t *out_prompt_len) {
    if (!alloc || !out_prompt)
        return HU_ERR_INVALID_ARGUMENT;

    const char *prefix =
        "Evaluate the following response. Score it as GOOD, ACCEPTABLE, or NEEDS_RETRY. "
        "If NEEDS_RETRY, explain what should be improved.\n\n"
        "User query: ";
    size_t prefix_len = strlen(prefix);

    const char *mid = "\n\nResponse: ";
    size_t mid_len = strlen(mid);

    const char *suffix = "\n\nEvaluation:";
    size_t suffix_len = strlen(suffix);

    size_t total = prefix_len + user_query_len + mid_len + response_len + suffix_len;
    char *buf = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    memcpy(buf + pos, prefix, prefix_len);
    pos += prefix_len;
    if (user_query && user_query_len > 0) {
        memcpy(buf + pos, user_query, user_query_len);
        pos += user_query_len;
    }
    memcpy(buf + pos, mid, mid_len);
    pos += mid_len;
    if (response && response_len > 0) {
        memcpy(buf + pos, response, response_len);
        pos += response_len;
    }
    memcpy(buf + pos, suffix, suffix_len);
    pos += suffix_len;
    buf[pos] = '\0';

    *out_prompt = buf;
    if (out_prompt_len)
        *out_prompt_len = pos;
    return HU_OK;
}

static hu_error_t reflection_try_parse_structured_json(const char *text, size_t text_len,
                                                         hu_reflection_result_t *out) {
    hu_allocator_t stab_alloc = hu_system_allocator();
    hu_allocator_t *stab = &stab_alloc;

    const char *end = text + text_len;
    const char *brace = memchr(text, '{', text_len);
    if (!brace)
        return HU_ERR_INVALID_ARGUMENT;
    size_t slice_len = (size_t)(end - brace);

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(stab, brace, slice_len, &root);
    if (err != HU_OK || !root)
        return err != HU_OK ? err : HU_ERR_INVALID_ARGUMENT;
    if (root->type != HU_JSON_OBJECT) {
        hu_json_free(stab, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    reflection_result_init_axes(out);

    const char *q = hu_json_get_string(root, "quality");
    out->quality = reflection_quality_from_string(q);

    const char *fb = hu_json_get_string(root, "feedback");
    if (fb) {
        out->feedback = hu_strdup(stab, fb);
        if (!out->feedback) {
            hu_json_free(stab, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
        out->feedback_len = strlen(out->feedback);
    }

    out->accuracy = reflection_clamp_axis(hu_json_get_number(root, "accuracy", -1.0));
    out->relevance = reflection_clamp_axis(hu_json_get_number(root, "relevance", -1.0));
    out->tone = reflection_clamp_axis(hu_json_get_number(root, "tone", -1.0));
    out->completeness = reflection_clamp_axis(hu_json_get_number(root, "completeness", -1.0));
    out->conciseness = reflection_clamp_axis(hu_json_get_number(root, "conciseness", -1.0));

    hu_json_free(stab, root);
    return HU_OK;
}

hu_error_t hu_reflection_evaluate_structured(hu_allocator_t *alloc, hu_provider_t *provider,
                                             const char *model, size_t model_len,
                                             const char *user_query, size_t user_query_len,
                                             const char *response, size_t response_len,
                                             hu_reflection_result_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    if (!provider || !provider->vtable || !provider->vtable->chat_with_system)
        return reflection_fill_heuristic(out, user_query, user_query_len, response, response_len);

    char *critique = NULL;
    size_t critique_len = 0;
    hu_error_t err = hu_reflection_build_critique_prompt(
        alloc, user_query, user_query_len, response, response_len, &critique, &critique_len);
    if (err != HU_OK || !critique)
        return reflection_fill_heuristic(out, user_query, user_query_len, response, response_len);

    static const char sys[] =
        "You are a response quality evaluator. Evaluate the response on these axes.\n"
        "Return ONLY valid JSON:\n"
        "{\"quality\": \"GOOD|ACCEPTABLE|NEEDS_RETRY\",\n"
        " \"accuracy\": 0.0-1.0,\n"
        " \"relevance\": 0.0-1.0,\n"
        " \"tone\": 0.0-1.0,\n"
        " \"completeness\": 0.0-1.0,\n"
        " \"conciseness\": 0.0-1.0,\n"
        " \"feedback\": \"specific improvement suggestion\"}\n"
        "Score axes as -1 if not applicable.";

    char *llm_out = NULL;
    size_t llm_out_len = 0;
    err = provider->vtable->chat_with_system(
        provider->ctx, alloc, sys, sizeof(sys) - 1, critique, critique_len,
        model && model_len > 0 ? model : "gpt-4o-mini",
        model && model_len > 0 ? model_len : 11, 0.0, &llm_out, &llm_out_len);
    alloc->free(alloc->ctx, critique, critique_len + 1);

    if (err != HU_OK || !llm_out || llm_out_len == 0) {
        if (llm_out)
            alloc->free(alloc->ctx, llm_out, llm_out_len + 1);
        return reflection_fill_heuristic(out, user_query, user_query_len, response, response_len);
    }

    hu_reflection_result_t parsed;
    reflection_result_init_axes(&parsed);
    hu_error_t perr = reflection_try_parse_structured_json(llm_out, llm_out_len, &parsed);
    alloc->free(alloc->ctx, llm_out, llm_out_len + 1);

    if (perr != HU_OK) {
        hu_allocator_t sys_alloc = hu_system_allocator();
        hu_reflection_result_free(&sys_alloc, &parsed);
        return reflection_fill_heuristic(out, user_query, user_query_len, response, response_len);
    }

    *out = parsed;
    return HU_OK;
}

hu_reflection_quality_t hu_reflection_evaluate_llm(hu_allocator_t *alloc, hu_provider_t *provider,
                                                   const char *user_query, size_t user_query_len,
                                                   const char *response, size_t response_len,
                                                   hu_reflection_quality_t heuristic_quality) {
    if (!alloc || !provider || !provider->vtable || !provider->vtable->chat_with_system)
        return heuristic_quality;

    hu_reflection_result_t tmp;
    hu_error_t err = hu_reflection_evaluate_structured(alloc, provider, "gpt-4o-mini", 11,
                                                       user_query, user_query_len, response,
                                                       response_len, &tmp);
    hu_reflection_quality_t q = heuristic_quality;
    if (err == HU_OK)
        q = tmp.quality;
    {
        hu_allocator_t sys_alloc = hu_system_allocator();
        hu_reflection_result_free(&sys_alloc, &tmp);
    }
    return q;
}

void hu_reflection_result_free(hu_allocator_t *alloc, hu_reflection_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->feedback && result->feedback_len > 0)
        alloc->free(alloc->ctx, result->feedback, result->feedback_len + 1);
    result->feedback = NULL;
    result->feedback_len = 0;
}
