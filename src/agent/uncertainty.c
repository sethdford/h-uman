#include "human/agent/uncertainty.h"
#include "human/core/string.h"
#include <ctype.h>
#include <string.h>

hu_error_t hu_uncertainty_evaluate(hu_allocator_t *alloc, const hu_uncertainty_signals_t *signals,
                                   hu_uncertainty_result_t *result) {
    if (!alloc || !signals || !result)
        return HU_ERR_INVALID_ARGUMENT;

    memset(result, 0, sizeof(*result));

    /* Phase 1 Task 2: soft-blended confidence score combining heuristic and real signals.
     * Strategy: interpolate between heuristic_score (old) and real_signal_score (new)
     * based on evidence_weight, which ramps 0→1 as fact_count goes 0→3.
     */

    /* Heuristic score (original method — backward compatibility when no real signals) */
    double heuristic_score = 0.0;
    heuristic_score += signals->retrieval_coverage * 0.3;
    if (signals->tool_results_count > 0)
        heuristic_score += 0.2;
    if (signals->has_citations)
        heuristic_score += 0.15;
    if (!signals->has_hedging_language)
        heuristic_score += 0.15;
    if (signals->memory_results_count >= 3)
        heuristic_score += 0.1;
    else
        heuristic_score += (double)signals->memory_results_count * 0.033;
    if (!signals->is_factual_query)
        heuristic_score += 0.1;
    if (heuristic_score > 1.0)
        heuristic_score = 1.0;
    if (heuristic_score < 0.0)
        heuristic_score = 0.0;

    /* Evidence weight: ramp from 0 at fact_count=0 to 1 at fact_count=3+
     * Formulation: min(fact_count / 3.0, 1.0) ensures smooth transition */
    double evidence_weight = (signals->fact_count >= 3) ? 1.0 : (signals->fact_count / 3.0);

    /* Real-signal score: grounded_confidence with contradiction penalty */
    double real_signal_score = signals->grounded_confidence;
    if (signals->contradiction_present)
        real_signal_score -= 0.15; /* 15pp penalty for contradictions */
    if (real_signal_score < 0.0)
        real_signal_score = 0.0;

    /* Soft blend: interpolate between heuristic and real signal scores */
    double blended_score =
        (1.0 - evidence_weight) * heuristic_score + evidence_weight * real_signal_score;

    /* Verbalized confidence gating: asymmetric rule
     * - Trust low claims: if model says confidence is lower, apply it
     * - Distrust high claims: if model says confidence is higher, ignore it (stay with blended)
     */
    double score = blended_score;
    if (signals->has_verbalized) {
        if (signals->verbalized_confidence < blended_score) {
            /* Model self-reports lower confidence: blend it in */
            /* weight: 60% blended (our assessment) + 40% verbalized (model's reported doubt) */
            score = 0.6 * blended_score + 0.4 * signals->verbalized_confidence;
        }
        /* If verbalized_confidence >= blended_score, trust blended (ignore optimistic model report)
         */
    }

    /* Clamp final score to [0, 1] */
    if (score > 1.0)
        score = 1.0;
    if (score < 0.0)
        score = 0.0;

    result->confidence = score;
    result->level = hu_confidence_level_from_score(score);

    switch (result->level) {
    case HU_CONFIDENCE_HIGH:
        result->recommendation = "answer";
        result->hedge_prefix = NULL;
        result->hedge_prefix_len = 0;
        break;
    case HU_CONFIDENCE_MEDIUM:
        result->recommendation = "hedge";
        result->hedge_prefix = hu_strndup(alloc, "Based on what I know, ", 23);
        result->hedge_prefix_len = result->hedge_prefix ? 23 : 0;
        break;
    case HU_CONFIDENCE_LOW:
        result->recommendation = "clarify";
        result->hedge_prefix = NULL;
        result->hedge_prefix_len = 0;
        break;
    case HU_CONFIDENCE_VERY_LOW:
        result->recommendation = "refuse";
        result->hedge_prefix = NULL;
        result->hedge_prefix_len = 0;
        break;
    }

    return HU_OK;
}

void hu_uncertainty_result_free(hu_allocator_t *alloc, hu_uncertainty_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->hedge_prefix) {
        hu_str_free(alloc, result->hedge_prefix);
        result->hedge_prefix = NULL;
        result->hedge_prefix_len = 0;
    }
}

static bool match_prefix_ci(const char *query, size_t query_len, const char *prefix) {
    size_t plen = strlen(prefix);
    size_t i = 0;
    while (i < query_len && isspace((unsigned char)query[i]))
        i++;
    if (query_len - i < plen)
        return false;
    for (size_t j = 0; j < plen; j++) {
        if (tolower((unsigned char)query[i + j]) != (unsigned char)prefix[j])
            return false;
    }
    return true;
}

