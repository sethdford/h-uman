#include "human/eval_judge.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t judge_cache_key(const char *actual, size_t actual_len, const char *expected,
                                size_t expected_len, const char *rubric, size_t rubric_len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < actual_len; i++) {
        h ^= (uint64_t)(unsigned char)actual[i];
        h *= 1099511628211ULL;
    }
    h ^= 0xFF;
    h *= 1099511628211ULL;
    for (size_t i = 0; i < expected_len; i++) {
        h ^= (uint64_t)(unsigned char)expected[i];
        h *= 1099511628211ULL;
    }
    if (rubric && rubric_len > 0) {
        h ^= 0xFE;
        h *= 1099511628211ULL;
        for (size_t i = 0; i < rubric_len; i++) {
            h ^= (uint64_t)(unsigned char)rubric[i];
            h *= 1099511628211ULL;
        }
    }
    return h;
}

static hu_eval_judge_result_t dup_result(hu_allocator_t *alloc, const hu_eval_judge_result_t *src) {
    hu_eval_judge_result_t dst = *src;
    if (src->reasoning && src->reasoning_len > 0) {
        dst.reasoning = hu_strndup(alloc, src->reasoning, src->reasoning_len);
        if (!dst.reasoning)
            dst.reasoning_len = 0;
    }
    return dst;
}

hu_error_t hu_eval_judge_cache_create(hu_allocator_t *alloc, hu_eval_judge_cache_t *cache) {
    if (!alloc || !cache)
        return HU_ERR_INVALID_ARGUMENT;
    cache->alloc = alloc;
    memset(cache->slots, 0, sizeof(cache->slots));
    return HU_OK;
}

bool hu_eval_judge_cache_lookup(hu_eval_judge_cache_t *cache, const char *actual, size_t actual_len,
                                const char *expected, size_t expected_len, const char *rubric,
                                size_t rubric_len, hu_eval_judge_result_t *out) {
    if (!cache || !actual || !expected || !out)
        return false;
    uint64_t h = judge_cache_key(actual, actual_len, expected, expected_len, rubric, rubric_len);
    size_t slot = (size_t)(h % HU_EVAL_JUDGE_CACHE_SLOTS);
    if (cache->slots[slot].occupied && cache->slots[slot].hash == h) {
        *out = dup_result(cache->alloc, &cache->slots[slot].result);
        return true;
    }
    return false;
}

static void cache_store(hu_eval_judge_cache_t *cache, const char *actual, size_t actual_len,
                        const char *expected, size_t expected_len, const char *rubric,
                        size_t rubric_len, const hu_eval_judge_result_t *result) {
    if (!cache)
        return;
    uint64_t h = judge_cache_key(actual, actual_len, expected, expected_len, rubric, rubric_len);
    size_t slot = (size_t)(h % HU_EVAL_JUDGE_CACHE_SLOTS);
    if (cache->slots[slot].occupied && cache->slots[slot].result.reasoning) {
        cache->alloc->free(cache->alloc->ctx, cache->slots[slot].result.reasoning,
                           cache->slots[slot].result.reasoning_len + 1);
    }
    cache->slots[slot].hash = h;
    cache->slots[slot].occupied = true;
    cache->slots[slot].result = dup_result(cache->alloc, result);
}

void hu_eval_judge_cache_destroy(hu_eval_judge_cache_t *cache) {
    if (!cache)
        return;
    for (size_t i = 0; i < HU_EVAL_JUDGE_CACHE_SLOTS; i++) {
        if (cache->slots[i].occupied && cache->slots[i].result.reasoning) {
            cache->alloc->free(cache->alloc->ctx, cache->slots[i].result.reasoning,
                               cache->slots[i].result.reasoning_len + 1);
            cache->slots[i].result.reasoning = NULL;
        }
    }
    memset(cache->slots, 0, sizeof(cache->slots));
}

#if !defined(HU_IS_TEST) || !HU_IS_TEST
static int parse_score_1to5(const char *start, const char *end) {
    const char *key = "\"score\"";
    const char *k = strstr(start, key);
    if (!k || k >= end)
        return 0;
    const char *colon = strchr(k + strlen(key), ':');
    if (!colon || colon >= end)
        return 0;
    const char *vs = colon + 1;
    while (vs < end && (*vs == ' ' || *vs == '\t'))
        vs++;
    int v = 0;
    (void)sscanf(vs, "%d", &v);
    if (v < 1)
        v = 1;
    if (v > 5)
        v = 5;
    return v;
}

