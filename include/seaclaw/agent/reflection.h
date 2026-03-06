#ifndef SC_AGENT_REFLECTION_H
#define SC_AGENT_REFLECTION_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/provider.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct sc_reflection_config {
    bool enabled;
    int min_response_tokens; /* only reflect on responses >= this length; 0 = always */
    int max_retries;         /* max self-correction retries; 0 = no retry */
} sc_reflection_config_t;

typedef enum sc_reflection_quality {
    SC_QUALITY_GOOD,
    SC_QUALITY_ACCEPTABLE,
    SC_QUALITY_NEEDS_RETRY,
} sc_reflection_quality_t;

typedef struct sc_reflection_result {
    sc_reflection_quality_t quality;
    char *feedback;  /* optional textual self-critique */
    size_t feedback_len;
} sc_reflection_result_t;

/* Run rule-based self-evaluation on a response.
 * This is fast and does not require an LLM call. */
sc_reflection_quality_t sc_reflection_evaluate(const char *user_query, size_t user_query_len,
                                               const char *response, size_t response_len,
                                               const sc_reflection_config_t *config);

/* Build a self-critique prompt for LLM-based evaluation.
 * Caller owns the returned string. */
sc_error_t sc_reflection_build_critique_prompt(sc_allocator_t *alloc, const char *user_query,
                                              size_t user_query_len, const char *response,
                                              size_t response_len, char **out_prompt,
                                              size_t *out_prompt_len);

void sc_reflection_result_free(sc_allocator_t *alloc, sc_reflection_result_t *result);

#endif /* SC_AGENT_REFLECTION_H */
