#ifndef HU_AGENT_REFLECTION_H
#define HU_AGENT_REFLECTION_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct hu_reflection_config {
    bool enabled;
    bool use_llm;            /* if true, ACCEPTABLE responses get LLM second opinion */
    int min_response_tokens; /* only reflect on responses >= this length; 0 = always */
    int max_retries;         /* max self-correction retries; 0 = no retry */
} hu_reflection_config_t;

typedef enum hu_reflection_quality {
    HU_QUALITY_GOOD,
    HU_QUALITY_ACCEPTABLE,
    HU_QUALITY_NEEDS_RETRY,
} hu_reflection_quality_t;

typedef struct hu_reflection_result {
    hu_reflection_quality_t quality;
    char *feedback; /* optional textual self-critique */
    size_t feedback_len;
} hu_reflection_result_t;

/* Run rule-based self-evaluation on a response.
 * This is fast and does not require an LLM call. */
hu_reflection_quality_t hu_reflection_evaluate(const char *user_query, size_t user_query_len,
                                               const char *response, size_t response_len,
                                               const hu_reflection_config_t *config);

/* Build a self-critique prompt for LLM-based evaluation.
 * Caller owns the returned string. */
hu_error_t hu_reflection_build_critique_prompt(hu_allocator_t *alloc, const char *user_query,
                                               size_t user_query_len, const char *response,
                                               size_t response_len, char **out_prompt,
                                               size_t *out_prompt_len);

void hu_reflection_result_free(hu_allocator_t *alloc, hu_reflection_result_t *result);

/* LLM-driven evaluation: sends the critique prompt to the provider and parses
 * the response for GOOD/ACCEPTABLE/NEEDS_RETRY keywords.
 * Falls back to heuristic_quality on provider failure. */
hu_reflection_quality_t hu_reflection_evaluate_llm(hu_allocator_t *alloc, hu_provider_t *provider,
                                                   const char *user_query, size_t user_query_len,
                                                   const char *response, size_t response_len,
                                                   hu_reflection_quality_t heuristic_quality);

#endif /* HU_AGENT_REFLECTION_H */
