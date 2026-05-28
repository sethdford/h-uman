#ifndef HU_AGENT_UNCERTAINTY_H
#define HU_AGENT_UNCERTAINTY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Uncertainty Quantification — estimates agent confidence and calibrates
 * behavior accordingly. Uses retrieval coverage, response consistency,
 * and heuristic signals to produce a confidence score.
 */

typedef enum hu_confidence_level {
    HU_CONFIDENCE_HIGH = 0, /* >= 0.8: answer directly */
    HU_CONFIDENCE_MEDIUM,   /* 0.5-0.8: answer with hedging */
    HU_CONFIDENCE_LOW,      /* 0.3-0.5: ask clarifying question */
    HU_CONFIDENCE_VERY_LOW, /* < 0.3: refuse or say "I don't know" */
} hu_confidence_level_t;

/* Forward declaration for hu_uncertainty_pick_hedge */
struct hu_persona_overlay;

typedef struct hu_uncertainty_signals {
    double retrieval_coverage;    /* 0-1: fraction of query terms found in memory results */
    double response_length_ratio; /* ratio of response length to expected length */
    bool has_hedging_language;    /* "I think", "possibly", "might" detected */
    bool has_citations;           /* references memory or tool results */
    size_t tool_results_count;    /* number of tool results grounding the response */
    size_t memory_results_count;  /* number of memory hits */
    bool is_factual_query;        /* query asks for facts vs opinions */

    /* NEW (Phase 1) */
    double grounded_confidence;   /* confidence from grounded facts, 0-1 */
    size_t fact_count;            /* number of relevant facts contributing to answer */
    double verbalized_confidence; /* model's self-reported confidence from tag, 0-1 */
    bool has_verbalized;          /* [conf=X] tag was present in response */
    bool contradiction_present;   /* contradictory facts found in evidence set */
    bool has_temporal_decay; /* meaningful age difference between stored and effective confidence */
} hu_uncertainty_signals_t;

typedef struct hu_uncertainty_result {
    double confidence; /* 0.0-1.0 overall confidence score */
    hu_confidence_level_t level;
    const char *recommendation; /* static string: "answer", "hedge", "clarify", "refuse" */
    char *hedge_prefix;         /* suggested hedge phrase, NULL if confident. Caller frees. */
    size_t hedge_prefix_len;
} hu_uncertainty_result_t;

/* Compute uncertainty from signals. Pure computation, no LLM calls. */
hu_error_t hu_uncertainty_evaluate(hu_allocator_t *alloc, const hu_uncertainty_signals_t *signals,
                                   hu_uncertainty_result_t *result);

void hu_uncertainty_result_free(hu_allocator_t *alloc, hu_uncertainty_result_t *result);

/* Extract uncertainty signals from a response string. */
hu_error_t hu_uncertainty_extract_signals(const char *response, size_t response_len,
                                          const char *query, size_t query_len,
                                          size_t tool_results_count, size_t memory_results_count,
                                          hu_uncertainty_signals_t *signals);

/* Map confidence score to level. */
hu_confidence_level_t hu_confidence_level_from_score(double score);

const char *hu_confidence_level_str(hu_confidence_level_t level);

/* Strip and parse tail-anchored [conf=0.X] tag from response.
 * Modifies response in place by truncating at tag start; updates *response_len.
 * Returns true if tag was found and parsed, false otherwise. */
bool hu_uncertainty_strip_verbalized(char *response, size_t *response_len, double *out_conf);

/* Pick a hedge phrase for the given confidence level, optionally from a persona overlay.
 * Returns empty string for HIGH confidence, a static or overlay phrase otherwise. */
const char *hu_uncertainty_pick_hedge(hu_confidence_level_t level,
                                      const struct hu_persona_overlay *overlay);

/* Phase 1 logging — ECE-ready uncertainty evaluation storage */
typedef struct hu_uncertainty_log_entry {
    const char *turn_id;
    const char *channel;
    const char *query_text;
    const char *response_text;
    double stated_confidence;
    hu_confidence_level_t level;
    const char *hedge_phrase_used;
    const char *signals_json;
    int64_t created_at_ms;
} hu_uncertainty_log_entry_t;

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

hu_error_t hu_uncertainty_storage_migrate(sqlite3 *db);
hu_error_t hu_uncertainty_log(sqlite3 *db, const hu_uncertainty_log_entry_t *entry);
hu_error_t hu_uncertainty_set_outcome(sqlite3 *db, const char *turn_id, const char *label,
                                      const char *source, int64_t recorded_at_ms);
#endif

#endif