static char *parse_reasoning(hu_allocator_t *alloc, const char *start, const char *end,
                             size_t *out_len) {
    const char *key = "\"reasoning\"";
    const char *k = strstr(start, key);
    if (!k || k >= end) {
        key = "\"reason\"";
        k = strstr(start, key);
    }
    if (!k || k >= end) {
        *out_len = 0;
        return NULL;
    }
    const char *colon = strchr(k + strlen(key), ':');
    if (!colon || colon >= end) {
        *out_len = 0;
        return NULL;
    }
    const char *qs = strchr(colon + 1, '"');
    if (!qs || qs >= end) {
        *out_len = 0;
        return NULL;
    }
    qs++;
    const char *qe = qs;
    while (qe < end && *qe != '"') {
        if (*qe == '\\')
            qe++;
        qe++;
    }
    size_t len = (size_t)(qe - qs);
    char *s = hu_strndup(alloc, qs, len);
    *out_len = len;
    return s;
}
#endif /* !HU_IS_TEST */

static size_t count_words_judge(const char *s, size_t len) {
    size_t c = 0;
    const char *p = s;
    const char *end = s + len;
    while (p < end) {
        while (p < end && isspace((unsigned char)*p))
            p++;
        if (p >= end)
            break;
        c++;
        while (p < end && !isspace((unsigned char)*p))
            p++;
    }
    return c;
}

static size_t count_word_matches(const char *actual, size_t actual_len, const char *expected,
                                 size_t expected_len) {
    size_t count = 0;
    const char *p = expected;
    const char *end = expected + expected_len;
    while (p < end) {
        while (p < end && isspace((unsigned char)*p))
            p++;
        if (p >= end)
            break;
        const char *ws = p;
        while (p < end && !isspace((unsigned char)*p))
            p++;
        size_t wlen = (size_t)(p - ws);
        if (wlen > 0 && hu_str_contains_ci(actual, actual_len, ws, wlen))
            count++;
    }
    return count;
}

static void heuristic_judge_1to5(const char *actual, size_t actual_len, const char *expected,
                                 size_t expected_len, int *score, double *raw) {
    if (hu_str_contains_ci(actual, actual_len, expected, expected_len)) {
        *score = 5;
        *raw = 1.0;
        return;
    }
    size_t exp_words = count_words_judge(expected, expected_len);
    if (exp_words == 0) {
        *score = 1;
        *raw = 0.0;
        return;
    }
    size_t matched = count_word_matches(actual, actual_len, expected, expected_len);
    double ratio = (double)matched / (double)exp_words;
    if (ratio >= 0.9)
        *score = 5;
    else if (ratio >= 0.7)
        *score = 4;
    else if (ratio >= 0.5)
        *score = 3;
    else if (ratio >= 0.25)
        *score = 2;
    else
        *score = 1;
    *raw = ratio;
}

