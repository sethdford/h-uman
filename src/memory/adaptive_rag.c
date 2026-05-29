#include "human/memory/adaptive_rag.h"
#include "human/core/string.h"
#include <string.h>

hu_error_t hu_adaptive_rag_create(hu_allocator_t *alloc,
#ifdef HU_ENABLE_SQLITE
                                  sqlite3 *db,
#else
                                  void *db,
#endif
                                  hu_adaptive_rag_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->alloc = alloc;
    out->db = db;
    for (int i = 0; i < HU_RAG_STRATEGY_COUNT; i++)
        out->strategy_weights[i] = 1.0;
    return HU_OK;
}

void hu_adaptive_rag_deinit(hu_adaptive_rag_t *rag) {
    if (rag)
        memset(rag, 0, sizeof(*rag));
}

static bool starts_with_ci(const char *str, size_t str_len, const char *prefix) {
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

hu_error_t hu_adaptive_rag_extract_features(const char *query, size_t query_len,
                                            hu_rag_query_features_t *features) {
    if (!features)
        return HU_ERR_INVALID_ARGUMENT;
    memset(features, 0, sizeof(*features));
    if (!query || query_len == 0)
        return HU_OK;

    size_t word_count = 0;
    size_t total_chars = 0;
    bool first_word = true;
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
        word_count++;
        total_chars += wlen;
        if (!first_word && ws[0] >= 'A' && ws[0] <= 'Z')
            features->has_entity_names = true;
        first_word = false;
    }
    features->word_count = word_count;
    features->avg_word_length = word_count > 0 ? (double)total_chars / (double)word_count : 0.0;

    static const char *temporal[] = {"yesterday", "last week", "last month", "ago", "recently", "today"};
    for (size_t i = 0; i < sizeof(temporal) / sizeof(temporal[0]); i++) {
        if (hu_str_contains_ci_cstr(query, query_len, temporal[i])) {
            features->has_temporal_marker = true;
            break;
        }
    }

    static const char *relational[] = {"who knows", "related to", "connected", "friends with", "works with"};
    for (size_t i = 0; i < sizeof(relational) / sizeof(relational[0]); i++) {
        if (hu_str_contains_ci_cstr(query, query_len, relational[i])) {
            features->has_relationship_query = true;
            break;
        }
    }

    static const char *factual[] = {"what is", "how does", "explain", "define", "when did"};
    for (size_t i = 0; i < sizeof(factual) / sizeof(factual[0]); i++) {
        if (starts_with_ci(query, query_len, factual[i])) {
            features->is_factual = true;
            break;
        }
    }

    static const char *personal[] = {"my ", "I ", "remember when", "you told me", "we discussed"};
    for (size_t i = 0; i < sizeof(personal) / sizeof(personal[0]); i++) {
        if (hu_str_contains_ci_cstr(query, query_len, personal[i])) {
            features->is_personal = true;
            break;
        }
    }

    return HU_OK;
}

hu_rag_strategy_t hu_adaptive_rag_select(hu_adaptive_rag_t *rag,
                                         const char *query, size_t query_len) {
    if (!rag || !query || query_len == 0) {
        if (rag) {
            rag->strategy_uses[HU_RAG_KEYWORD]++;
        }
        return HU_RAG_KEYWORD;
    }

    hu_rag_query_features_t f;
    hu_adaptive_rag_extract_features(query, query_len, &f);

    hu_rag_strategy_t selected = HU_RAG_SEMANTIC;

    if (f.has_relationship_query)
        selected = HU_RAG_GRAPH;
    else if (f.word_count <= 3 && !f.is_factual)
        selected = HU_RAG_KEYWORD;
    else if (f.is_factual && f.word_count >= 5)
        selected = HU_RAG_CORRECTIVE;
    else if (f.avg_word_length > 7.0)
        selected = HU_RAG_SEMANTIC;
    else if (f.word_count >= 8)
        selected = HU_RAG_HYBRID;
    else if (f.is_personal)
        selected = HU_RAG_HYBRID;

    if (rag->strategy_weights[selected] < 0.3) {
        hu_rag_strategy_t fallbacks[] = {HU_RAG_HYBRID, HU_RAG_SEMANTIC, HU_RAG_KEYWORD};
        for (size_t i = 0; i < 3; i++) {
            if (fallbacks[i] != selected && rag->strategy_weights[fallbacks[i]] >= 0.3) {
                selected = fallbacks[i];
                break;
            }
        }
    }

    rag->strategy_uses[selected]++;
    return selected;
}

hu_error_t hu_adaptive_rag_record_outcome(hu_adaptive_rag_t *rag,
                                          hu_rag_strategy_t strategy,
                                          double quality_score) {
    if (!rag || strategy < 0 || strategy >= HU_RAG_STRATEGY_COUNT)
        return HU_ERR_INVALID_ARGUMENT;
    rag->strategy_weights[strategy] = 0.9 * rag->strategy_weights[strategy] + 0.1 * quality_score;
    return HU_OK;
}

const char *hu_rag_strategy_str(hu_rag_strategy_t strategy) {
    switch (strategy) {
    case HU_RAG_NONE:       return "none";
    case HU_RAG_KEYWORD:    return "keyword";
    case HU_RAG_SEMANTIC:   return "semantic";
    case HU_RAG_HYBRID:     return "hybrid";
    case HU_RAG_GRAPH:      return "graph";
    case HU_RAG_CORRECTIVE: return "corrective";
    default:                return "unknown";
    }
}
