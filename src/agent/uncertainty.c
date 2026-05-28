#include "human/agent/uncertainty.h"
#include "human/core/string.h"
#include "human/persona.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Task 5: temporal-aware MEDIUM hedges — selected when a grounded fact's
 * confidence has materially decayed with age (signals.has_temporal_decay).
 * Declared above hu_uncertainty_evaluate so the evaluator can route to it;
 * the non-temporal default banks live with hu_uncertainty_pick_hedge below. */
static const char *const k_default_hedges_medium_temporal[] = {
    "I think — though it's been a while — ",
    "Going from older memory: ", "Pretty sure, but the info's a bit stale — "};

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

    /* Real-signal score: grounded_confidence (no penalty here — applied post-blend) */
    double real_signal_score = signals->grounded_confidence;
    if (real_signal_score < 0.0)
        real_signal_score = 0.0;
    if (real_signal_score > 1.0)
        real_signal_score = 1.0;

    /* Soft blend: interpolate between heuristic and real signal scores */
    double blended_score =
        (1.0 - evidence_weight) * heuristic_score + evidence_weight * real_signal_score;

    /* Contradiction penalty applies AFTER blend, regardless of evidence weight */
    if (signals->contradiction_present)
        blended_score -= 0.15;

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
    case HU_CONFIDENCE_MEDIUM: {
        result->recommendation = "hedge";
        /* Task 5: when the fact's confidence has decayed with age, surface the
         * staleness with a temporal hedge; otherwise a generic MEDIUM hedge. */
        const char *hedge = "Based on what I know, ";
        if (signals->has_temporal_decay) {
            size_t n = sizeof(k_default_hedges_medium_temporal) /
                       sizeof(k_default_hedges_medium_temporal[0]);
            hedge = k_default_hedges_medium_temporal[(size_t)rand() % n];
        }
        size_t hlen = strlen(hedge);
        result->hedge_prefix = hu_strndup(alloc, hedge, hlen);
        result->hedge_prefix_len = result->hedge_prefix ? hlen : 0;
        break;
    }
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

/* Tail-anchored [conf=0.X] parser. Modifies response in place by
 * truncating at tag start; updates *response_len. Per
 * substring-classifier-pitfalls.md, requires bracket boundaries. */
bool hu_uncertainty_strip_verbalized(char *response, size_t *response_len, double *out_conf) {
    if (!response || !response_len || *response_len < 10)
        return false;

    size_t end = *response_len;
    while (end > 0 && isspace((unsigned char)response[end - 1]))
        end--;
    if (end == 0 || response[end - 1] != ']')
        return false;

    size_t cap = (end > 32) ? end - 32 : 0;
    size_t open_pos = end;
    for (size_t i = end; i > cap; i--) {
        if (response[i - 1] == '[') {
            open_pos = i - 1;
            break;
        }
    }
    if (open_pos >= end)
        return false;

    if (end - open_pos < 8)
        return false;
    if (strncmp(response + open_pos, "[conf=", 6) != 0)
        return false;

    char buf[16];
    size_t num_len = end - 1 - (open_pos + 6);
    if (num_len == 0 || num_len >= sizeof(buf))
        return false;
    memcpy(buf, response + open_pos + 6, num_len);
    buf[num_len] = '\0';

    char *endptr = NULL;
    double parsed = strtod(buf, &endptr);
    if (endptr == buf)
        return false;
    if (parsed < 0.0 || parsed > 1.0)
        return false;

    while (open_pos > 0 && isspace((unsigned char)response[open_pos - 1]))
        open_pos--;
    *response_len = open_pos;
    if (out_conf)
        *out_conf = parsed;
    return true;
}

/* Task 4: Default hedge phrase banks per confidence level */
static const char *const k_default_hedges_high[] = {""};
static const char *const k_default_hedges_medium[] = {"I'm pretty sure — ",
                                                      "Best read I have: ", "Going from memory, "};
static const char *const k_default_hedges_low[] = {"I'm not certain, but ", "Could be off here — ",
                                                   "Worth double-checking, but "};
static const char *const k_default_hedges_very_low[] = {"I don't think I know this well enough — ",
                                                        "Honestly, I'm guessing — ",
                                                        "Not confident on this: "};

