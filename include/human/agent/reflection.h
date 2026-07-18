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
    char *feedback;
    size_t feedback_len;
    /* Structured rubric scores (0.0–1.0, -1.0 = not scored) */
    double accuracy;     /* factual correctness */
    double relevance;    /* addresses the user's question */
    double tone;         /* appropriate tone/style */
    double completeness; /* comprehensive coverage */
    double conciseness;  /* not verbose or padded */
} hu_reflection_result_t;

/* Run rule-based self-evaluation on a response.
 * This is fast and does not require an LLM call. */
hu_reflection_quality_t hu_reflection_evaluate(const char *user_query, size_t user_query_len,
                                               const char *response, size_t response_len,
                                               const hu_reflection_config_t *config);

/* Build a self-critique prompt for LLM-based evaluation.
 * Caller owns the returned string. */
/* Retry-context instruction appended to history after a NEEDS_RETRY
 * verdict. A REWRITE directive — deliberately free of judge vocabulary
 * (Evaluate/Score/NEEDS_RETRY): the 2026-07-12 incident showed the retry
 * generation answers whatever the last user message asks, and when that
 * message was the judge prompt the "reply" was an evaluation, which got
 * sent to a real contact. Caller owns *out_prompt (len+1 bytes). */
hu_error_t hu_reflection_build_retry_prompt(hu_allocator_t *alloc, char **out_prompt,
                                            size_t *out_prompt_len);

hu_error_t hu_reflection_build_critique_prompt(hu_allocator_t *alloc, const char *user_query,
                                               size_t user_query_len, const char *response,
                                               size_t response_len, char **out_prompt,
                                               size_t *out_prompt_len);

void hu_reflection_result_free(hu_allocator_t *alloc, hu_reflection_result_t *result);

/* Structured LLM evaluation: JSON rubric + quality. On provider/parse failure,
 * fills `out` from the heuristic evaluator (same as hu_reflection_evaluate).
 * When non-NULL, `out->feedback` is allocated with the system allocator; free with
 * `hu_allocator_t a = hu_system_allocator(); hu_reflection_result_free(&a, out)`. */
hu_error_t hu_reflection_evaluate_structured(hu_allocator_t *alloc, hu_provider_t *provider,
                                             const char *model, size_t model_len,
                                             const char *user_query, size_t user_query_len,
                                             const char *response, size_t response_len,
                                             hu_reflection_result_t *out);

/* LLM-driven evaluation: uses structured evaluator internally; returns quality only. */
hu_reflection_quality_t hu_reflection_evaluate_llm(hu_allocator_t *alloc, hu_provider_t *provider,
                                                   const char *user_query, size_t user_query_len,
                                                   const char *response, size_t response_len,
                                                   hu_reflection_quality_t heuristic_quality);

#endif /* HU_AGENT_REFLECTION_H */