hu_error_t hu_eval_judge_check(hu_allocator_t *alloc, hu_provider_t *provider, const char *model,
                               size_t model_len, const char *question, size_t question_len,
                               const char *actual, size_t actual_len, const char *expected,
                               size_t expected_len, const char *rubric, size_t rubric_len,
                               int pass_threshold, hu_eval_judge_cache_t *cache,
                               hu_eval_judge_result_t *out) {
    if (!alloc || !actual || !expected || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    if (pass_threshold < 1 || pass_threshold > 5)
        pass_threshold = 3;

    if (cache && hu_eval_judge_cache_lookup(cache, actual, actual_len, expected, expected_len,
                                            rubric, rubric_len, out)) {
        out->passed = (out->score >= pass_threshold);
        return HU_OK;
    }

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)provider;
    (void)model;
    (void)model_len;
    (void)question;
    (void)question_len;
    (void)rubric;
    (void)rubric_len;
    heuristic_judge_1to5(actual, actual_len, expected, expected_len, &out->score, &out->raw_score);
    out->passed = (out->score >= pass_threshold);
    out->reasoning = hu_strndup(alloc, "heuristic", 9);
    out->reasoning_len = 9;
    if (cache)
        cache_store(cache, actual, actual_len, expected, expected_len, rubric, rubric_len, out);
    return HU_OK;
#else
    if (!provider || !provider->vtable || !provider->vtable->chat_with_system) {
        heuristic_judge_1to5(actual, actual_len, expected, expected_len, &out->score,
                             &out->raw_score);
        out->passed = (out->score >= pass_threshold);
        out->reasoning = hu_strndup(alloc, "heuristic fallback", 18);
        out->reasoning_len = 18;
        if (cache)
            cache_store(cache, actual, actual_len, expected, expected_len, rubric, rubric_len, out);
        return HU_OK;
    }
    {
        const char *rubric_section = "";
        char rubric_buf[512];
        if (rubric && rubric_len > 0) {
            int rl = snprintf(rubric_buf, sizeof(rubric_buf), "\n\nRubric:\n%.*s\n",
                              (int)(rubric_len < 400 ? rubric_len : 400), rubric);
            if (rl > 0 && (size_t)rl < sizeof(rubric_buf))
                rubric_section = rubric_buf;
        }

        char *user_msg =
            hu_sprintf(alloc,
                       "Evaluate the following response against the expected answer.%s\n\n"
                       "%s%.*s%s"
                       "Expected answer: %.*s\n\n"
                       "Actual response: %.*s\n\n"
                       "Score 1-5:\n"
                       "  5 = Fully correct, complete, well-articulated\n"
                       "  4 = Mostly correct with minor gaps\n"
                       "  3 = Partially correct, key elements present\n"
                       "  2 = Minimally relevant, major gaps\n"
                       "  1 = Incorrect or irrelevant\n\n"
                       "Respond with ONLY a JSON object:\n"
                       "{\"score\": <1-5>, \"reasoning\": \"<brief explanation>\"}",
                       rubric_section, (question && question_len > 0) ? "Question: " : "",
                       (int)(question_len < 500 ? question_len : 500), question ? question : "",
                       (question && question_len > 0) ? "\n\n" : "",
                       (int)(expected_len < 1000 ? expected_len : 1000), expected,
                       (int)(actual_len < 2000 ? actual_len : 2000), actual);

        if (!user_msg) {
            heuristic_judge_1to5(actual, actual_len, expected, expected_len, &out->score,
                                 &out->raw_score);
            out->passed = (out->score >= pass_threshold);
            return HU_OK;
        }

        const char *sys = "You are a strict evaluation judge. Score responses 1-5 against the "
                          "expected answer. Output only valid JSON.";
        char *response = NULL;
        size_t response_len = 0;
        hu_error_t err = provider->vtable->chat_with_system(
            provider->ctx, alloc, sys, strlen(sys), user_msg, strlen(user_msg), model ? model : "",
            model_len, 0.0, &response, &response_len);
        alloc->free(alloc->ctx, user_msg, strlen(user_msg) + 1);

        if (err != HU_OK || !response) {
            heuristic_judge_1to5(actual, actual_len, expected, expected_len, &out->score,
                                 &out->raw_score);
            out->passed = (out->score >= pass_threshold);
            if (response)
                alloc->free(alloc->ctx, response, response_len + 1);
            return HU_OK;
        }

        out->score = parse_score_1to5(response, response + response_len);
        out->raw_score = (double)(out->score - 1) / 4.0;
        out->passed = (out->score >= pass_threshold);
        out->reasoning =
            parse_reasoning(alloc, response, response + response_len, &out->reasoning_len);
        alloc->free(alloc->ctx, response, response_len + 1);

        if (cache)
            cache_store(cache, actual, actual_len, expected, expected_len, rubric, rubric_len, out);
        return HU_OK;
    }
#endif
}

void hu_eval_judge_result_free(hu_allocator_t *alloc, hu_eval_judge_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->reasoning) {
        alloc->free(alloc->ctx, result->reasoning, result->reasoning_len + 1);
        result->reasoning = NULL;
        result->reasoning_len = 0;
    }
}
