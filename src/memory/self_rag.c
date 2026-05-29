#include "human/memory/self_rag.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include <stdatomic.h>
#include <string.h>

hu_srag_config_t hu_srag_config_default(void) {
    hu_srag_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = true;
    cfg.confidence_threshold = 0.7;
    cfg.provider = NULL;
    return cfg;
}

static bool srag_starts_with_ci(const char *str, size_t str_len, const char *prefix) {
    size_t plen = strlen(prefix);
    if (plen > str_len)
        return false;
    for (size_t i = 0; i < plen; i++) {
        char a = str[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + 32);
        if (a != b)
            return false;
    }
    return true;
}

static bool is_greeting(const char *query, size_t query_len) {
    static const char *greetings[] = {"hi", "hello", "hey", "thanks", "bye", "ok", "thank you"};
    for (size_t i = 0; i < sizeof(greetings) / sizeof(greetings[0]); i++) {
        size_t glen = strlen(greetings[i]);
        if (query_len == glen && hu_str_contains_ci_cstr(query, query_len, greetings[i]))
            return true;
        if (query_len > glen && srag_starts_with_ci(query, query_len, greetings[i]) &&
            (query[glen] == ' ' || query[glen] == '!' || query[glen] == '.'))
            return true;
    }
    return false;
}

hu_error_t hu_srag_should_retrieve(hu_allocator_t *alloc, const hu_srag_config_t *config,
                                   const char *query, size_t query_len, const char *history,
                                   size_t history_len, hu_srag_assessment_t *out) {
    (void)alloc;
    (void)history;
    (void)history_len;

    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (!config || !config->enabled) {
        static atomic_bool warned_self_rag_disabled = false;
        hu_log_info_once(&warned_self_rag_disabled, "self_rag", NULL,
                         "self-RAG disabled — set self_rag.enabled=true in "
                         "config.json to enable adaptive retrieval decisions "
                         "(defaulting to always-retrieve at confidence 0.5)");
        out->decision = HU_SRAG_RETRIEVE;
        out->confidence = 0.5;
        return HU_OK;
    }

    if (!query || query_len == 0) {
        out->decision = HU_SRAG_NO_RETRIEVAL;
        out->confidence = 1.0;
        return HU_OK;
    }

    if (is_greeting(query, query_len)) {
        out->decision = HU_SRAG_NO_RETRIEVAL;
        out->confidence = 0.9;
        return HU_OK;
    }

    static const char *creative[] = {"write ",   "generate ", "brainstorm ",
                                     "imagine ", "create a ", "compose "};
    for (size_t i = 0; i < sizeof(creative) / sizeof(creative[0]); i++) {
        if (srag_starts_with_ci(query, query_len, creative[i])) {
            out->decision = HU_SRAG_NO_RETRIEVAL;
            out->confidence = 0.8;
            out->is_creative_query = true;
            return HU_OK;
        }
    }

    static const char *personal[] = {"my ",         "I ",           "remember when",
                                     "you told me", "we discussed", "my name"};
    for (size_t i = 0; i < sizeof(personal) / sizeof(personal[0]); i++) {
        if (hu_str_contains_ci_cstr(query, query_len, personal[i])) {
            out->decision = HU_SRAG_RETRIEVE;
            out->confidence = 0.9;
            out->is_personal_query = true;
            return HU_OK;
        }
    }

    static const char *temporal[] = {"yesterday", "last week", "ago", "recently", "last month"};
    for (size_t i = 0; i < sizeof(temporal) / sizeof(temporal[0]); i++) {
        if (hu_str_contains_ci_cstr(query, query_len, temporal[i])) {
            out->decision = HU_SRAG_RETRIEVE_AND_VERIFY;
            out->confidence = 0.85;
            out->has_temporal_marker = true;
            return HU_OK;
        }
    }

    static const char *factual[] = {"what ",    "who ",     "when ",   "where ", "how does",
                                    "how many", "how much", "explain", "define"};
    for (size_t i = 0; i < sizeof(factual) / sizeof(factual[0]); i++) {
        if (srag_starts_with_ci(query, query_len, factual[i])) {
            out->decision = HU_SRAG_RETRIEVE_AND_VERIFY;
            out->confidence = 0.7;
            out->is_factual_query = true;
            return HU_OK;
        }
    }

    out->decision = HU_SRAG_RETRIEVE;
    out->confidence = 0.5;
    return HU_OK;
}

hu_error_t hu_srag_verify_relevance(hu_allocator_t *alloc, const hu_srag_config_t *config,
                                    const char *query, size_t query_len, const char *retrieved,
                                    size_t retrieved_len, double *relevance_score,
                                    bool *should_use) {
    (void)alloc;
    (void)config;

    if (!query || !retrieved || !relevance_score || !should_use)
        return HU_ERR_INVALID_ARGUMENT;
    if (query_len == 0 || retrieved_len == 0) {
        *relevance_score = 0.0;
        *should_use = false;
        return HU_OK;
    }

    size_t query_words = 0;
    size_t overlap = 0;
    const char *p = query;
    const char *end = query + query_len;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        if (p >= end)
            break;
        const char *ws = p;
        while (p < end && *p != ' ' && *p != '\t')
            p++;
        size_t wlen = (size_t)(p - ws);
        if (wlen <= 2) {
            query_words++;
            continue;
        }
        query_words++;
        for (size_t i = 0; i + wlen <= retrieved_len; i++) {
            bool match = true;
            for (size_t j = 0; j < wlen; j++) {
                char a = retrieved[i + j];
                char b = ws[j];
                if (a >= 'A' && a <= 'Z')
                    a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z')
                    b = (char)(b + 32);
                if (a != b) {
                    match = false;
                    break;
                }
            }
            if (match) {
                overlap++;
                break;
            }
        }
    }

    *relevance_score = query_words > 0 ? (double)overlap / (double)query_words : 0.0;
    *should_use = *relevance_score >= 0.2;
    return HU_OK;
}
