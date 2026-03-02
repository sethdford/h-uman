#include "seaclaw/memory/retrieval/adaptive.h"
#include <ctype.h>
#include <string.h>

static int is_question_query(const char *query, size_t len) {
    static const char *prefixes[] = {
        "what ", "how ", "why ", "when ", "where ", "who ",
        "which ", "can ", "could ", "does ", "do ", "is ", "are ",
    };
    if (len == 0) return 0;
    char lower[9];
    size_t check = len < 8 ? len : 8;
    for (size_t i = 0; i < check; i++) {
        char c = query[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    lower[check] = '\0';
    for (size_t p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++) {
        size_t pl = strlen(prefixes[p]);
        if (pl <= check && strncmp(lower, prefixes[p], pl) == 0) return 1;
    }
    return 0;
}

sc_query_analysis_t sc_adaptive_analyze_query(const char *query, size_t query_len,
    const sc_adaptive_config_t *config) {
    sc_query_analysis_t out = {
        .token_count = 0,
        .has_special_chars = false,
        .is_question = false,
        .avg_token_length = 0.0f,
        .recommended_strategy = SC_ADAPTIVE_HYBRID,
    };

    if (!config || !config->enabled) return out;

    unsigned token_count = 0;
    unsigned total_len = 0;
    int has_special = 0;

    const char *p = query;
    const char *end = query + query_len;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end) break;
        const char *start = p;
        while (p < end && *p != ' ' && *p != '\t') {
            if (!has_special && (*p == '_' || *p == '.' || *p == '/' || *p == '\\' || *p == ':' || *p == '-'))
                has_special = 1;
            p++;
        }
        token_count++;
        total_len += (unsigned)(p - start);
    }

    out.token_count = token_count;
    out.has_special_chars = (bool)has_special;
    out.is_question = (bool)is_question_query(query, query_len);

    if (token_count == 0) {
        out.recommended_strategy = SC_ADAPTIVE_KEYWORD_ONLY;
        return out;
    }

    out.avg_token_length = (float)total_len / (float)token_count;

    if (has_special)
        out.recommended_strategy = SC_ADAPTIVE_KEYWORD_ONLY;
    else if (token_count <= config->keyword_max_tokens)
        out.recommended_strategy = SC_ADAPTIVE_KEYWORD_ONLY;
    else if (out.is_question && token_count >= config->vector_min_tokens)
        out.recommended_strategy = SC_ADAPTIVE_VECTOR_ONLY;
    else if (token_count >= config->vector_min_tokens)
        out.recommended_strategy = SC_ADAPTIVE_HYBRID;
    else
        out.recommended_strategy = SC_ADAPTIVE_HYBRID;

    return out;
}
