#include "human/agent/reflection.h"
#include "human/core/string.h"
#include <string.h>

static bool contains_ci(const char *s, size_t slen, const char *needle, size_t nlen) {
    if (nlen > slen)
        return false;
    for (size_t i = 0; i <= slen - nlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            char a = s[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z')
                a += 32;
            if (b >= 'A' && b <= 'Z')
                b += 32;
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
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
    if (contains_ci(response, response_len, "i cannot", 8) ||
        contains_ci(response, response_len, "i can't", 7) ||
        contains_ci(response, response_len, "i'm unable", 10) ||
        contains_ci(response, response_len, "as an ai", 8))
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

hu_reflection_quality_t hu_reflection_evaluate_llm(hu_allocator_t *alloc, hu_provider_t *provider,
                                                   const char *user_query, size_t user_query_len,
                                                   const char *response, size_t response_len,
                                                   hu_reflection_quality_t heuristic_quality) {
    if (!alloc || !provider || !provider->vtable || !provider->vtable->chat_with_system)
        return heuristic_quality;

    char *critique = NULL;
    size_t critique_len = 0;
    hu_error_t err = hu_reflection_build_critique_prompt(
        alloc, user_query, user_query_len, response, response_len, &critique, &critique_len);
    if (err != HU_OK || !critique)
        return heuristic_quality;

    static const char sys[] =
        "You are a response quality evaluator. Respond with exactly one word: "
        "GOOD, ACCEPTABLE, or NEEDS_RETRY. No other output.";

    char *llm_out = NULL;
    size_t llm_out_len = 0;
    err = provider->vtable->chat_with_system(provider->ctx, alloc, sys, sizeof(sys) - 1, critique,
                                             critique_len, "gpt-4o-mini", 11, 0.0, &llm_out,
                                             &llm_out_len);
    alloc->free(alloc->ctx, critique, critique_len + 1);

    if (err != HU_OK || !llm_out || llm_out_len == 0) {
        if (llm_out)
            alloc->free(alloc->ctx, llm_out, llm_out_len + 1);
        return heuristic_quality;
    }

    hu_reflection_quality_t result = heuristic_quality;
    if (contains_ci(llm_out, llm_out_len, "needs_retry", 11))
        result = HU_QUALITY_NEEDS_RETRY;
    else if (contains_ci(llm_out, llm_out_len, "good", 4))
        result = HU_QUALITY_GOOD;
    else if (contains_ci(llm_out, llm_out_len, "acceptable", 10))
        result = HU_QUALITY_ACCEPTABLE;

    alloc->free(alloc->ctx, llm_out, llm_out_len + 1);
    return result;
}

void hu_reflection_result_free(hu_allocator_t *alloc, hu_reflection_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->feedback && result->feedback_len > 0)
        alloc->free(alloc->ctx, result->feedback, result->feedback_len + 1);
    result->feedback = NULL;
    result->feedback_len = 0;
}