static bool contains_phrase_ci(const char *text, size_t text_len, const char *phrase) {
    size_t plen = strlen(phrase);
    if (text_len < plen)
        return false;
    for (size_t i = 0; i <= text_len - plen; i++) {
        bool match = true;
        for (size_t j = 0; j < plen; j++) {
            if (tolower((unsigned char)text[i + j]) != (unsigned char)phrase[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            /* Check word boundary - phrase should not be mid-word */
            if ((i == 0 || isspace((unsigned char)text[i - 1])) &&
                (i + plen >= text_len || isspace((unsigned char)text[i + plen]) ||
                 !isalnum((unsigned char)text[i + plen])))
                return true;
        }
    }
    return false;
}

hu_error_t hu_uncertainty_extract_signals(const char *response, size_t response_len,
                                          const char *query, size_t query_len,
                                          size_t tool_results_count, size_t memory_results_count,
                                          hu_uncertainty_signals_t *signals) {
    if (!signals)
        return HU_ERR_INVALID_ARGUMENT;

    memset(signals, 0, sizeof(*signals));
    signals->tool_results_count = tool_results_count;
    signals->memory_results_count = memory_results_count;

    /* Hedging language */
    const char *hedges[] = {"i think",  "i believe", "possibly", "might",    "perhaps",
                            "not sure", "it seems",  "may be",   "could be", "unclear"};
    for (size_t i = 0; i < sizeof(hedges) / sizeof(hedges[0]); i++) {
        if (contains_phrase_ci(response, response_len, hedges[i])) {
            signals->has_hedging_language = true;
            break;
        }
    }

    /* Citations */
    const char *citations[] = {"according to", "based on", "from memory", "i recall",
                               "you mentioned"};
    for (size_t i = 0; i < sizeof(citations) / sizeof(citations[0]); i++) {
        if (contains_phrase_ci(response, response_len, citations[i])) {
            signals->has_citations = true;
            break;
        }
    }

    /* Factual query patterns */
    const char *factual_prefixes[] = {"what is",  "what are", "when did", "when was", "how many",
                                      "how much", "who is",   "who are",  "where is", "where are"};
    for (size_t i = 0; i < sizeof(factual_prefixes) / sizeof(factual_prefixes[0]); i++) {
        if (query && match_prefix_ci(query, query_len, factual_prefixes[i])) {
            signals->is_factual_query = true;
            break;
        }
    }

    /* retrieval_coverage: count query words found in response / total query words */
    if (query && query_len > 0) {
        size_t query_words = 0;
        size_t found_words = 0;
        const char *p = query;
        const char *end = query + query_len;
        while (p < end) {
            while (p < end && isspace((unsigned char)*p))
                p++;
            if (p >= end)
                break;
            const char *word_start = p;
            while (p < end && !isspace((unsigned char)*p) && *p != '\0')
                p++;
            size_t wlen = (size_t)(p - word_start);
            if (wlen > 1) { /* skip single chars */
                query_words++;
                if (response && response_len >= wlen) {
                    for (size_t j = 0; j <= response_len - wlen; j++) {
                        bool match = true;
                        for (size_t k = 0; k < wlen; k++) {
                            if (tolower((unsigned char)response[j + k]) !=
                                tolower((unsigned char)word_start[k])) {
                                match = false;
                                break;
                            }
                        }
                        if (match) {
                            found_words++;
                            break;
                        }
                    }
                }
            }
        }
        signals->retrieval_coverage =
            query_words > 0 ? (double)found_words / (double)query_words : 1.0;
        if (signals->retrieval_coverage > 1.0)
            signals->retrieval_coverage = 1.0;
    } else {
        signals->retrieval_coverage = 1.0;
    }

    /* response_length_ratio: response_len / (query_len * 3) clamped to [0, 1] */
    if (query_len > 0) {
        double expected = (double)query_len * 3.0;
        double ratio = expected > 0 ? (double)response_len / expected : 0.0;
        if (ratio > 1.0)
            ratio = 1.0;
        if (ratio < 0.0)
            ratio = 0.0;
        signals->response_length_ratio = ratio;
    } else {
        signals->response_length_ratio = 0.5;
    }

    return HU_OK;
}

hu_confidence_level_t hu_confidence_level_from_score(double score) {
    if (score >= 0.8)
        return HU_CONFIDENCE_HIGH;
    if (score >= 0.5)
        return HU_CONFIDENCE_MEDIUM;
    if (score >= 0.3)
        return HU_CONFIDENCE_LOW;
    return HU_CONFIDENCE_VERY_LOW;
}

const char *hu_confidence_level_str(hu_confidence_level_t level) {
    switch (level) {
    case HU_CONFIDENCE_HIGH:
        return "high";
    case HU_CONFIDENCE_MEDIUM:
        return "medium";
    case HU_CONFIDENCE_LOW:
        return "low";
    case HU_CONFIDENCE_VERY_LOW:
        return "very_low";
    default:
        return "unknown";
    }
}