static const struct {
    const char *const *phrases;
    size_t count;
} k_default_banks[4] = {
    {k_default_hedges_high, 1},
    {k_default_hedges_medium, 3},
    {k_default_hedges_low, 3},
    {k_default_hedges_very_low, 3},
};

const char *hu_uncertainty_pick_hedge(hu_confidence_level_t level,
                                      const struct hu_persona_overlay *overlay) {
    if (level < 0 || level >= 4)
        return "";

    if (overlay && overlay->hedge_phrases[level] && overlay->hedge_phrase_counts[level] > 0) {
        size_t idx = (size_t)rand() % overlay->hedge_phrase_counts[level];
        return overlay->hedge_phrases[level][idx];
    }

    size_t idx = (size_t)rand() % k_default_banks[level].count;
    return k_default_banks[level].phrases[idx];
}

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>
#include <stdio.h>

static const char *const k_uncertainty_migrations =
    "CREATE TABLE IF NOT EXISTS uncertainty_evaluations ("
    "    eval_id TEXT PRIMARY KEY,"
    "    turn_id TEXT NOT NULL,"
    "    channel TEXT NOT NULL,"
    "    query_text TEXT,"
    "    response_text TEXT,"
    "    stated_confidence REAL NOT NULL,"
    "    confidence_level TEXT NOT NULL,"
    "    hedge_phrase_used TEXT,"
    "    signals_json TEXT NOT NULL,"
    "    outcome_label TEXT,"
    "    outcome_source TEXT,"
    "    outcome_recorded_at_ms INTEGER,"
    "    created_at_ms INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_uncertainty_recent"
    "  ON uncertainty_evaluations(created_at_ms DESC);"
    "CREATE INDEX IF NOT EXISTS idx_uncertainty_unlabeled"
    "  ON uncertainty_evaluations(outcome_label, created_at_ms DESC)"
    "  WHERE outcome_label IS NULL;";

hu_error_t hu_uncertainty_storage_migrate(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;

    char *errmsg = NULL;
    int rc = sqlite3_exec(db, k_uncertainty_migrations, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        if (errmsg)
            sqlite3_free(errmsg);
        return HU_ERR_IO;
    }
    return HU_OK;
}

hu_error_t hu_uncertainty_log(sqlite3 *db, const hu_uncertainty_log_entry_t *entry) {
    if (!db || !entry)
        return HU_ERR_INVALID_ARGUMENT;

    char eval_id[128];
    snprintf(eval_id, sizeof(eval_id), "%lld_%s", (long long)entry->created_at_ms,
             entry->turn_id ? entry->turn_id : "unknown");

    const char *level_str = hu_confidence_level_str(entry->level);

    static const char *const insert_sql =
        "INSERT INTO uncertainty_evaluations("
        "  eval_id, turn_id, channel, query_text, response_text,"
        "  stated_confidence, confidence_level, hedge_phrase_used,"
        "  signals_json, created_at_ms"
        ") VALUES (?,?,?,?,?,?,?,?,?,?);";

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, insert_sql, -1, &st, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(st, 1, eval_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, entry->turn_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, entry->channel, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, entry->query_text, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 5, entry->response_text, -1, SQLITE_STATIC);
    sqlite3_bind_double(st, 6, entry->stated_confidence);
    sqlite3_bind_text(st, 7, level_str, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 8, entry->hedge_phrase_used, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 9, entry->signals_json, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 10, entry->created_at_ms);

    rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE)
        return HU_ERR_IO;
    return HU_OK;
}

hu_error_t hu_uncertainty_set_outcome(sqlite3 *db, const char *turn_id, const char *label,
                                      const char *source, int64_t recorded_at_ms) {
    if (!db || !turn_id || !label || !source)
        return HU_ERR_INVALID_ARGUMENT;

    static const char *const update_sql =
        "UPDATE uncertainty_evaluations"
        " SET outcome_label=?, outcome_source=?, outcome_recorded_at_ms=?"
        " WHERE turn_id=?;";

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, update_sql, -1, &st, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(st, 1, label, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, source, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, recorded_at_ms);
    sqlite3_bind_text(st, 4, turn_id, -1, SQLITE_STATIC);

    rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE)
        return HU_ERR_IO;
    return HU_OK;
}

#endif
